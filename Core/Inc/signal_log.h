/**
 * @file signal_log.h
 * @brief 轻量环形日志, 用于反馈协议切换结果与错误原因
 * @note  本模块不依赖硬件, 可在宿主机上单独编译测试.
 *        日志内容由界面读取, 后续接入串口时无需改动写入方.
 */
#ifndef __SIGNAL_LOG_H
#define __SIGNAL_LOG_H

#include <stdint.h>
#include "app_config.h"

/** 日志级别 */
typedef enum
{
  SIGLOG_INFO = 0, /* 正常事件, 例如协议切换成功 */
  SIGLOG_WARN,     /* 可继续但需注意 */
  SIGLOG_ERROR     /* 操作失败 */
} SignalLogLevel;

/** 单条日志 */
typedef struct
{
  SignalLogLevel level;
  uint8_t        seq;                     /* 递增序号, 便于判重 */
  char           msg[SIGLOG_MSG_LEN];
} SignalLogEntry;

/** 清空日志 */
void SignalLog_Init(void);

/**
 * @brief 写入一条日志
 * @param level 级别
 * @param msg   以 0 结尾的字符串, 超长会被截断
 */
void SignalLog_Push(SignalLogLevel level, const char *msg);

/**
 * @brief 格式化并写入一条日志
 * @note  超长会被截断; 格式串不可为空
 */
void SignalLog_Printf(SignalLogLevel level, const char *fmt, ...);

/**
 * @brief 取最新一条日志
 * @return 日志指针; 日志为空时返回 NULL
 */
const SignalLogEntry *SignalLog_Latest(void);

/**
 * @brief 按时间倒序取日志
 * @param index 0 为最新
 * @return 日志指针; 越界时返回 NULL
 */
const SignalLogEntry *SignalLog_At(uint8_t index);

/** 当前已保存的日志条数(不超过 SIGLOG_CAPACITY) */
uint8_t SignalLog_Count(void);

#endif /* __SIGNAL_LOG_H */
