/**
 * @file signal_protocol.c
 * @brief 协议注册表、参数解析与编码器实现
 *
 * 时间基准换算(核心):
 *   硬件一个周期的计数 = (ARR+1) * (PSC+1)
 *   因此对给定周期 T(us):  counts = timer_hz * T / 1e6, 再解出 PSC/ARR.
 *   DShot 例外: 位周期固定, 1 个位槽 = 1 个定时器周期, 由 DMA 逐位改 CCR.
 */
#include "signal_protocol.h"
#include "app_config.h"

#include <string.h>

/* ========================== 时间换算 ========================== */

/** 周期(us) 转频率(Hz) */
#define US_TO_HZ(us) ((us) == 0U ? 0U : (1000000UL / (us)))
/** 频率(Hz) 转周期(us) */
#define HZ_TO_US(hz) ((hz) == 0U ? 0U : (1000000UL / (hz)))

/*
 * DShot 位周期在 72MHz 定时器下的计数:
 *   位周期 = 1e9 / 比特率 (ns)
 *   DShot150: 6666.7ns -> 480.0 tick
 *   DShot300: 3333.3ns -> 240.0 tick
 *   DShot600: 1666.7ns -> 120.0 tick
 * 三者的 tick 都能被 8 整除, 因此 "0" 位(3/8 位周期) 与 "1" 位(6/8 位周期)
 * 都能整数表达. 这是 DShot 能在 72MHz 器件上跑准的前提.
 */
#define DSHOT150_TICK_PER_BIT 480UL
#define DSHOT300_TICK_PER_BIT 240UL
#define DSHOT600_TICK_PER_BIT 120UL

/**
 * DShot 帧结构.
 * 标准帧为 11 位油门 + 1 位遥测 + 4 位校验 = 16 个数据位, 之后是一个停止位.
 *
 * 关键在于帧的重复率: DShot 不是"一帧紧接着一帧"发送, 而是在固定的节流周期
 * (looptime, 见 DSHOT_LOOPTIME_US, 标准 125us = 8kHz)内发完一帧, 其余时间
 * 保持低电平. 因此需要在数据位后填充足够的空闲位槽, 使
 *    位槽总数 = 节流周期 / 位周期
 * 例如 72MHz + 125us 下: DShot600(120 tick) = 75 槽, DShot300(240) = 37 槽,
 * DShot150(480) = 18 槽.
 *
 * 若按"16 位 + 紧接重发"处理, 帧率会高达数十 kHz, 电调会判为非法信号.
 * 因此描述符里的 frame_slots 留 0, 由 BuildPlan 运行时按 looptime 推导.
 */
#define DSHOT_DATA_BITS 16U

/** 位槽数下限: 至少要容纳 16 个数据位与 1 个停止位 */
#define DSHOT_MIN_SLOTS (DSHOT_DATA_BITS + 1U)

/* DShot 油门特殊值: 0 = 停机; 1..47 为保留指令区间 */
#define DSHOT_THROTTLE_STOP 0U
#define DSHOT_THROTTLE_MIN  48U
#define DSHOT_THROTTLE_MAX  2047U

/* ========================== 协议注册表 ========================== */

/**
 * @note  新增协议只需追加一条描述符, 无需改动本文件其它逻辑.
 *
 * 已注册协议与其参数含义(kind 决定界面单位):
 *   PWM        : 频率 50..20000Hz, 占空比 0.0..100.0% (0.1% 步进)
 *   OneShot125 : 单位时间 125us, 帧周期 250us, 脉宽 1..250us
 *   OneShot42  : 单位时间 42us,  帧周期 84us,  脉宽 1..84us
 *   MultiShot  : 单位时间 5us,   帧周期 30us,  脉宽 1..25us
 *   DShot150/300/600 : 11 位油门(0 停机 或 48..2047), 速率由位周期决定
 */
static const SignalProtocol s_protocols[] = {
  /* --- 1: 标准 PWM --- */
  {
    .id              = 1U,
    .name            = "PWM",
    .detail          = "Std PWM",
    .wave            = SIG_WAVE_PWM,
    .tick_per_unit   = 0U,
    .frame_bits      = 0U,
    .frame_slots     = 0U,
    .fixed_period_us = 0U,
    .params = {
      {SIG_PARAM_FREQ_HZ,       50U, 20000U, 1000U, 50U, 1U},
      {SIG_PARAM_DUTY_PCT_X10,   0U,  1000U,  500U, 10U, 0U},
    },
    .param_count = 2U,
  },
  /* --- 2: OneShot125 --- */
  {
    .id              = 2U,
    .name            = "OneShot125",
    .detail          = "125us unit",
    .wave            = SIG_WAVE_ONESHOT,
    .tick_per_unit   = 0U,
    .frame_bits      = 0U,
    .frame_slots     = 0U,
    .fixed_period_us = 250U,
    .params = {
      {SIG_PARAM_PULSE_US, 1U, 250U, 125U, 1U, 0U},
      {SIG_PARAM_NONE,     0U,   0U,   0U, 0U, 0U},
    },
    .param_count = 1U,
  },
  /* --- 3: OneShot42 --- */
  {
    .id              = 3U,
    .name            = "OneShot42",
    .detail          = "42us unit",
    .wave            = SIG_WAVE_ONESHOT,
    .tick_per_unit   = 0U,
    .frame_bits      = 0U,
    .frame_slots     = 0U,
    .fixed_period_us = 84U,
    .params = {
      {SIG_PARAM_PULSE_US, 1U, 84U, 42U, 1U, 0U},
      {SIG_PARAM_NONE,     0U,  0U,  0U, 0U, 0U},
    },
    .param_count = 1U,
  },
  /* --- 4: MultiShot --- */
  {
    .id              = 4U,
    .name            = "MultiShot",
    .detail          = "5us unit",
    .wave            = SIG_WAVE_ONESHOT,
    .tick_per_unit   = 0U,
    .frame_bits      = 0U,
    .frame_slots     = 0U,
    .fixed_period_us = 30U,
    .params = {
      {SIG_PARAM_PULSE_US, 1U, 25U, 12U, 1U, 0U},
      {SIG_PARAM_NONE,     0U,  0U,  0U, 0U, 0U},
    },
    .param_count = 1U,
  },
  /* --- 5: DShot150 --- */
  {
    .id              = 5U,
    .name            = "DShot150",
    .detail          = "150kbps",
    .wave            = SIG_WAVE_DSHOT,
    .tick_per_unit   = DSHOT150_TICK_PER_BIT,
    .frame_bits      = DSHOT_DATA_BITS,
    .frame_slots     = 0U, /* 由 looptime 推导 */
    .fixed_period_us = 0U,
    .params = {
      {SIG_PARAM_THROTTLE, DSHOT_THROTTLE_MIN, DSHOT_THROTTLE_MAX, 0U, 1U, 0U},
      {SIG_PARAM_NONE,     0U, 0U, 0U, 0U, 0U},
    },
    .param_count = 1U,
  },
  /* --- 6: DShot300 --- */
  {
    .id              = 6U,
    .name            = "DShot300",
    .detail          = "300kbps",
    .wave            = SIG_WAVE_DSHOT,
    .tick_per_unit   = DSHOT300_TICK_PER_BIT,
    .frame_bits      = DSHOT_DATA_BITS,
    .frame_slots     = 0U, /* 由 looptime 推导 */
    .fixed_period_us = 0U,
    .params = {
      {SIG_PARAM_THROTTLE, DSHOT_THROTTLE_MIN, DSHOT_THROTTLE_MAX, 0U, 1U, 0U},
      {SIG_PARAM_NONE,     0U, 0U, 0U, 0U, 0U},
    },
    .param_count = 1U,
  },
  /* --- 7: DShot600 --- */
  {
    .id              = 7U,
    .name            = "DShot600",
    .detail          = "600kbps",
    .wave            = SIG_WAVE_DSHOT,
    .tick_per_unit   = DSHOT600_TICK_PER_BIT,
    .frame_bits      = DSHOT_DATA_BITS,
    .frame_slots     = 0U, /* 由 looptime 推导 */
    .fixed_period_us = 0U,
    .params = {
      {SIG_PARAM_THROTTLE, DSHOT_THROTTLE_MIN, DSHOT_THROTTLE_MAX, 0U, 1U, 0U},
      {SIG_PARAM_NONE,     0U, 0U, 0U, 0U, 0U},
    },
    .param_count = 1U,
  },
  /*
   * 扩展点: 在此追加描述符即可注册新协议.
   *
   * DShot1200 为何不注册: 位周期 = 1e9/1200000 = 833.3ns, 72MHz 下为 60.0 tick,
   * 但 "0" 位需 3/8 位周期 = 22.5 tick, 非整数, 无法用 16 位定时器精确表达.
   * 该情况会被 SignalProtocol_Probe 以 tick_per_unit % 8 != 0 拒绝.
   * 若将来提高定时器时钟(或换更高主频器件), 只需在此补一条描述符.
   */
};

#define PROTOCOL_COUNT ((uint8_t)(sizeof(s_protocols) / sizeof(s_protocols[0])))

/* ========================== 基础访问 ========================== */

/* 下标与 SignalStatus 负值的绝对值一一对应: [0] 对应 SIG_OK, [1] 对应 -1 */
static const char *const s_status_text[] = {
  "OK",        /* 0 */
  "NULL",      /* -1 */
  "NOID",      /* -2 */
  "TICKFAST",  /* -3 */
  "PERLONG",   /* -4 */
  "PERSHORT",  /* -5 */
  "FRAMELONG", /* -6 */
  "UNSUP",     /* -7 */
  "RANGE",     /* -8 */
  "NOPARAM",   /* -9 */
};

const char *SignalStatus_Text(SignalStatus status)
{
  uint8_t index;

  if (status == SIG_OK)
  {
    return s_status_text[0];
  }

  index = (uint8_t)(0 - (int32_t)status);
  if (index >= (uint8_t)(sizeof(s_status_text) / sizeof(s_status_text[0])))
  {
    return "UNKNOWN";
  }

  return s_status_text[index];
}

uint8_t SignalProtocol_Count(void)
{
  return PROTOCOL_COUNT;
}

const SignalProtocol *SignalProtocol_At(uint8_t index)
{
  if (index >= PROTOCOL_COUNT)
  {
    return NULL;
  }
  return &s_protocols[index];
}

const SignalProtocol *SignalProtocol_FindById(uint8_t id)
{
  uint8_t i;

  for (i = 0U; i < PROTOCOL_COUNT; i++)
  {
    if (s_protocols[i].id == id)
    {
      return &s_protocols[i];
    }
  }

  return NULL;
}

const SignalParam *SignalProtocol_ParamAt(const SignalProtocol *proto, uint8_t index)
{
  if ((proto == NULL) || (index >= proto->param_count) || (index >= SIG_MAX_PARAMS))
  {
    return NULL;
  }
  return &proto->params[index];
}

/* ========================== 参数操作 ========================== */

uint32_t SignalProtocol_ClampParam(const SignalProtocol *proto, uint8_t index, uint32_t value)
{
  const SignalParam *param = SignalProtocol_ParamAt(proto, index);

  if (param == NULL)
  {
    return 0U;
  }

  if (value < param->min)
  {
    return param->min;
  }
  if (value > param->max)
  {
    return param->max;
  }
  return value;
}

uint32_t SignalProtocol_DefaultParam(const SignalProtocol *proto, uint8_t index)
{
  const SignalParam *param = SignalProtocol_ParamAt(proto, index);

  if (param == NULL)
  {
    return 0U;
  }
  return param->default_value;
}

uint32_t SignalProtocol_StepParam(const SignalProtocol *proto, uint8_t index,
                                  uint32_t current, int8_t direction)
{
  const SignalParam *param = SignalProtocol_ParamAt(proto, index);
  uint32_t           step;
  int64_t            next;

  if ((param == NULL) || (direction == 0))
  {
    return current;
  }

  current = SignalProtocol_ClampParam(proto, index, current);

  step = (param->step == 0U) ? 1U : param->step;

  /* 对数手感: 步进随当前值增大, 低端精细、高端快速, 便于跨越宽量程 */
  if (param->log_step != 0U)
  {
    step += (current / 16U);
  }

  next = (int64_t)current + ((direction > 0) ? (int64_t)step : -(int64_t)step);
  if (next < 0)
  {
    next = 0;
  }

  return SignalProtocol_ClampParam(proto, index, (uint32_t)next);
}

/* ========================== 能力判断 ========================== */

SignalStatus SignalProtocol_Probe(const SignalProtocol *proto, uint32_t timer_hz)
{
  if (proto == NULL)
  {
    return SIG_ERR_NULL_HANDLE;
  }

  if (timer_hz == 0U)
  {
    return SIG_ERR_UNSUPPORTED;
  }

  if (proto->wave == SIG_WAVE_DSHOT)
  {
    if (proto->frame_bits == 0U)
    {
      return SIG_ERR_UNSUPPORTED;
    }
    if (proto->frame_bits >= DSHOT_MIN_SLOTS)
    {
      /* 数据位必须少于最小区间, 否则没有停止位空间 */
      return SIG_ERR_UNSUPPORTED;
    }

    /*
     * 核心能力约束: 位周期既要能装进 16 位计数, 又要能被 8 整除,
     * 才能把 3/8 与 6/8 位周期表达为整数比较值. 否则位宽失真,
     * 电调解码必然失败 —— 这类协议必须在切换前就拒绝, 而不是先切再报错.
     */
    if (proto->tick_per_unit > APP_TIMER_COUNTS_MAX)
    {
      return SIG_ERR_PERIOD_TOO_LONG;
    }
    if (proto->tick_per_unit < 8U)
    {
      return SIG_ERR_TICK_TOO_FAST;
    }
    if ((proto->tick_per_unit % 8U) != 0U)
    {
      return SIG_ERR_TICK_TOO_FAST;
    }

    /*
     * 位周期必须能整除定时器时钟, 否则实际位率与协议标称速率不符.
     * 例如 72MHz / 480tick = 150kbps 精确成立; 若不能整除, 位宽存在
     * 累积误差, 长帧会漂移到电调解码容限之外.
     */
    if ((timer_hz % proto->tick_per_unit) != 0U)
    {
      return SIG_ERR_TICK_TOO_FAST;
    }
  }
  else if (proto->wave == SIG_WAVE_ONESHOT)
  {
    if (proto->fixed_period_us == 0U)
    {
      return SIG_ERR_UNSUPPORTED;
    }
    /* 周期计数必须可表达(硬件层会按需引入预分频) */
    if ((((uint64_t)timer_hz * proto->fixed_period_us) / 1000000ULL) > 0xFFFFFFFFULL)
    {
      return SIG_ERR_PERIOD_TOO_LONG;
    }
  }
  else if (proto->wave == SIG_WAVE_PWM)
  {
    if (proto->params[0].min == 0U)
    {
      return SIG_ERR_UNSUPPORTED;
    }
  }
  else
  {
    return SIG_ERR_UNSUPPORTED;
  }

  return SIG_OK;
}

/* ========================== 方案解析 ========================== */

/**
 * @brief 把所需计数换算为 PSC/ARR
 * @param counts 1 个周期需要的定时器计数(1 分频下)
 * @param psc    输出预分频寄存器值
 * @param arr    输出重载寄存器值
 * @note  硬件周期计数 = (ARR+1) * (PSC+1), 故 ARR = counts/(PSC+1) - 1.
 *        优先取最小预分频, 使占空比/脉宽分辨率尽量高.
 */
static SignalStatus ResolveTimebase(uint32_t counts, uint16_t *psc, uint16_t *arr)
{
  uint32_t prescaler;

  if ((psc == NULL) || (arr == NULL) || (counts == 0U))
  {
    return SIG_ERR_NULL_HANDLE;
  }

  if (counts > APP_TIMER_COUNTS_MAX)
  {
    prescaler = (counts + APP_TIMER_COUNTS_MAX - 1U) / APP_TIMER_COUNTS_MAX;
    if (prescaler > 65536U)
    {
      return SIG_ERR_PERIOD_TOO_LONG;
    }
    *psc = (uint16_t)(prescaler - 1U);
  }
  else
  {
    *psc = 0U;
  }

  *arr = (uint16_t)((counts / ((uint32_t)(*psc) + 1U)) - 1U);

  return SIG_OK;
}

/** 由 ARR/PSC 回算真实周期(us); 界面显示的是实际生效值而非期望值 */
static uint32_t PlanPeriodUs(uint16_t psc, uint16_t arr, uint32_t timer_hz)
{
  return (uint32_t)((((uint64_t)((uint32_t)arr + 1U) * ((uint32_t)psc + 1U)) *
                     1000000ULL) / (uint64_t)timer_hz);
}

SignalStatus SignalProtocol_BuildPlan(const SignalProtocol *proto,
                                     const uint32_t       *param_values,
                                     uint32_t              timer_hz,
                                     uint16_t              dma_capacity,
                                     SignalPlan           *out)
{
  SignalStatus status;
  uint32_t     period_us;

  if ((proto == NULL) || (param_values == NULL) || (out == NULL))
  {
    return SIG_ERR_NULL_HANDLE;
  }

  status = SignalProtocol_Probe(proto, timer_hz);
  if (status != SIG_OK)
  {
    return status;
  }

  memset(out, 0, sizeof(*out));
  out->proto      = proto;
  out->timer_hz   = timer_hz;
  out->frame_bits = proto->frame_bits;

  if (proto->wave == SIG_WAVE_DSHOT)
  {
    uint32_t slots;
    uint32_t throttle;

    /*
     * 位槽总数由节流周期推导: 数据位之后补足空闲位, 使帧重复率等于
     * 节流频率(DSHOT_LOOPTIME_US, 125us -> 8kHz).
     *   位槽数 = looptime_us * timer_hz / (位周期计数 * 1e6)
     */
    slots = (uint32_t)(((uint64_t)DSHOT_LOOPTIME_US * (uint64_t)timer_hz) /
                       ((uint64_t)proto->tick_per_unit * 1000000ULL));

    if (slots < DSHOT_MIN_SLOTS)
    {
      /* 节流周期连一帧都放不下: 该配置无法成立, 明确报错而不是截断成非法帧 */
      return SIG_ERR_PERIOD_TOO_SHORT;
    }
    if (slots > dma_capacity)
    {
      return SIG_ERR_FRAME_TOO_LONG;
    }
    if (slots > 0xFFFFU)
    {
      return SIG_ERR_FRAME_TOO_LONG;
    }

    /*
     * 1 个位槽 = 1 个定时器周期; 硬件周期为 (ARR+1) 个计数,
     * 因此 ARR 取 tick_per_unit-1 恰好得到位周期.
     */
    out->psc      = 0U;
    out->arr      = (uint16_t)(proto->tick_per_unit - 1U);
    out->ccr_low  = (uint16_t)((proto->tick_per_unit * 3U) / 8U);
    out->ccr_high = (uint16_t)((proto->tick_per_unit * 6U) / 8U);
    out->dma_len  = (uint16_t)slots;

    /* 油门 0 是合法的"停机", 单独放行; 1..47 为保留指令区间, 夹到下限 */
    throttle = param_values[0];
    if (throttle == DSHOT_THROTTLE_STOP)
    {
      out->param_values[0] = DSHOT_THROTTLE_STOP;
    }
    else
    {
      out->param_values[0] = SignalProtocol_ClampParam(proto, 0U, throttle);
    }

    /* 实际帧周期 = 位槽数 * 位周期 */
    period_us = (uint32_t)(((uint64_t)proto->tick_per_unit * (uint64_t)slots *
                            1000000ULL) / (uint64_t)timer_hz);
    if (period_us == 0U)
    {
      return SIG_ERR_PERIOD_TOO_SHORT;
    }
  }
  else
  {
    /* PWM 与 OneShot 共用 "ARR 定周期 + CCR 定高电平" 路径 */
    uint32_t counts;

    if (proto->wave == SIG_WAVE_PWM)
    {
      uint32_t freq = SignalProtocol_ClampParam(proto, 0U, param_values[0]);

      if (freq == 0U)
      {
        return SIG_ERR_RANGE;
      }
      period_us            = HZ_TO_US(freq);
      out->param_values[0] = freq;
    }
    else
    {
      period_us = proto->fixed_period_us;
      if (period_us == 0U)
      {
        return SIG_ERR_UNSUPPORTED;
      }
      out->param_values[0] = SignalProtocol_ClampParam(proto, 0U, param_values[0]);
    }

    out->param_values[1] = (proto->param_count > 1U)
                             ? SignalProtocol_ClampParam(proto, 1U, param_values[1])
                             : 0U;

    if (period_us == 0U)
    {
      return SIG_ERR_PERIOD_TOO_SHORT;
    }

    counts = (uint32_t)(((uint64_t)timer_hz * (uint64_t)period_us) / 1000000ULL);
    if (counts == 0U)
    {
      return SIG_ERR_PERIOD_TOO_SHORT;
    }

    status = ResolveTimebase(counts, &out->psc, &out->arr);
    if (status != SIG_OK)
    {
      return status;
    }

    if (proto->wave == SIG_WAVE_PWM)
    {
      uint32_t duty_x10 = out->param_values[1];
      uint32_t high;

      /* 一个周期共 (ARR+1) 个计数, 比较值合法范围 0..ARR */
      high = (uint32_t)(((uint64_t)duty_x10 * ((uint32_t)out->arr + 1U)) / 1000ULL);
      if (high > out->arr)
      {
        high = out->arr;
      }
      out->ccr = (uint16_t)high;
    }
    else
    {
      uint32_t prescaled_hz = timer_hz / ((uint32_t)out->psc + 1U);
      uint32_t pulse_us     = out->param_values[0];
      uint32_t high;

      if (prescaled_hz == 0U)
      {
        return SIG_ERR_UNSUPPORTED;
      }

      high = (uint32_t)(((uint64_t)prescaled_hz * pulse_us) / 1000000ULL);

      /*
       * 脉宽限幅: 必须严格小于一个周期, 保证每个周期都有可见的低电平间隔,
       * 否则接收端无法识别脉冲边界. 故上限取 ARR-1.
       */
      if (high >= out->arr)
      {
        high = ((uint32_t)out->arr > 0U) ? ((uint32_t)out->arr - 1U) : 0U;
      }
      if ((high == 0U) && (pulse_us > 0U))
      {
        high = 1U;
      }
      out->ccr = (uint16_t)high;
    }

    period_us = PlanPeriodUs(out->psc, out->arr, timer_hz);
  }

  out->period_us = period_us;
  out->actual_hz = US_TO_HZ(period_us);

  return SIG_OK;
}

/* ========================== DShot 编码 ========================== */

uint16_t SignalProtocol_DshotRawFrame(uint16_t throttle, uint8_t telemetry)
{
  uint16_t frame;

  if (throttle > DSHOT_THROTTLE_MAX)
  {
    throttle = DSHOT_THROTTLE_MAX;
  }

  frame = (uint16_t)(throttle & 0x07FFU); /* bit0..10: 11 位油门 */
  if (telemetry != 0U)
  {
    frame |= 0x0800U; /* bit11: 遥测请求 */
  }
  return frame;
}

uint8_t SignalProtocol_DshotCrc(uint16_t value)
{
  /*
   * DShot 的校验位是对帧各半字节异或后取低 4 位, 不是多项式 CRC:
   *   校验 = (帧 ^ (帧>>4) ^ (帧>>8)) & 0x0F
   * 帧为 16 位(11 位油门 + 1 位遥测 + 4 位校验), 其中校验字段本身为 0,
   * 因此等价于对 12 位内容的高/中/低三个半字节异或.
   *
   * 注意: 这与 x^4+x+1 的多项式 CRC 结果不同. 用错算法会让所有帧校验失败,
   * 电调静默丢弃, 表现为电机完全不响应.
   */
  uint16_t frame = (uint16_t)(value & 0x0FFFU); /* 12 位内容 */

  return (uint8_t)((frame ^ (frame >> 4U) ^ (frame >> 8U)) & 0x0FU);
}

SignalStatus SignalProtocol_EncodeDshotFrame(const SignalPlan *plan,
                                             uint16_t          throttle,
                                             uint8_t           telemetry,
                                             uint16_t         *buf,
                                             uint16_t          capacity)
{
  uint16_t frame;
  uint16_t full;
  uint16_t i;

  if ((plan == NULL) || (plan->proto == NULL) || (buf == NULL))
  {
    return SIG_ERR_NULL_HANDLE;
  }
  if (plan->proto->wave != SIG_WAVE_DSHOT)
  {
    return SIG_ERR_UNSUPPORTED;
  }
  if ((plan->dma_len == 0U) || (capacity < plan->dma_len))
  {
    return SIG_ERR_FRAME_TOO_LONG;
  }
  if (plan->frame_bits == 0U)
  {
    return SIG_ERR_UNSUPPORTED;
  }

  frame = SignalProtocol_DshotRawFrame(throttle, telemetry);
  full  = (uint16_t)((frame << 4U) | (uint16_t)SignalProtocol_DshotCrc(frame));

  for (i = 0U; i < plan->dma_len; i++)
  {
    if (i < plan->frame_bits)
    {
      /* MSB 先发: 第 i 个位槽对应 full 的第 (frame_bits-1-i) 位 */
      uint8_t bitval = (uint8_t)((full >> (uint8_t)(plan->frame_bits - 1U - i)) & 0x01U);

      buf[i] = (bitval != 0U) ? plan->ccr_high : plan->ccr_low;
    }
    else
    {
      /* 停止位与后续空闲位: 恒定低电平, 使帧重复率等于节流频率 */
      buf[i] = 0U;
    }
  }

  return SIG_OK;
}
