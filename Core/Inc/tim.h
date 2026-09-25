/**
 * @file tim.h
 * @brief 定时器资源初始化
 * @note  资源分配:
 *          TIM3 -> 信号输出(TIM3_CH1 + DMA1_Channel3)
 *          TIM4 -> 1ms 控制循环时基(仅更新中断, 无输出引脚)
 *        两者独立, 改信号周期不会影响控制循环节奏.
 */
#ifndef __TIM_H
#define __TIM_H

#include "main.h"

/** 信号输出定时器(通道 1) */
extern TIM_HandleTypeDef htim3;

/** 控制循环时基定时器 */
extern TIM_HandleTypeDef htim4;

/**
 * @brief 初始化 TIM4 为 1ms 周期中断, 并启动
 * @return HAL_OK 或错误码
 * @note  信号输出定时器 TIM3 由 signal_output.c 自行管理,
 *        因为它需要频繁改写 PSC/ARR, 不适合在这里固定初始化.
 */
HAL_StatusTypeDef MX_TIM4_Init(void);

#endif /* __TIM_H */
