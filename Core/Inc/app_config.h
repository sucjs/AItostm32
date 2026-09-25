/**
 * @file app_config.h
 * @brief 应用层可调参数集中定义
 * @note  本文件只放常量, 不放代码. 修改量程/时序只需改这里.
 */
#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ========================== 时基 ========================== */

/** 系统节拍(ms), HAL_Init 的 SysTick 周期 */
#define APP_TICK_MS 1U

/** 控制循环频率(Hz), 由 TIM4 周期中断驱动 */
#define APP_CONTROL_HZ 1000U

/** TIM3/TIM4 的输入时钟(Hz). STM32F103 在 APB1 预分频=2 时定时器时钟等于 HCLK */
#define APP_TIMER_HZ 72000000UL

/**
 * 定时器重载寄存器上限. ARR 为 16 位, 合法范围 0..65535.
 * 注意: 硬件一个周期的计数是 (ARR+1)*(PSC+1), 因此可容纳的最大计数是
 * APP_TIMER_COUNTS_MAX, 而写入 ARR 的值最大是 APP_TIMER_ARR_MAX.
 */
#define APP_TIMER_ARR_MAX 65535UL

/** 每周期可容纳的最大计数 (ARR+1) */
#define APP_TIMER_COUNTS_MAX 65536UL

/* ========================== 电位器 / ADC ========================== */

/** ADC 满量程(12 位) */
#define KNOB_ADC_MAX 4095U

/** 中值滤波窗口(奇数, 建议 3 或 5) */
#define KNOB_MEDIAN_WINDOW 3U

/** 滑动平均窗口(2 的幂, 便于移位) */
#define KNOB_MOVAVG_WINDOW 16U
#define KNOB_MOVAVG_SHIFT 4U

/** 归一化输出上限(0..KNOB_LEVEL_MAX) */
#define KNOB_LEVEL_MAX 1000U

/** 死区: 归一化值变化超过该阈值才认为旋钮有效变化(抑制显示抖动) */
#define KNOB_LEVEL_DEADBAND 4U

/** 旋钮总行程两端的无效区(ADC 原始值), 规避机械行程外的悬空读数 */
#define KNOB_RAW_MIN_VALID 20U
#define KNOB_RAW_MAX_VALID 4075U

/* ========================== 按键 ========================== */

/** 消抖时间(ms): 连续稳定该时长才确认电平 */
#define BTN_DEBOUNCE_MS 20U

/** 长按判定时间(ms) */
#define BTN_LONGPRESS_MS 800U

/** 长按后重复触发短按的间隔(ms), 用于编辑态连续调整 */
#define BTN_REPEAT_MS 150U

/** 按键有效电平: 0 表示低电平有效(内部上拉) */
#define BTN_ACTIVE_LEVEL 0U

/* ========================== 菜单 / 显示 ========================== */

/** 菜单项上限: 1 个协议选择项 + 协议自身参数项 */
#define MENU_MAX_ITEMS 3U

/** 刷新周期(ms), 限制 OLED 刷新率, 避免 I2C 阻塞控制循环 */
#define UI_REFRESH_MS 80U

/** 条形图区域(像素) */
#define UI_BAR_X 0U
#define UI_BAR_Y 40U
#define UI_BAR_W 120U
#define UI_BAR_H 10U
/** 波形图区域(像素) */
#define UI_WAVE_X 0U
#define UI_WAVE_Y 52U
#define UI_WAVE_W 124U
#define UI_WAVE_H 11U

/** 波形图保存的历史点数 */
#define UI_WAVE_POINTS 62U

/** 日志条目数 */
#define SIGLOG_CAPACITY 8U

/** 单条日志的最大长度(含结尾 0) */
#define SIGLOG_MSG_LEN 22U

/* ========================== 信号输出 ========================== */

/**
 * DShot 帧间隔(us), 即"节流周期/looptime", 决定帧重复率.
 * 125us 对应 8kHz, 是当前电调的标准节流周期.
 * DShot 不是把一帧紧接着一帧发, 而是每 125us 发一帧, 帧后保持低电平.
 * 若该值取 0, 协议层退化为"最小帧 + 紧接重发", 帧率会高达数十 kHz,
 * 多数电调会判为非法信号.
 */
#define DSHOT_LOOPTIME_US 125U

/**
 * 信号输出 DMA 通道. STM32F103 的 DMA 请求与通道是硬件固定映射:
 * TIM3_UP -> DMA1_Channel3 (见 RM0008 表 78), 不可自由选择.
 */
#define SIGOUT_DMA_CHANNEL DMA1_Channel3

/** 信号输出引脚: TIM3_CH1 默认映射 PA6 */
#define SIGOUT_PORT        GPIOA
#define SIGOUT_PIN         GPIO_PIN_6

/** 信号空闲时输出电平: CCR=0 -> 低电平, 符合舵机/电调无效电平约定 */
#define SIGOUT_IDLE_COMPARE 0U

#endif /* __APP_CONFIG_H */
