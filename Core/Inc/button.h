/**
 * @file button.h
 * @brief 按键消抖与短按/长按/重复事件(纯逻辑, 不依赖 HAL)
 * @note  本模块只接受"当前是否按下"这一个布尔量, 由调用者每 1ms 喂一次,
 *        因此可以在宿主机上直接编译测试, 实测无需任何 STM32 头文件.
 *
 * 事件语义:
 *   - 按下并稳定 BTN_DEBOUNCE_MS 后才认为真的按下(消抖), 抖动不会产生事件
 *   - 松开时若按住时长 <  BTN_LONGPRESS_MS  -> BTN_EVENT_SHORT
 *   - 松开时若按住时长 >= BTN_LONGPRESS_MS  -> BTN_EVENT_LONG
 *   - 按住超过 BTN_LONGPRESS_MS 期间, 每 BTN_REPEAT_MS 产生一次 BTN_EVENT_REPEAT
 */
#ifndef __BUTTON_H
#define __BUTTON_H

#include <stdint.h>

/** 按键事件 */
typedef enum
{
  BTN_EVENT_NONE = 0, /* 无事件 */
  BTN_EVENT_SHORT,    /* 短按(已在松开沿产生) */
  BTN_EVENT_LONG,     /* 长按(在松开沿产生) */
  BTN_EVENT_REPEAT    /* 长按保持期间的周期重复 */
} ButtonEvent;

/** 复位状态机 */
void Button_Init(void);

/**
 * @brief 每 1ms 调用一次, 喂入当前按键电平
 * @param level_pressed 1 = 按下, 0 = 松开
 */
void Button_Update(uint8_t level_pressed);

/**
 * @brief 取出并清除待处理事件
 * @return 最早已排队的事件; 无事件时返回 BTN_EVENT_NONE
 * @note  同一时刻最多保留一个事件, 事件未被取走时新的重复事件会被丢弃,
 *        避免控制循环一次挤入多个事件.
 */
ButtonEvent Button_Poll(void);

#endif /* __BUTTON_H */
