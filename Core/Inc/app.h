/**
 * @file app.h
 * @brief 应用层装配与主循环
 * @note  分工:
 *          App_Tick()  在 TIM4 中断里跑, 只做"计数 + 置标志", 必须极短
 *          App_Loop()  在主循环里跑, 每次处理 1ms 的控制迭代
 *        中断与主循环通过 tick 计数 / 待处理标志交接, 主循环侧不做忙等.
 */
#ifndef __APP_H
#define __APP_H

#include <stdint.h>

/**
 * @brief 初始化所有应用模块
 * @note  必须在 HAL_Init / SystemClock_Config / MX_GPIO_Init / MX_I2C2_Init
 *        之后调用. 内部会按依赖顺序初始化外设、输出层与界面.
 */
void App_Init(void);

/**
 * @brief 1ms 节拍回调(在 TIM4 中断上下文执行)
 * @warning 只允许做计数与置标志: 不得调用 I2C/OLED/日志/任何可能阻塞的接口,
 *          否则会拖长中断、破坏 1ms 节拍.
 */
void App_Tick(void);

/**
 * @brief 主循环体, 反复调用
 * @note  检测到新的 1ms 节拍后执行一次控制迭代: 采样按键/旋钮,
 *        分发菜单事件, 并按 UI_REFRESH_MS 限制刷新显示.
 */
void App_Loop(void);

/**
 * @brief 自启动以来 1ms 节拍的总数
 * @note  用于验证中断与主循环的交接是否正常(节拍必须持续增长).
 */
uint32_t App_GetTickCount(void);

/**
 * @brief 主循环已处理的控制迭代次数
 * @note  正常情况下应与 App_GetTickCount() 同步增长.
 */
uint32_t App_GetLoopCount(void);

#endif /* __APP_H */
