/**
 * @file test_signal_log.c
 * @brief 日志环形缓冲宿主机单元测试
 * @note  日志是协议切换结果的唯一反馈通道, 其行为必须可验证:
 *        覆盖环绕、容量上限、超长截断、倒序读取与 seq 单调性.
 */
#include <stdio.h>
#include <string.h>
#include "signal_log.h"

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

static void test_empty(void)
{
  printf("[empty]\n");
  SignalLog_Init();

  CHECK(SignalLog_Count() == 0U, "初始应为空");
  CHECK(SignalLog_Latest() == NULL, "空日志 Latest 应为 NULL");
  CHECK(SignalLog_At(0U) == NULL, "空日志 At(0) 应为 NULL");
}

static void test_basic(void)
{
  printf("[basic]\n");
  SignalLog_Init();

  SignalLog_Push(SIGLOG_INFO, "SW DShot600 OK");
  CHECK(SignalLog_Count() == 1U, "应有 1 条, 实际 %u", SignalLog_Count());

  {
    const SignalLogEntry *e = SignalLog_Latest();

    CHECK(e != NULL, "Latest 不应为 NULL");
    CHECK(strcmp(e->msg, "SW DShot600 OK") == 0, "消息不符: '%s'", e->msg);
    CHECK(e->level == SIGLOG_INFO, "级别应为 INFO");
  }

  SignalLog_Push(SIGLOG_ERROR, "TICKFAST");
  {
    const SignalLogEntry *e = SignalLog_Latest();

    CHECK(strcmp(e->msg, "TICKFAST") == 0, "最新应为 TICKFAST, 实际 '%s'", e->msg);
    CHECK(e->level == SIGLOG_ERROR, "级别应为 ERROR");
  }
}

static void test_seq_monotonic(void)
{
  uint8_t i;

  printf("[seq]\n");
  SignalLog_Init();

  for (i = 0U; i < 5U; i++)
  {
    SignalLog_Printf(SIGLOG_INFO, "evt%d", i);
  }

  /* 倒序读取: At(0) 最新, At(4) 最旧 */
  for (i = 0U; i < 5U; i++)
  {
    const SignalLogEntry *e = SignalLog_At(i);

    CHECK(e != NULL, "At(%u) 不应为 NULL", i);
    if (e != NULL)
    {
      uint8_t want = (uint8_t)(4U - i);

      CHECK(e->seq == want, "At(%u) 期望 seq=%u, 实际 %u", i, want, e->seq);
    }
  }

  /* seq 必须严格单调递增, 便于界面判重 */
  {
    const SignalLogEntry *newest = SignalLog_At(0U);
    const SignalLogEntry *oldest = SignalLog_At(4U);

    CHECK(newest->seq == 4U, "最新 seq 应为 4, 实际 %u", newest->seq);
    CHECK(oldest->seq == 0U, "最旧 seq 应为 0, 实际 %u", oldest->seq);
  }
}

static void test_ring_wraparound(void)
{
  uint8_t i;

  printf("[wrap]\n");
  SignalLog_Init();

  /* 写入远超容量的条数, 验证只保留最新 SIGLOG_CAPACITY 条 */
  for (i = 0U; i < (uint8_t)(SIGLOG_CAPACITY + 5U); i++)
  {
    SignalLog_Printf(SIGLOG_INFO, "m%u", i);
  }

  CHECK(SignalLog_Count() == SIGLOG_CAPACITY,
        "容量应封顶为 %u, 实际 %u", SIGLOG_CAPACITY, SignalLog_Count());
  CHECK(SignalLog_At(SIGLOG_CAPACITY) == NULL, "越界读取应为 NULL");

  /* 最新的应是最后写入的那条 */
  {
    const SignalLogEntry *e = SignalLog_Latest();
    char                  want[16];

    snprintf(want, sizeof(want), "m%u", (unsigned)(SIGLOG_CAPACITY + 4U));
    CHECK(strcmp(e->msg, want) == 0, "最新应为 '%s', 实际 '%s'", want, e->msg);
  }

  /* 最旧的应是被保留窗口的第一条 */
  {
    const SignalLogEntry *e = SignalLog_At((uint8_t)(SIGLOG_CAPACITY - 1U));
    char                  want[16];

    snprintf(want, sizeof(want), "m5");
    CHECK(strcmp(e->msg, want) == 0, "最旧应为 '%s', 实际 '%s'", want, e->msg);
  }
}

static void test_truncation(void)
{
  char   long_msg[64];
  size_t i;

  printf("[trunc]\n");
  SignalLog_Init();

  for (i = 0U; i < sizeof(long_msg) - 1U; i++)
  {
    long_msg[i] = 'X';
  }
  long_msg[sizeof(long_msg) - 1U] = '\0';

  SignalLog_Push(SIGLOG_WARN, long_msg);

  {
    const SignalLogEntry *e = SignalLog_Latest();

    CHECK(e != NULL, "不应为 NULL");
    /* 必须始终以 0 结尾, 且长度不超过缓冲 */
    CHECK(strlen(e->msg) == (size_t)(SIGLOG_MSG_LEN - 1U),
          "超长消息应被截断到 %u 字符, 实际 %u",
          SIGLOG_MSG_LEN - 1U, (unsigned)strlen(e->msg));
    CHECK(e->msg[SIGLOG_MSG_LEN - 1U] == '\0', "结尾必须为 0");
  }
}

static void test_null_and_printf(void)
{
  printf("[null/printf]\n");
  SignalLog_Init();

  /* NULL 消息不得崩溃, 也不应写入 */
  SignalLog_Push(SIGLOG_ERROR, NULL);
  CHECK(SignalLog_Count() == 0U, "NULL 消息不应写入, 实际 %u", SignalLog_Count());

  SignalLog_Printf(SIGLOG_ERROR, NULL);
  CHECK(SignalLog_Count() == 0U, "NULL 格式串不应写入, 实际 %u", SignalLog_Count());

  /* 格式化写入 */
  SignalLog_Printf(SIGLOG_WARN, "SW %s ERR:%d", "DShot600", -3);
  {
    const SignalLogEntry *e = SignalLog_Latest();

    CHECK(e != NULL, "不应为 NULL");
    CHECK(strcmp(e->msg, "SW DShot600 ERR:-3") == 0, "格式化结果不符: '%s'", e->msg);
  }

  /* 重新初始化必须清空 */
  SignalLog_Init();
  CHECK(SignalLog_Count() == 0U, "Init 后应清空");
}

int main(void)
{
  printf("=== signal_log 单元测试 ===\n");

  test_empty();
  test_basic();
  test_seq_monotonic();
  test_ring_wraparound();
  test_truncation();
  test_null_and_printf();

  printf("=== %s (失败 %d 项) ===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return (g_fail == 0) ? 0 : 1;
}
