/**
 * @file tim.c
 * @brief TIM4 控制循环时基初始化
 */
#include "tim.h"
#include "app_config.h"

TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;

/**
 * @brief 初始化 TIM4 为固定 1ms 周期中断
 * @note  时基计算: TIM4 位于 APB1, 当 APB1 预分频 != 1 时定时器时钟为 PCLK1*2 = 72MHz.
 *        取 PSC = 7200-1, 计数频率 = 10kHz, ARR = 10-1 -> 周期 1ms.
 *        该组合误差为 0 (72e6 / 7200 / 10 = 1000Hz).
 */
HAL_StatusTypeDef MX_TIM4_Init(void)
{
  TIM_ClockConfigTypeDef  clock_source = {0};
  TIM_MasterConfigTypeDef master       = {0};
  uint32_t                prescaler;
  uint32_t                period;

  __HAL_RCC_TIM4_CLK_ENABLE();

  /* 由目标频率反推 PSC/ARR, 保证 72MHz 之外的时钟也能正确工作 */
  prescaler = (APP_TIMER_HZ / 10000UL);        /* 先把计数频率压到 10kHz */
  if (prescaler == 0U)
  {
    prescaler = 1U;
  }
  period = (10000UL / APP_CONTROL_HZ);         /* 10kHz / 1000Hz = 10 */
  if (period == 0U)
  {
    period = 1U;
  }

  htim4.Instance               = TIM4;
  htim4.Init.Prescaler         = prescaler - 1U;
  htim4.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim4.Init.Period            = period - 1U;
  htim4.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    return HAL_ERROR;
  }

  clock_source.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim4, &clock_source) != HAL_OK)
  {
    return HAL_ERROR;
  }

  master.MasterOutputTrigger = TIM_TRGO_RESET;
  master.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &master) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* 使能更新中断, 由 stm32f1xx_it.c 中的 TIM4_IRQHandler 调用 HAL_TIM_IRQHandler */
  HAL_NVIC_SetPriority(TIM4_IRQn, 2U, 0U);
  HAL_NVIC_EnableIRQ(TIM4_IRQn);

  return HAL_TIM_Base_Start_IT(&htim4);
}

/**
 * @brief TIM4 MSP 初始化: 使能时钟与中断
 * @note  由 HAL 在 HAL_TIM_Base_Init 内部回调.
 */
void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM4)
  {
    __HAL_RCC_TIM4_CLK_ENABLE();
  }
}
