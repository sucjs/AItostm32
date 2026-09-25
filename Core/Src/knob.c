/**
 * @file knob.c
 * @brief 电位器滤波与归一化实现
 * @note  本文件不包含任何 STM32 头文件: 原始样本由 Knob_AdcSample() 钩子提供,
 *        目标端在 adc.c 里实现它, 宿主机测试在测试文件里实现它.
 *        这样同一份滤波代码在两种环境下逐位一致, 测试结论对固件有效.
 */
#include "knob.h"

/* 配置自检: 窗口长度必须是奇数, 否则"中值"会落在两个元素之间 */
#if ((KNOB_MEDIAN_WINDOW % 2U) == 0U)
#error "KNOB_MEDIAN_WINDOW 必须是奇数"
#endif

/* 配置自检: 滑动平均窗口必须是 2 的幂, 且与 SHIFT 一致, 否则移位求平均不成立 */
#if (KNOB_MOVAVG_WINDOW != (1U << KNOB_MOVAVG_SHIFT))
#error "KNOB_MOVAVG_WINDOW 必须等于 1 << KNOB_MOVAVG_SHIFT"
#endif

/** 归一化区间宽度(原始 ADC 计数), 预先算好避免每次用变量做除数 */
#define KNOB_RAW_SPAN (KNOB_RAW_MAX_VALID - KNOB_RAW_MIN_VALID)

static uint16_t s_median_buf[KNOB_MEDIAN_WINDOW]; /* 中值滤波环形缓冲 */
static uint8_t  s_median_pos;                     /* 环形写指针 */
static uint8_t  s_median_filled;                  /* 已填充个数(启动阶段不足窗口) */

static uint16_t s_avg_buf[KNOB_MOVAVG_WINDOW];    /* 滑动平均环形缓冲 */
static uint8_t  s_avg_pos;                        /* 环形写指针 */
static uint32_t s_avg_sum;                        /* 运行和: 入队累加, 出队相减 */

static uint16_t s_raw;      /* 最近一次原始值 */
static uint16_t s_filtered; /* 最近一次滤波值 */
static uint16_t s_level;    /* 最近一次归一化档位 */
static uint16_t s_reported; /* 上次被取走时的档位基线 */
static uint8_t  s_changed;  /* 档位变化标志 */

void Knob_Init(void)
{
  uint8_t i;

  for (i = 0U; i < KNOB_MEDIAN_WINDOW; i++)
  {
    s_median_buf[i] = 0U;
  }
  for (i = 0U; i < KNOB_MOVAVG_WINDOW; i++)
  {
    s_avg_buf[i] = 0U;
  }

  s_median_pos    = 0U;
  s_median_filled = 0U;
  s_avg_pos       = 0U;
  s_avg_sum       = 0U;

  s_raw      = 0U;
  s_filtered = 0U;
  s_level    = 0U;
  s_reported = 0U;
  s_changed  = 0U;
}

/**
 * @brief 对窗口内样本求中值
 * @param count 窗口内实际有效样本数(启动阶段可能不足窗口长度)
 * @return 中值
 * @note  窗口长度很小(默认 3), 直接插入排序比任何间接方法都省, 且无动态内存.
 */
static uint16_t knob_median(uint8_t count)
{
  uint16_t tmp[KNOB_MEDIAN_WINDOW];
  uint8_t  i;
  uint8_t  j;

  for (i = 0U; i < count; i++)
  {
    tmp[i] = s_median_buf[i];
  }

  for (i = 1U; i < count; i++)
  {
    uint16_t key = tmp[i];

    j = i;
    while ((j > 0U) && (tmp[j - 1U] > key))
    {
      tmp[j] = tmp[j - 1U];
      j--;
    }
    tmp[j] = key;
  }

  return tmp[count / 2U];
}

/** 滤波值 -> 归一化档位, 超出有效区间时夹到两端 */
static uint16_t knob_normalize(uint16_t filtered)
{
  uint32_t level;

  if (filtered <= KNOB_RAW_MIN_VALID)
  {
    return 0U;
  }
  if (filtered >= KNOB_RAW_MAX_VALID)
  {
    return KNOB_LEVEL_MAX;
  }

  /* 先减再乘: 最大中间值 4055 * 1000 = 4.055e6, 必须用 32 位承载 */
  level = ((uint32_t)(filtered - KNOB_RAW_MIN_VALID) * KNOB_LEVEL_MAX) / KNOB_RAW_SPAN;

  if (level > KNOB_LEVEL_MAX)
  {
    level = KNOB_LEVEL_MAX;
  }

  return (uint16_t)level;
}

void Knob_PushSample(uint16_t raw)
{
  uint16_t median;
  uint16_t oldest;
  uint16_t level;
  uint16_t diff;

  s_raw = raw;

  /* --- 第一级: 中值滤波, 把偶发尖峰整体剔除 --- */
  s_median_buf[s_median_pos] = raw;
  s_median_pos               = (uint8_t)((s_median_pos + 1U) % KNOB_MEDIAN_WINDOW);
  if (s_median_filled < KNOB_MEDIAN_WINDOW)
  {
    s_median_filled++;
  }

  median = knob_median(s_median_filled);

  /*
   * --- 第二级: 滑动平均 ---
   * 用"运行和 - 最旧值 + 新值"代替每次重新累加, 恒定 O(1).
   * 首帧让整个缓冲等于首个中值, 使平均从第一帧起就代表真实水平,
   * 不会出现"从 0 缓慢爬升"的启动瞬态(那会让上电瞬间档位严重偏低).
   */
  if (s_median_filled == 1U)
  {
    uint8_t i;

    for (i = 0U; i < KNOB_MOVAVG_WINDOW; i++)
    {
      s_avg_buf[i] = median;
    }
    s_avg_pos = 0U;
    s_avg_sum = (uint32_t)median * KNOB_MOVAVG_WINDOW;
  }

  oldest               = s_avg_buf[s_avg_pos];
  s_avg_buf[s_avg_pos] = median;
  s_avg_pos            = (uint8_t)((s_avg_pos + 1U) % KNOB_MOVAVG_WINDOW);
  s_avg_sum            = s_avg_sum - oldest + median;

  /* 窗口长度为 2 的幂, 右移等价于除法 */
  s_filtered = (uint16_t)(s_avg_sum >> KNOB_MOVAVG_SHIFT);

  /* --- 归一化与死区判定 --- */
  level   = knob_normalize(s_filtered);
  s_level = level;

  diff = (level > s_reported) ? (uint16_t)(level - s_reported)
                              : (uint16_t)(s_reported - level);
  if (diff > KNOB_LEVEL_DEADBAND)
  {
    s_changed = 1U;
  }
}

void Knob_Update(void)
{
  /*
   * 目标端: Knob_AdcSample() 从连续转换模式的 ADC 数据寄存器取值,
   * 不阻塞、不等待, 因此可以安全地在 1ms 控制循环里调用.
   */
  Knob_PushSample(Knob_AdcSample());
}

uint16_t Knob_GetRaw(void)
{
  return s_raw;
}

uint16_t Knob_GetFiltered(void)
{
  return s_filtered;
}

uint16_t Knob_GetLevel(void)
{
  return s_level;
}

uint8_t Knob_Changed(void)
{
  uint8_t changed = s_changed;

  s_changed = 0U;
  if (changed != 0U)
  {
    /*
     * 只有真正被取走时才推进基线, 因此"自上次读取以来累计的小位移"
     * 会被保留并累加, 慢速微调不会因为每帧都小于死区而被吞掉.
     */
    s_reported = s_level;
  }

  return changed;
}

void Knob_ClearChanged(void)
{
  s_changed = 0U;
}
