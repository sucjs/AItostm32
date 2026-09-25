/**
 * @file adc.h
 * @brief ADC1 初始化(电位器输入 PA0 / ADC1_IN0)
 * @note  资源分配:
 *          ADC1_IN0 -> PA0, 单通道连续转换, 供 knob.c 每次直接读取 DR.
 *        连续转换模式下硬件不断刷新数据寄存器, 读取即是"最新一次"结果,
 *        因此控制循环里不需要 Poll 等待, 也不会阻塞 1ms 节拍.
 */
#ifndef __ADC_H
#define __ADC_H

#include "main.h"

extern ADC_HandleTypeDef hadc1;

/**
 * @brief 初始化 ADC1 为单通道连续转换
 * @return HAL_OK 或错误码
 * @note  内部顺序: 时钟/引脚(MSP) -> HAL_ADC_Init -> 校准 -> 通道配置 -> 启动转换.
 *        校准必须在内核使能之后、启动转换之前完成, 否则首批读数偏差较大.
 */
HAL_StatusTypeDef MX_ADC1_Init(void);

#endif /* __ADC_H */
