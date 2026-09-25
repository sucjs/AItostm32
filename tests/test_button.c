/**
 * @file test_button.c
 * @brief 按键消抖与短按/长按/重复事件的宿主机单元测试
 * @note  按键模块不引用任何 STM32 头文件, 因此可以在 PC 上直接验证时序逻辑.
 *        这里以"每毫秒喂一次电平"的方式模拟真实控制循环的调用节奏.
 */
#include <stdio.h>
#include "button.h"
#include "app_config.h"

static int g_fail;

#define CHECK(cond, ...)                            \
  do {                                              \
    if (!(cond)) {                                  \
      g_fail++;                                     \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
    }                                               \
  } while (0)

/* 事件收集: 每毫秒 Button_Update 之后立刻取样, 模拟主循环的排空方式 */
#define EVENT_MAX 8192

static ButtonEvent g_events[EVENT_MAX];
static int         g_event_count;

static int count_of(ButtonEvent kind)
{
  int i;
  int n = 0;

  for (i = 0; i < g_event_count; i++)
  {
    if (g_events[i] == kind)
    {
      n++;
    }
  }
  return n;
}

static void reset_events(void)
{
  g_event_count = 0;
}

/** 以固定电平喂 ms 毫秒, 并同步排空事件 */
static void feed(int ms, uint8_t level)
{
  int i;

  for (i = 0; i < ms; i++)
  {
    ButtonEvent ev;

    Button_Update(level);
    ev = Button_Poll();
    if (ev != BTN_EVENT_NONE)
    {
      if (g_event_count < EVENT_MAX)
      {
        g_events[g_event_count] = ev;
        g_event_count++;
      }
    }
  }
}

/** 喂 ms 毫秒的抖动脉冲(逐毫秒与 start 交替), 模拟机械触点弹跳 */
static void feed_bounce(int ms, uint8_t start)
{
  int i;

  for (i = 0; i < ms; i++)
  {
    feed(1, (uint8_t)((start + (uint8_t)i) % 2U));
  }
}

/* ========================== 测试项 ========================== */

/** 抖动不得产生任何事件: 一次干净的短按只应产出 1 个 SHORT */
static void test_debounce_single_short(void)
{
  printf("[debounce] 抖动输入应只产生一次 SHORT\n");
  Button_Init();
  reset_events();

  /* 按下沿抖动 12ms(反复跳变), 之后稳定按下 30ms */
  feed_bounce(12, 1U);
  feed(30, 1U);

  /* 松开沿同样抖动, 再稳定松开 30ms */
  feed_bounce(12, 0U);
  feed(30, 0U);

  CHECK(g_event_count == 1, "一次抖动按键应只产生 1 个事件, 实际 %d 个", g_event_count);
  CHECK(count_of(BTN_EVENT_SHORT) == 1, "应产生 1 个 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));
  CHECK(count_of(BTN_EVENT_LONG) == 0, "不应产生 LONG");
  CHECK(count_of(BTN_EVENT_REPEAT) == 0, "不应产生 REPEAT");
}

/** 消抖时长本身: 稳定不足 BTN_DEBOUNCE_MS 的电平不得被确认 */
static void test_debounce_threshold(void)
{
  printf("[debounce threshold] 未稳定满 %u ms 不应确认\n", (unsigned)BTN_DEBOUNCE_MS);
  Button_Init();
  reset_events();

  /* 先确认一次按下 */
  feed(40, 1U);

  /*
   * 每隔 BTN_DEBOUNCE_MS-1 毫秒翻转一次电平:
   * 稳定时间永远差 1ms 达不到阈值, 因此不应产生任何松开事件.
   */
  feed((int)BTN_DEBOUNCE_MS - 1, 0U);
  feed((int)BTN_DEBOUNCE_MS - 1, 1U);
  feed((int)BTN_DEBOUNCE_MS - 1, 0U);
  feed((int)BTN_DEBOUNCE_MS - 1, 1U);

  CHECK(g_event_count == 0, "电平每次都在阈值前翻转, 不应产生事件, 实际 %d 个",
        g_event_count);

  /* 真正稳定松开后应立即产生 SHORT */
  feed(40, 0U);
  CHECK(count_of(BTN_EVENT_SHORT) == 1, "稳定松开后应产生 1 个 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));
}

/** 短按: 松开沿产生 SHORT, 且不产生 LONG/REPEAT */
static void test_short_press(void)
{
  printf("[short] 短按应产生 SHORT\n");
  Button_Init();
  reset_events();

  /* 按住 300ms(< BTN_LONGPRESS_MS), 再松开 */
  feed(300, 1U);
  feed(40, 0U);

  CHECK(count_of(BTN_EVENT_SHORT) == 1, "应产生 1 个 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));
  CHECK(count_of(BTN_EVENT_LONG) == 0, "短按不应产生 LONG, 实际 %d",
        count_of(BTN_EVENT_LONG));
  CHECK(count_of(BTN_EVENT_REPEAT) == 0, "短按不应产生 REPEAT, 实际 %d",
        count_of(BTN_EVENT_REPEAT));
}

/** 长按: 松开沿产生 LONG, 保持期间周期性 REPEAT */
static void test_long_press(void)
{
  int repeats;

  printf("[long] 长按应产生 LONG 与周期性 REPEAT\n");
  Button_Init();
  reset_events();

  /*
   * 按住远超长按阈值的时长.
   * REPEAT 从"越过 BTN_LONGPRESS_MS 之后"才开始计数, 首个 REPEAT 出现在
   * 松开之前的最后一个 BTN_REPEAT_MS 窗口内, 故预期次数为
   * (按住时长 - BTN_LONGPRESS_MS) / BTN_REPEAT_MS 附近, 这里只做下界检查.
   */
  feed((int)BTN_LONGPRESS_MS * 2, 1U);
  repeats = count_of(BTN_EVENT_REPEAT);

  CHECK(repeats >= 2, "长按 %u ms 应产生多个 REPEAT, 实际 %d",
        (unsigned)(BTN_LONGPRESS_MS * 2U), repeats);

  /* 长按期间不得提前发出 SHORT */
  CHECK(count_of(BTN_EVENT_SHORT) == 0, "长按期间不应产生 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));

  /* 松开 */
  feed(40, 0U);

  CHECK(count_of(BTN_EVENT_LONG) == 1, "长按松开应产生 1 个 LONG, 实际 %d",
        count_of(BTN_EVENT_LONG));
  CHECK(count_of(BTN_EVENT_SHORT) == 0, "长按不应产生 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));
}

/** 刚过长按阈值即松开: 仍应算 LONG, 且此时还没到 REPEAT 的间隔 */
static void test_long_press_boundary(void)
{
  printf("[long boundary] 恰好越过阈值即松开应判为 LONG\n");
  Button_Init();
  reset_events();

  /* 稳定按下 BTN_DEBOUNCE_MS 之后才开始累计 hold, 故多给一点余量 */
  feed((int)BTN_LONGPRESS_MS + (int)BTN_DEBOUNCE_MS + 5, 1U);
  feed(40, 0U);

  CHECK(count_of(BTN_EVENT_LONG) == 1, "越过阈值应判 LONG, 实际 %d",
        count_of(BTN_EVENT_LONG));
  CHECK(count_of(BTN_EVENT_SHORT) == 0, "不应判为 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));
}

/** 连击: 三次独立短按应产生 3 个 SHORT */
static void test_multiple_presses(void)
{
  printf("[multi] 连续三次短按应产生 3 个 SHORT\n");
  Button_Init();
  reset_events();

  feed(60, 1U);
  feed(60, 0U);
  feed(60, 1U);
  feed(60, 0U);
  feed(60, 1U);
  feed(60, 0U);

  CHECK(count_of(BTN_EVENT_SHORT) == 3, "应产生 3 个 SHORT, 实际 %d",
        count_of(BTN_EVENT_SHORT));
  CHECK(g_event_count == 3, "不应有其它事件, 实际总数 %d", g_event_count);
}

/** Poll 必须"读后即清", 同一事件不得被重复取走 */
static void test_poll_clears(void)
{
  printf("[poll] 事件取出后必须清除\n");
  Button_Init();
  reset_events();

  feed(60, 1U);
  feed(60, 0U);

  CHECK(Button_Poll() == BTN_EVENT_NONE, "事件已被取走, 再次 Poll 应为 NONE");

  /* 静止输入不应产生新事件 */
  feed(200, 0U);
  CHECK(Button_Poll() == BTN_EVENT_NONE, "静止输入不应产生事件");
}

int main(void)
{
  printf("=== button 单元测试 ===\n");

  test_debounce_single_short();
  test_debounce_threshold();
  test_short_press();
  test_long_press();
  test_long_press_boundary();
  test_multiple_presses();
  test_poll_clears();

  printf("=== %s (失败 %d 项) ===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return (g_fail == 0) ? 0 : 1;
}
