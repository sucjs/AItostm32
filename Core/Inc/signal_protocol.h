/**
 * @file signal_protocol.h
 * @brief 信号协议描述符、注册表与编码器(纯逻辑, 不依赖硬件)
 * @note  新增协议只需在协议表中追加一条描述符, 必要时实现编码分支;
 *        输出引擎与界面均按描述符驱动, 无需改动核心逻辑.
 *
 * 波形与硬件路径的对应关系:
 *   - SIG_WAVE_PWM      : 固定 ARR 决定周期, CCR 决定占空比 -> 只用定时器
 *   - SIG_WAVE_ONESHOT  : 同上. 本协议是"周期固定的 PWM", 只是参数以脉宽表达
 *   - SIG_WAVE_DSHOT    : 位周期固定, 每位的 CCR 不同 -> ARR 固定 + DMA 逐位写 CCR
 *
 * 已注册协议:
 *   标准 PWM, OneShot125, OneShot42, MultiShot, DShot150, DShot300, DShot600
 *
 * 关于 DShot1200: 72MHz 定时器下位周期 0.833us 仅 60 个计数,
 * "0" 位需要 3/8 位周期 = 22.5 个计数, 无法整数表达, 故不注册.
 * 注册表按 tick_per_unit % 8 == 0 校验, 保证 3/8 与 6/8 都是整数.
 */
#ifndef __SIGNAL_PROTOCOL_H
#define __SIGNAL_PROTOCOL_H
#include <stdint.h>
#include "app_config.h"
#include <stdint.h>


/** 单协议最多参数个数(PWM 为频率+占空比, 其余为 1) */
#define SIG_MAX_PARAMS 2U

/** 协议输出波形类型 */
typedef enum
{
  SIG_WAVE_PWM = 0,     /* 连续方波, 周期与占空比均可变 */
  SIG_WAVE_ONESHOT,     /* 固定周期单脉冲, 脉宽可变 */
  SIG_WAVE_DSHOT        /* 数字帧脉冲串 */
} SignalWaveType;

/** 参数项的物理含义, 决定界面单位与格式化方式 */
typedef enum
{
  SIG_PARAM_NONE = 0,
  SIG_PARAM_FREQ_HZ,        /* 频率, 单位 Hz */
  SIG_PARAM_DUTY_PCT_X10,   /* 占空比, 单位 0.1% (0..1000) */
  SIG_PARAM_PULSE_US,       /* 脉宽, 单位 us */
  SIG_PARAM_THROTTLE        /* DShot 油门值 (0..2047) */
} SignalParamKind;

/** 返回码. 0 表示成功, 负值为失败原因 */
typedef enum
{
  SIG_OK = 0,
  SIG_ERR_NULL_HANDLE = -1,      /* 传入空指针 */
  SIG_ERR_UNKNOWN_ID = -2,       /* 协议未注册 */
  SIG_ERR_TICK_TOO_FAST = -3,    /* 定时器分辨率不足以整数表达最小时间单位 */
  SIG_ERR_PERIOD_TOO_LONG = -4,  /* 所需周期超出定时器计数范围 */
  SIG_ERR_PERIOD_TOO_SHORT = -5, /* 所需周期小于定时器最小计数 */
  SIG_ERR_FRAME_TOO_LONG = -6,   /* 数字帧长度超出 DMA 缓冲 */
  SIG_ERR_UNSUPPORTED = -7,      /* 硬件或配置不支持该协议 */
  SIG_ERR_RANGE = -8,            /* 参数超出该协议允许范围 */
  SIG_ERR_NO_PARAM = -9          /* 协议未定义该参数槽 */
} SignalStatus;

/** 返回码转短字符串(不含符号), 供界面与日志显示 */
const char *SignalStatus_Text(SignalStatus status);

/**
 * @brief 单个参数槽的定义
 * @note  duty 的限幅逻辑即本结构的 min/max, 不需要额外字段.
 */
typedef struct
{
  SignalParamKind kind;
  uint32_t        min;            /* 允许下限 */
  uint32_t        max;            /* 允许上限 */
  uint32_t        default_value;  /* 首次切换到该协议时采用 */
  uint32_t        step;           /* 线性步进 */
  uint8_t         log_step;       /* 置 1 时步进随当前值按比例增大(对数手感) */
} SignalParam;

/** 协议描述符(只读常量表) */
typedef struct
{
  uint8_t        id;      /* 唯一 ID, 与注册顺序无关 */
  const char    *name;    /* 短名, 菜单显示用, 建议 <= 14 字符 */
  const char    *detail;  /* 详情, 状态行显示用 */
  SignalWaveType wave;    /* 波形类型 */

  /**
   * DShot 的位周期在定时器上的计数; 其它协议为 0.
   * 校验要求: 非 0 时必须能被 8 整除, 否则无法整数表达 3/8 与 6/8 位周期.
   */
  uint32_t tick_per_unit;
  uint8_t frame_bits;   /* DShot 数据位数(不含停止位); 其它协议为 0 */
  uint8_t frame_slots;  /* DShot 一帧占用的位槽总数(含数据位与填充); 其它为 0 */
 
  /** 固定周期(us). 0 表示由频率参数决定(标准 PWM); DShot 由位周期与位槽数推导 */
  uint32_t fixed_period_us;
  SignalParam params[SIG_MAX_PARAMS];
  uint8_t     param_count;
} SignalProtocol;

/**
 * @brief 一次可执行输出方案(由描述符 + 参数解析得到的纯计算结果)
 * @note  全部校验通过后才交给硬件层写寄存器; 失败时不会产生半成品配置.
 */
typedef struct
{
  const SignalProtocol *proto;

  uint32_t timer_hz;  /* 定时器输入时钟 */
  uint16_t psc;       /* 预分频 */
  uint16_t arr;       /* 自动重载 */
  uint16_t ccr;       /* PWM/OneShot: 唯一比较值; DShot: 不使用 */

  uint16_t ccr_low;   /* DShot: "0" 位比较值 */
  uint16_t ccr_high;  /* DShot: "1" 位比较值 */
  uint16_t dma_len;   /* DShot: DMA 传输长度 */
  uint8_t  frame_bits;/* DShot: 数据位数(不含停止位) */

  uint32_t param_values[SIG_MAX_PARAMS]; /* 本次生效的参数值 */
  uint32_t period_us;                    /* 实际周期(us) */
  uint32_t actual_hz;                    /* 实际频率(Hz), DShot 为帧率 */
} SignalPlan;
/** 已注册协议数量 */
uint8_t SignalProtocol_Count(void);

/** 按注册索引取描述符; 越界返回 NULL */
const SignalProtocol *SignalProtocol_At(uint8_t index);

/** 按 ID 取描述符; 不存在返回 NULL */
const SignalProtocol *SignalProtocol_FindById(uint8_t id);

/**
 * @brief 取参数槽定义
 * @return 槽指针; proto 为空或 index 越界返回 NULL
 */
const SignalParam *SignalProtocol_ParamAt(const SignalProtocol *proto, uint8_t index);

/** 协议在给定定时器时钟下是否可用(仅做静态能力判断) */
SignalStatus SignalProtocol_Probe(const SignalProtocol *proto, uint32_t timer_hz);

/** 参数夹取到协议允许范围 */
uint32_t SignalProtocol_ClampParam(const SignalProtocol *proto, uint8_t index, uint32_t value);

/**
 * @brief 按方向调整参数值(菜单编辑用)
 * @param proto     协议
 * @param index     参数槽
 * @param current   当前值
 * @param direction 正数增大, 负数减小
 * @return 调整并夹取后的值
 */
uint32_t SignalProtocol_StepParam(const SignalProtocol *proto, uint8_t index,
                                  uint32_t current, int8_t direction);

/** 取协议参数默认值; 槽无效时返回 0 */
uint32_t SignalProtocol_DefaultParam(const SignalProtocol *proto, uint8_t index);

/**
 * @brief 解析并校验运行参数, 生成可执行方案
 * @param proto        协议描述符
 * @param param_values 参数数组, 长度不小于 proto->param_count
 * @param timer_hz     定时器输入时钟(Hz)
 * @param dma_capacity DMA 帧缓冲容量(位槽数)
 * @param out          输出方案
 * @return SignalStatus
 * @note  本函数不触碰任何寄存器.
 */
SignalStatus SignalProtocol_BuildPlan(const SignalProtocol       *proto,
                                      const uint32_t             *param_values,
                                      uint32_t                    timer_hz,
                                      uint16_t                    dma_capacity,
                                      SignalPlan                 *out);

/**
 * @brief 计算 DShot 帧的 4 位 CRC
 * @param value 16 位帧内容(11 位油门 + 1 位遥测请求)
 * @return 4 位 CRC
 */
uint8_t SignalProtocol_DshotCrc(uint16_t value);

/**
 * @brief 把 DShot 帧编码为定时器比较值序列
 * @param plan     已解析方案
 * @param throttle 油门值(0..2047)
 * @param telemetry 非 0 则置遥测请求位
 * @param buf      输出缓冲, 长度需 >= plan->dma_len
 * @param capacity 缓冲容量
 * @return SignalStatus
 * @note  缓冲前 frame_bits 位为数据位, 第 frame_bits 位为停止位(恒 0),
 *        其余槽位填 0 以维持帧周期内的空闲低电平.
 */
SignalStatus SignalProtocol_EncodeDshotFrame(const SignalPlan *plan,
                                             uint16_t          throttle,
                                             uint8_t           telemetry,
                                             uint16_t         *buf,
                                             uint16_t          capacity);

/**
 * @brief 取 DShot 帧的 12 位原始内容(11 位油门 + 1 位遥测, 不含校验)
 * @return 12 位帧内容; 油门越界时夹到 2047
 */
uint16_t SignalProtocol_DshotRawFrame(uint16_t throttle, uint8_t telemetry);

/**
 * @brief 计算 DShot 帧的 4 位校验(12 位内容各半字节异或取低 4 位)
 * @param value 12 位帧内容(11 位油门 + 1 位遥测请求)
 * @return 4 位校验值, 将作为帧的最低 4 位发送
 * @note  这不是多项式 CRC, 而是半字节异或; 详见 .c 实现处的说明.
 */
uint8_t SignalProtocol_DshotCrc(uint16_t value);

#endif /* __SIGNAL_PROTOCOL_H */
