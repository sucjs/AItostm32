/**
 * @file adc.c
 * @brief ADC1 初始化实现(电位器输入 PA0)
 *
 * 时钟链:
 *   PCLK2 = 72MHz -> ADC 预分频 6 -> ADCCLK = 12MHz
 *   STM32F103 的 ADCCLK 上限为 14MHz, 取 /6 而非 /4 是为了留出裕量.
 *
 * 采样时间取 55.5 + 12.5 = 68 个 ADCCLK 周期, 约 5.7us.
 * 电位器源阻抗通常为几十 kOhm, 该采样时间足以完成充电, 读数不会随位置漂移.
 */
#include "adc.h"
#include "app_config.h"

ADC_HandleTypeDef hadc1;

HAL_StatusTypeDef MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef channel = {0};
  HAL_StatusTypeDef      status;

  hadc1.Instance                   = ADC1;
  hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode    = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion       = 1U;
  hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;

  /* 此处触发 HAL_ADC_MspInit: 使能 ADC1/GPIOA 时钟并配置 ADC 预分频 */
  status = HAL_ADC_Init(&hadc1);
  if (status != HAL_OK)
  {
    return status;
  }

  /* 校准: 由硬件写入内部校准码, 消除器件间偏移 */
  status = HAL_ADCEx_Calibration_Start(&hadc1);
  if (status != HAL_OK)
  {
    return status;
  }

  channel.Channel      = ADC_CHANNEL_0;
  channel.Rank         = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;

  status = HAL_ADC_ConfigChannel(&hadc1, &channel);
  if (status != HAL_OK)
  {
    return status;
  }

  /*
   * 连续转换模式: 启动一次后硬件持续转换, 数据寄存器始终保存最新结果.
   * 之后每次需要读数时直接 HAL_ADC_GetValue(读 DR, 同时清 EOC)即可.
   */
  return HAL_ADC_Start(&hadc1);
}

/**
 * @brief ADC MSP 初始化: 时钟与模拟引脚
 * @note  由 HAL 在 HAL_ADC_Init 内部回调.
 */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  GPIO_InitTypeDef gpio = {0};

  if (hadc->Instance == ADC1)
  {
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PCLK2 / 6 = 12MHz, 低于 F103 的 14MHz 上限 */
    __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6);

    /* PA0 必须为模拟输入: 关闭数字输入缓冲, 避免引脚漏电影响读数 */
    gpio.Pin  = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOA, &gpio);
  }
}

/**
 * @brief knob.c 的原始采样钩子(目标端实现)
 * @return 最新一次 ADC 转换结果
 * @note  连续转换模式下数据寄存器始终保存最新结果, 读取 DR 即可,
 *        不需要 Start/Poll 等待, 因此不会阻塞 1ms 控制循环.
 *        knob.c 本身不含任何 STM32 头文件, 硬件耦合点仅此一处.
 */
uint16_t Knob_AdcSample(void)
{
  return (uint16_t)HAL_ADC_GetValue(&hadc1);
}
