/**
 * @file button.c
 * @brief 按键消抖与事件识别实现
 * @note  纯整数、纯逻辑, 不引用任何 STM32 头文件, 便于宿主机单元测试.
 *
 * 消抖策略:
 *   原始电平每次变化就把"稳定计时"清零, 只有连续 BTN_DEBOUNCE_MS 毫秒
 *   电平不变, 才把它认作有效电平. 这样机械抖动的每一次跳变都会重置计时,
 *   抖动结束后只产生一次状态迁移, 因此一次真实的按下-松开只产生一个事件.
 */
#include "button.h"
#include "app_config.h"

/* 消抖计时上限: 电平稳定到该值后不再累加, 避免 uint16 无意义增长 */
#define BTN_STABLE_CAP_MS BTN_DEBOUNCE_MS

static uint8_t     s_level;      /* 消抖后的确认电平, 1 = 按下 */
static uint8_t     s_raw_level;  /* 最近一次喂入的原始电平 */
static uint16_t    s_stable_ms;  /* s_raw_level 已保持的时长(ms) */
static uint16_t    s_hold_ms;    /* 确认按下后已持续的时长(ms) */
static uint16_t    s_repeat_ms;  /* 距上一次 REPEAT 的时长(ms) */
static ButtonEvent s_pending;    /* 待取走的事件 */

void Button_Init(void)
{
  s_level      = 0U;
  s_raw_level  = 0U;
  s_stable_ms  = BTN_STABLE_CAP_MS;
  s_hold_ms    = 0U;
  s_repeat_ms  = 0U;
  s_pending    = BTN_EVENT_NONE;
}

/** 排队一个事件; 已有事件未被取走时丢弃新事件 */
static void button_emit(ButtonEvent ev)
{
  if (s_pending == BTN_EVENT_NONE)
  {
    s_pending = ev;
  }
}

void Button_Update(uint8_t level_pressed)
{
  uint8_t raw = (level_pressed != 0U) ? 1U : 0U;

  /* --- 原始电平变化: 重新开始消抖计时 --- */
  if (raw != s_raw_level)
  {
    s_raw_level = raw;
    s_stable_ms = 0U;
  }
  else if (s_stable_ms < BTN_STABLE_CAP_MS)
  {
    s_stable_ms++;
  }
  else
  {
    /* 已稳定, 无需处理 */
  }

  /* --- 稳定足够久才把原始电平提升为确认电平 --- */
  if ((s_stable_ms >= BTN_DEBOUNCE_MS) && (s_level != s_raw_level))
  {
    s_level = s_raw_level;

    if (s_level != 0U)
    {
      /* 按下沿: 开始计时长按 */
      s_hold_ms   = 0U;
      s_repeat_ms = 0U;
    }
    else
    {
      /* 松开沿: 依据按住时长判定短按/长按 */
      if (s_hold_ms >= BTN_LONGPRESS_MS)
      {
        button_emit(BTN_EVENT_LONG);
      }
      else
      {
        button_emit(BTN_EVENT_SHORT);
      }
      s_hold_ms   = 0U;
      s_repeat_ms = 0U;
    }
  }

  /* --- 保持按下: 累计时长, 越过长按阈值后周期性产生 REPEAT --- */
  if (s_level != 0U)
  {
    if (s_hold_ms < 0xFFFFU)
    {
      s_hold_ms++;
    }

    if (s_hold_ms > BTN_LONGPRESS_MS)
    {
      s_repeat_ms++;
      if (s_repeat_ms >= BTN_REPEAT_MS)
      {
        s_repeat_ms = 0U;
        button_emit(BTN_EVENT_REPEAT);
      }
    }
  }
}

ButtonEvent Button_Poll(void)
{
  ButtonEvent ev = s_pending;

  s_pending = BTN_EVENT_NONE;
  return ev;
}
