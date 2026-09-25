/**
 * @file signal_log.c
 * @brief 轻量环形日志实现
 */
#include "signal_log.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* 日志环形缓冲. head 指向下一个待写入位置, count 为已用条数 */
static SignalLogEntry s_log[SIGLOG_CAPACITY];
static uint8_t        s_head;
static uint8_t        s_count;
static uint8_t        s_seq;

void SignalLog_Init(void)
{
  memset(s_log, 0, sizeof(s_log));
  s_head  = 0U;
  s_count = 0U;
  s_seq   = 0U;
}

void SignalLog_Push(SignalLogLevel level, const char *msg)
{
  SignalLogEntry *slot;

  if (msg == NULL)
  {
    return;
  }

  slot = &s_log[s_head];
  slot->level = level;
  slot->seq   = s_seq++;

  /* strncpy 在源串过长时不补 0, 因此手工保证结尾 */
  strncpy(slot->msg, msg, SIGLOG_MSG_LEN - 1U);
  slot->msg[SIGLOG_MSG_LEN - 1U] = '\0';

  s_head = (uint8_t)((s_head + 1U) % SIGLOG_CAPACITY);
  if (s_count < SIGLOG_CAPACITY)
  {
    s_count++;
  }
}

void SignalLog_Printf(SignalLogLevel level, const char *fmt, ...)
{
  char    buf[SIGLOG_MSG_LEN];
  va_list args;

  if (fmt == NULL)
  {
    return;
  }

  va_start(args, fmt);
  (void)vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  SignalLog_Push(level, buf);
}

const SignalLogEntry *SignalLog_At(uint8_t index)
{
  uint8_t pos;

  if (index >= s_count)
  {
    return NULL;
  }

  /* 从 head 往前回退 index 条 */
  pos = (uint8_t)((s_head + SIGLOG_CAPACITY - 1U - index) % SIGLOG_CAPACITY);
  return &s_log[pos];
}

const SignalLogEntry *SignalLog_Latest(void)
{
  return SignalLog_At(0U);
}

uint8_t SignalLog_Count(void)
{
  return s_count;
}
