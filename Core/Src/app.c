/**
 * @file app.c
 * @brief 应用层装配与主循环实现
 *
 * 时序设计:
 *   TIM4 每 1ms 中断 -> HAL_TIM_PeriodElapsedCallback -> App_Tick()
 *   App_Tick 只递增计数器并置标志(中断内必须极短).
 *   App_Loop 在主循环里检测标志, 每个节拍执行一次控制迭代.
 *
 * 为什么不在中断里直接做控制:
 *   OLED 刷新走 I2C 阻塞传输, 单帧可达数十毫秒. 若放进中断, 会长时间
 *   占住 CPU 并让 SysTick/其它中断饥饿. 因此中断只做交接, 重活留给主循环.
 *
 * 为什么节拍用"计数器 + 标志"而不是单纯 bool:
 *   主循环若因 I2C 阻塞错过了若干个节拍, 计数器能反映真实经过的毫秒数,
 *   调试时可立刻看出控制循环是否被拖慢.
 */
#include "app.h"
#include "app_config.h"

#include "main.h"
#include "gpio.h"
#include "i2c.h"
#include "adc.h"
#include "tim.h"

#include "knob.h"
#include "button.h"
#include "menu.h"
#include "ui.h"
#include "signal_log.h"
#include "signal_output.h"

/* ========================== 状态 ========================== */

/** 自启动以来的 1ms 节拍数(中断侧递增, 主循环侧只读) */
static volatile uint32_t s_tick_count;

/** 有待处理的节拍标志(中断置位, 主循环清除) */
static volatile uint8_t s_tick_pending;

/** 主循环已处理的迭代次数 */
static uint32_t s_loop_count;

/** 距上次界面刷新已过去的毫秒数 */
static uint16_t s_ui_ms;

/* ========================== 私有工具 ========================== */

/** 记录一次初始化步骤的结果, 失败时立即返回 0 */
static uint8_t app_check_hal(const char *step, HAL_StatusTypeDef status)
{
  if (status != HAL_OK)
  {
    SignalLog_Printf(SIGLOG_ERROR, "%s fail", step);
    return 0U;
  }

  SignalLog_Printf(SIGLOG_INFO, "%s ok", step);
  return 1U;
}

/** 读取按键当前是否按下(低电平有效) */
static uint8_t app_button_pressed(void)
{
  return (HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET) ? 1U : 0U;
}

/* ========================== 初始化 ========================== */

void App_Init(void)
{
  HAL_StatusTypeDef status;

  s_tick_count   = 0U;
  s_tick_pending = 0U;
  s_loop_count   = 0U;
  s_ui_ms        = 0U;

  SignalLog_Init();

  /*
   * 初始化顺序遵循依赖关系:
   *   GPIO(按键/指示灯) -> I2C(OLED 物理层) -> ADC(旋钮) -> TIM4(节拍)
   *   -> 日志 -> 输出层 -> 旋钮/按键 -> 界面 -> 菜单
   *
   * 输出层放在菜单之前: Menu_Init 会调用 SignalOutput_Select, 必须已有可用硬件.
   * 界面放在菜单之前: Menu_Init 产生的日志与状态需要被界面读取.
   *
   * 任一外设初始化失败都会记日志但继续执行, 以便至少把失败原因显示在屏幕上;
   * 真正需要中断启动的只有 TIM4.
   */
  MX_GPIO_Init();
  SignalLog_Push(SIGLOG_INFO, "gpio ok");

  MX_I2C2_Init();
  SignalLog_Push(SIGLOG_INFO, "i2c2 ok");

  status = MX_ADC1_Init();
  (void)app_check_hal("adc", status);

  status = MX_TIM4_Init();
  if (app_check_hal("tim4", status) == 0U)
  {
    /* 没有节拍则按键/旋钮都不会被采样, 必须显式暴露 */
    SignalLog_Push(SIGLOG_ERROR, "no tick!");
  }

  {
    SignalStatus out_status = SignalOutput_Init();

    if (out_status == SIG_OK)
    {
      SignalLog_Push(SIGLOG_INFO, "out init ok");
    }
    else
    {
      SignalLog_Printf(SIGLOG_ERROR, "out %s", SignalStatus_Text(out_status));
    }
  }

  Knob_Init();
  Button_Init();

  UI_Init();
  SignalLog_Push(SIGLOG_INFO, "ui init ok");

  Menu_Init();
}

/* ========================== 节拍 ========================== */

void App_Tick(void)
{
  /*
   * 中断上下文: 只做计数与置标志.
   * 任何 I2C / 格式化 / 字符串操作都在这里被禁止, 否则会拖长中断.
   */
  s_tick_count++;
  s_tick_pending = 1U;
}

uint32_t App_GetTickCount(void)
{
  return s_tick_count;
}

uint32_t App_GetLoopCount(void)
{
  return s_loop_count;
}

/* ========================== 主循环 ========================== */

/**
 * @brief 执行一次 1ms 控制迭代
 * @note  只在主循环上下文调用.
 */
static void app_control_step(void)
{
  ButtonEvent ev;

  /* 1) 按键: 每 1ms 喂一次电平, 再把排队事件全部取空 */
  Button_Update(app_button_pressed());
  for (;;)
  {
    ev = Button_Poll();
    if (ev == BTN_EVENT_NONE)
    {
      break;
    }
    Menu_HandleEvent(ev);
  }

  /* 2) 旋钮: 采样 + 滤波, 仅在档位有效变化时才驱动菜单 */
  Knob_Update();
  if (Knob_Changed() != 0U)
  {
    Menu_HandleKnob(Knob_GetLevel());
  }

  /* 3) 界面: 按 UI_REFRESH_MS 限速, 避免 I2C 阻塞拖慢控制节奏 */
  s_ui_ms++;
  if (s_ui_ms >= UI_REFRESH_MS)
  {
    s_ui_ms = 0U;
    UI_Refresh();
  }

  /* 4) 心跳指示: 每 500 个节拍翻转一次 LED, 用于确认主循环真的在跑 */
  if ((s_loop_count % 500U) == 0U)
  {
    if ((s_loop_count % 1000U) == 0U)
    {
      LED_ON();
    }
    else
    {
      LED_OFF();
    }
  }
}

void App_Loop(void)
{
  uint8_t pending;

  /* 原子地取出并清除待处理标志(该标志由中断置位) */
  __disable_irq();
  pending        = s_tick_pending;
  s_tick_pending = 0U;
  __enable_irq();

  if (pending == 0U)
  {
    return;
  }

  app_control_step();
  s_loop_count++;
}
