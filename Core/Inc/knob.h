/**
 * @file knob.h
 * @brief 电位器采样、滤波与归一化(纯逻辑, 不依赖 HAL)
 * @note  为便于在宿主机上做单元测试, 本模块只包含 <stdint.h> + app_config.h,
 *        唯一的硬件耦合点被隔离成 Knob_AdcSample() 这一个"钩子":
 *
 *          - 目标端(Firmware): 由 adc.c 实现, 内部调用 HAL_ADC_GetValue(&hadc1)
 *          - 宿主机(Host test): 由测试文件自行实现, 返回预设样本
 *
 *        因此 knob.c 在两种环境下都能编译, 实测滤波器行为只需喂样本即可.
 *
 * 滤波链: 原始值 -> 中值滤波(KNOB_MEDIAN_WINDOW) -> 滑动平均(KNOB_MOVAVG_WINDOW)
 *         -> 归一化到 0..KNOB_LEVEL_MAX
 *        中值先行的理由: 它能把偶发的大幅尖峰(ADC 噪声、I2C 干扰)整体剔除,
 *        而滑动平均只能把尖峰摊薄到多帧, 单独使用会在界面上留下可见抖动.
 */
#ifndef __KNOB_H
#define __KNOB_H

#include <stdint.h>
#include "app_config.h"

/** 复位滤波器与归一化状态 */
void Knob_Init(void);

/**
 * @brief 读取一次原始 ADC 样本并送入滤波链
 * @note  目标端从连续转换的 ADC 数据寄存器直接取值, 不做阻塞等待.
 *        宿主机测试请改用 Knob_PushSample() 注入样本.
 */
void Knob_Update(void);

/**
 * @brief 直接注入一个原始样本(供宿主机测试或自定义采样路径使用)
 * @param raw 原始 ADC 值, 12 位(0..KNOB_ADC_MAX)
 */
void Knob_PushSample(uint16_t raw);

/** 最近一次送入滤波链的原始值 */
uint16_t Knob_GetRaw(void);

/** 中值 + 滑动平均之后的滤波值 */
uint16_t Knob_GetFiltered(void);

/** 归一化档位, 范围 0..KNOB_LEVEL_MAX */
uint16_t Knob_GetLevel(void);

/**
 * @brief 归一化档位相对上次上报是否发生了超过死区的变化
 * @return 1 = 有效变化, 0 = 无变化
 * @note  读取即清除. 死区内的小幅抖动不会置位, 避免界面/参数被噪声推动.
 */
uint8_t Knob_Changed(void);

/** 主动清除变化标志(不改变当前档位基线) */
void Knob_ClearChanged(void);

/**
 * @brief 原始 ADC 采样钩子
 * @return 当前原始 ADC 值
 * @note  目标端由 adc.c 实现; 宿主机由测试桩实现(非弱符号, 缺失时链接期报错,
 *        以避免"忘记实现却静默返回 0"这类难以发现的错误).
 */
uint16_t Knob_AdcSample(void);

#endif /* __KNOB_H */
