/**
 * @file signal_output.c
 * @brief 信号输出硬件层实现(TIM3_CH1 PWM + DMA1_Channel3)
 *
 * 依赖关系:
 *   - 本文件是唯一直接操作 TIM3/DMA1 的地方, 其它模块只通过本层接口输出信号.
 *   - 协议解析与编码全部委托给 signal_protocol.c, 本层不含任何协议知识.
 *
 * 切换事务性:
 *   1. 先 SignalProtocol_BuildPlan 生成方案(纯计算, 不碰寄存器)
 *   2. 失败则立即返回, 硬件与当前协议完全不变
 *   3. 成功才停 DMA -> 写 PSC/ARR/CCR -> 需要时重挂 DMA -> 使能输出
 *   这样任何失败都不会让输出停在半个周期或未配置状态.
 */
#include "signal_output.h"
#include "app_config.h"
#include "tim.h"

#include <string.h>

/*
 * DMA 帧缓冲容量(位槽数).
 * 必须能容纳最长的 DShot 帧: 72MHz + 125us 节流周期下 DShot600 需要
 * 75 个位槽(16 数据位 + 停止位 + 空闲填充). 取 80 留少量余量,
 * 新增更高速率协议时这里会由 BuildPlan 校验并明确报 FRAMELONG.
 */
#define SIGOUT_DMA_BUF_LEN 80U

static TIM_HandleTypeDef s_htim3;

/*
 * TIM3_UP 的 DMA 句柄.
 * @note 之所以对外可见(不加 s_ 前缀): stm32f1xx_it.c 的
 *       DMA1_Channel3_IRQHandler 必须把中断交给"HAL 正在使用的那个"句柄.
 *       若在中断文件里另建句柄, 其 DmaBaseAddress 为空指针,
 *       HAL_DMA_IRQHandler 会在解引用时崩溃.
 */
DMA_HandleTypeDef hdma_tim3;

static uint16_t         s_dma_buf[SIGOUT_DMA_BUF_LEN];
static SignalOutputInfo s_info;

/* ========================== 私有工具 ========================== */

/** 把输出固定为低电平空闲态(CCR=0, 不产生脉冲) */
static void output_set_idle_level(void)
{
  __HAL_TIM_SET_COMPARE(&s_htim3, TIM_CHANNEL_1, SIGOUT_IDLE_COMPARE);
}

/** 停止并解绑可能存在的 DMA 传输 */
static void output_stop_dma(void)
{
  if (s_info.use_dma != 0U)
  {
    /* 先关定时器的 DMA 请求, 再中止 DMA, 避免停在中途触发半帧写入 */
    __HAL_TIM_DISABLE_DMA(&s_htim3, TIM_DMA_UPDATE);
    (void)HAL_DMA_Abort(&hdma_tim3);
    s_info.use_dma = 0U;
  }
}

/** 关断定时器输出并让引脚回到空闲电平 */
static void output_gate_off(void)
{
  if (s_htim3.Instance != NULL)
  {
    (void)HAL_TIM_PWM_Stop(&s_htim3, TIM_CHANNEL_1);
    output_set_idle_level();
  }
}

/* ========================== 初始化 ========================== */

SignalStatus SignalOutput_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  memset(&s_info, 0, sizeof(s_info));
  s_info.state      = SIGOUT_STATE_IDLE;
  s_info.last_error = SIG_OK;

  /* --- 时钟: 定时器 / DMA / 输出端口 --- */
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  if (SIGOUT_PORT == GPIOA)
  {
    __HAL_RCC_GPIOA_CLK_ENABLE();
  }
  else
  {
    __HAL_RCC_GPIOB_CLK_ENABLE();
  }

  /* --- PWM 引脚: 复用推挽输出 --- */
  gpio.Pin   = SIGOUT_PIN;
  gpio.Mode  = GPIO_MODE_AF_PP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SIGOUT_PORT, &gpio);

  /* --- 时基: 先按安全值配置, 具体周期由 Select 写入 --- */
  s_htim3.Instance               = TIM3;
  s_htim3.Init.Prescaler         = 0U;
  s_htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
  s_htim3.Init.Period            = APP_TIMER_ARR_MAX; /* 最长周期, 频率最低, 最安全 */
  s_htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  s_htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

  if (HAL_TIM_PWM_Init(&s_htim3) != HAL_OK)
  {
    s_info.state      = SIGOUT_STATE_ERROR;
    s_info.last_error = SIG_ERR_UNSUPPORTED;
    return SIG_ERR_UNSUPPORTED;
  }

  /* --- 输出通道: PWM1, 高电平有效 --- */
  {
    TIM_OC_InitTypeDef oc = {0};

    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = SIGOUT_IDLE_COMPARE;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&s_htim3, &oc, TIM_CHANNEL_1) != HAL_OK)
    {
      s_info.state      = SIGOUT_STATE_ERROR;
      s_info.last_error = SIG_ERR_UNSUPPORTED;
      return SIG_ERR_UNSUPPORTED;
    }
  }

  /* --- DMA: TIM3_UP 请求固定连到 DMA1_Channel3 --- */
  hdma_tim3.Instance                 = SIGOUT_DMA_CHANNEL;
  hdma_tim3.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_tim3.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_tim3.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_tim3.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_tim3.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
  /*
   * 循环模式: DShot 必须连续重复发送整帧, 电调才认为信号有效.
   * 若用 DMA_NORMAL, 一帧传完就停止, 帧周期内其余时间输出保持低电平,
   * 电调会因为收不到后续帧而判定信号丢失.
   * 循环模式下 DMA 传输完成后自动重载地址与计数, 无需在中断里重新挂载.
   */
  hdma_tim3.Init.Mode                = DMA_CIRCULAR;
  hdma_tim3.Init.Priority            = DMA_PRIORITY_HIGH;

  if (HAL_DMA_Init(&hdma_tim3) != HAL_OK)
  {
    s_info.state      = SIGOUT_STATE_ERROR;
    s_info.last_error = SIG_ERR_UNSUPPORTED;
    return SIG_ERR_UNSUPPORTED;
  }

  /*
   * 使能 DMA 传输完成中断的 NVIC 线.
   * HAL_DMA_Start_IT 只在 DMA 外设内打开 TCIE 位; 若 NVIC 不使能,
   * DMA1_Channel3_IRQHandler 永远不会被调用, DShot 的每帧完成事件就无人处理.
   * 该中断优先级低于 TIM4(2), 保证 1ms 控制节拍不被帧传输打断.
   */
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 3U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);

  /* 定时器更新事件 -> DMA 写 CCR1 */
  __HAL_LINKDMA(&s_htim3, hdma[TIM_DMA_ID_UPDATE], hdma_tim3);

  output_set_idle_level();
  s_info.state = SIGOUT_STATE_READY;

  return SIG_OK;
}

/* ========================== 校验 ========================== */

uint16_t SignalOutput_DmaCapacity(void)
{
  return (uint16_t)SIGOUT_DMA_BUF_LEN;
}

SignalStatus SignalOutput_Probe(const SignalProtocol *proto)
{
  if (proto == NULL)
  {
    return SIG_ERR_NULL_HANDLE;
  }
  if (s_info.state != SIGOUT_STATE_READY)
  {
    return SIG_ERR_UNSUPPORTED;
  }

  /*
   * 仅做静态能力校验(定时器分辨率/帧长), 不触碰寄存器.
   * 参数相关的校验在 BuildPlan 中完成.
   */
  return SignalProtocol_Probe(proto, APP_TIMER_HZ);
}

/* ========================== 硬件写入 ========================== */

/**
 * @brief 把已校验的方案写入硬件
 * @note  调用前必须已停止 DMA 与输出, 避免写入过程中产生杂波.
 */
static SignalStatus output_program(const SignalPlan *plan)
{
  uint32_t compare = plan->ccr;

  if (plan->proto->wave == SIG_WAVE_DSHOT)
  {
    uint16_t len = plan->dma_len;

    if ((len == 0U) || (len > SIGOUT_DMA_BUF_LEN))
    {
      return SIG_ERR_FRAME_TOO_LONG;
    }

    /* 先按当前参数编好整帧, 再启动 DMA: 保证首帧就是完整帧 */
    if (SignalProtocol_EncodeDshotFrame(plan, (uint16_t)plan->param_values[0], 0U,
                                        s_dma_buf, SIGOUT_DMA_BUF_LEN) != SIG_OK)
    {
      return SIG_ERR_FRAME_TOO_LONG;
    }

    /* 写时基与该位的高电平长度(第 0 位), 使首位的边沿立即正确 */
    __HAL_TIM_SET_PRESCALER(&s_htim3, plan->psc);
    __HAL_TIM_SET_AUTORELOAD(&s_htim3, plan->arr);
    __HAL_TIM_SET_COMPARE(&s_htim3, TIM_CHANNEL_1, s_dma_buf[0]);
    __HAL_TIM_SET_COUNTER(&s_htim3, 0U);
  }
  else
  {
    __HAL_TIM_SET_PRESCALER(&s_htim3, plan->psc);
    __HAL_TIM_SET_AUTORELOAD(&s_htim3, plan->arr);
    output_set_idle_level();
  }

  /* 产生更新事件, 让刚写入的 PSC/ARR 立即生效 */
  s_htim3.Instance->EGR = TIM_EGR_UG;
  __HAL_TIM_CLEAR_FLAG(&s_htim3, TIM_FLAG_UPDATE);

  /* 开输出: PWM 与单脉冲都走 PWM 模式, 由 CCR 决定高电平长度 */
  if (HAL_TIM_PWM_Start(&s_htim3, TIM_CHANNEL_1) != HAL_OK)
  {
    return SIG_ERR_UNSUPPORTED;
  }

  if (plan->proto->wave == SIG_WAVE_DSHOT)
  {
    /* 启动 DMA 前先使能定时器更新 DMA 请求, 否则首个更新事件会丢 */
    __HAL_TIM_ENABLE_DMA(&s_htim3, TIM_DMA_UPDATE);

    /*
     * 用非中断方式启动: 循环模式下 DMA 自动重发, 不需要传输完成中断.
     * 若用 Start_IT, 中断会以帧率触发(DShot600 约 33kHz), 白白消耗 CPU,
     * 并与 1ms 控制节拍争抢. 运行期间不再改写帧缓冲, 故无需完成回调.
     */
    if (HAL_DMA_Start(&hdma_tim3, (uint32_t)s_dma_buf, (uint32_t)&TIM3->CCR1,
                      plan->dma_len) != HAL_OK)
    {
      __HAL_TIM_DISABLE_DMA(&s_htim3, TIM_DMA_UPDATE);
      output_gate_off();
      return SIG_ERR_UNSUPPORTED;
    }

    s_info.use_dma = 1U;
  }
  else
  {
    /* 非 DMA 路径: 只改比较值决定占空比/脉宽 */
    __HAL_TIM_SET_COMPARE(&s_htim3, TIM_CHANNEL_1, (uint16_t)compare);
    s_info.use_dma = 0U;
  }

  return SIG_OK;
}

/** 用方案刷新 info 快照 */
static void output_update_info(const SignalPlan *plan)
{
  s_info.active_proto = plan->proto;
  s_info.param_values[0] = plan->param_values[0];
  s_info.param_values[1] = plan->param_values[1];
  s_info.actual_hz       = plan->actual_hz;
  s_info.period_us       = plan->period_us;
  s_info.psc             = plan->psc;
  s_info.arr             = plan->arr;
  s_info.dma_len         = plan->dma_len;
  s_info.ccr = (plan->proto->wave == SIG_WAVE_DSHOT) ? plan->ccr_high : plan->ccr;
}

/* ========================== 对外接口 ========================== */

SignalStatus SignalOutput_Select(const SignalProtocol *proto, const uint32_t *param_values)
{
  SignalPlan   plan;
  SignalStatus status;

  if ((proto == NULL) || (param_values == NULL))
  {
    s_info.last_error = SIG_ERR_NULL_HANDLE;
    return SIG_ERR_NULL_HANDLE;
  }
  if (s_info.state != SIGOUT_STATE_READY)
  {
    s_info.last_error = SIG_ERR_UNSUPPORTED;
    return SIG_ERR_UNSUPPORTED;
  }

  /* 第一步: 纯计算 + 全量校验. 此处失败时硬件完全未被触碰 */
  status = SignalProtocol_BuildPlan(proto, param_values, APP_TIMER_HZ,
                                    (uint16_t)SIGOUT_DMA_BUF_LEN, &plan);
  if (status != SIG_OK)
  {
    s_info.last_error = status;
    return status;
  }

  /* 第二步: 停旧输出(DMA 必须先停, 否则新配置会被残余传输覆盖) */
  output_stop_dma();
  output_gate_off();

  /* 第三步: 写入新配置 */
  status = output_program(&plan);
  if (status != SIG_OK)
  {
    /*
     * 写入失败: 回到安全的空闲低电平, 而不是继续输出半配置波形.
     * 协议字段保持为旧值, 使界面显示与实际不一致时能看出异常.
     */
    output_stop_dma();
    output_gate_off();
    s_info.last_error = status;
    return status;
  }

  output_update_info(&plan);
  s_info.last_error = SIG_OK;

  return SIG_OK;
}

SignalStatus SignalOutput_Apply(const uint32_t *param_values)
{
  if (param_values == NULL)
  {
    s_info.last_error = SIG_ERR_NULL_HANDLE;
    return SIG_ERR_NULL_HANDLE;
  }
  if (s_info.active_proto == NULL)
  {
    s_info.last_error = SIG_ERR_UNKNOWN_ID;
    return SIG_ERR_UNKNOWN_ID;
  }

  /*
   * 参数更新复用 Select 的完整流程: 需要重算 PSC/ARR 的协议(如 PWM 改频率)
   * 与只需重编码的协议(DShot)都能正确处理, 且同样具备失败不变性.
   */
  return SignalOutput_Select(s_info.active_proto, param_values);
}

void SignalOutput_Stop(void)
{
  output_stop_dma();
  output_gate_off();

  s_info.state        = SIGOUT_STATE_IDLE;
  s_info.active_proto = NULL;
  s_info.actual_hz    = 0U;
  s_info.period_us    = 0U;
}

const SignalOutputInfo *SignalOutput_GetInfo(void)
{
  return &s_info;
}

const SignalProtocol *SignalOutput_ActiveProto(void)
{
  return s_info.active_proto;
}
