/**
 * @file signal_output.h
 * @brief 信号输出硬件层: TIM3_CH1 + DMA1_Channel3
 * @note  本层是"协议无关"的: 它只接受 SignalPlan(已校验的纯计算结果),
 *        然后配置 PSC/ARR/CCR 与 DMA. 新增协议不需要改动本层.
 *
 * 硬件路径:
 *   PWM / OneShot -> ARR 决定周期, CCR 决定高电平, 不使用 DMA
 *   DShot         -> ARR = 位周期, DMA 逐位把该位的比较值写入 CCR
 *
 * 切换协议是"先算后写": BuildPlan 失败时一个寄存器都不动,
 * 因此失败不会破坏当前正在输出的信号.
 */
#ifndef __SIGNAL_OUTPUT_H
#define __SIGNAL_OUTPUT_H

#include <stdint.h>
#include "signal_protocol.h"

/** 输出通道状态 */
typedef enum
{
  SIGOUT_STATE_IDLE = 0, /* 未初始化或已停止 */
  SIGOUT_STATE_READY,    /* 已配置并可输出 */
  SIGOUT_STATE_ERROR     /* 初始化失败 */
} SignalOutputState;

/** 输出层运行信息, 供界面显示真实生效值 */
typedef struct
{
  SignalOutputState     state;
  const SignalProtocol *active_proto;  /* 当前生效协议 */
  uint32_t              param_values[SIG_MAX_PARAMS];
  uint32_t              actual_hz;     /* 实际频率(DShot 为帧率) */
  uint32_t              period_us;     /* 实际周期 */
  uint16_t              psc;
  uint16_t              arr;
  uint16_t              ccr;
  uint16_t              dma_len;       /* DShot 帧长度 */
  uint8_t               use_dma;       /* 当前是否走 DMA 路径 */
  SignalStatus          last_error;    /* 最近一次失败原因 */
} SignalOutputInfo;

/**
 * @brief 初始化输出层(TIM3_CH1 + DMA1_Channel3)
 * @return SIG_OK 或失败原因
 * @note  初始化完成后输出为低电平空闲态, 不产生脉冲.
 */
SignalStatus SignalOutput_Init(void);

/**
 * @brief 解析并切换到指定协议/参数
 * @param proto        目标协议
 * @param param_values 参数数组(长度 >= proto->param_count)
 * @return SIG_OK 或失败原因
 * @note  失败时硬件保持原状, 当前信号继续输出; last_error 记录原因.
 *        这是"回退"语义: 失败 = 不切换, 而不是切到半成品状态.
 */
SignalStatus SignalOutput_Select(const SignalProtocol *proto, const uint32_t *param_values);

/**
 * @brief 在当前协议下更新参数(不改变协议种类)
 * @note  用于编辑态旋钮/按键调参. PWM/OneShot 只改 CCR(轻量);
 *        DShot 需重建帧并重挂 DMA.
 */
SignalStatus SignalOutput_Apply(const uint32_t *param_values);

/** 停止输出并释放 DMA, 引脚回到空闲低电平 */
void SignalOutput_Stop(void);

/** 读取当前输出信息(只读快照) */
const SignalOutputInfo *SignalOutput_GetInfo(void);

/** 当前生效协议; 未配置时返回 NULL */
const SignalProtocol *SignalOutput_ActiveProto(void);

/**
 * @brief 校验协议在此硬件上是否可用, 不改变任何状态
 * @note  菜单在列出协议时用它与 SignalProtocol_Probe 一起过滤不可选项.
 */
SignalStatus SignalOutput_Probe(const SignalProtocol *proto);

/** DMA 帧缓冲容量(位槽数), 供 BuildPlan 校验 */
uint16_t SignalOutput_DmaCapacity(void);

#endif /* __SIGNAL_OUTPUT_H */
