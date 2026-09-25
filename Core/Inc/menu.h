/**
 * @file menu.h
 * @brief 菜单状态机: 协议选择 + 逐参数编辑(每协议各自一套参数)
 * @note  主控只有一颗按键(规格固定), 因此协议选择与参数编辑必须共用同一交互:
 *
 *          浏览态(BROWSE)  短按 -> 焦点移到下一项(循环)
 *                          长按 -> 进入编辑态(焦点在参数项时)
 *          编辑态(EDIT)    短按 -> 立即提交并停留在编辑态(可继续微调)
 *                          长按 -> 提交并返回浏览态
 *
 *        即"长按"始终是进入/退出的开关, "短按"在浏览态负责切项、在编辑态负责提交.
 *        协议项本身没有可调数值, 故长按不进入编辑态(否则会出现旋钮无效的编辑态),
 *        协议切换由浏览态下对协议项的短按完成.
 *
 *        每协议一套参数(用户选择): 切换协议时按新协议默认值重建参数数组,
 *        旧协议的编辑结果不会带到新协议上.
 */
#ifndef __MENU_H
#define __MENU_H

#include <stdint.h>
#include "signal_protocol.h"
#include "signal_output.h"
#include "button.h"

/** 状态文本缓冲长度(ASCII, 含结尾 0) */
#define MENU_STATUS_LEN 21U

/** 菜单项 ID. 参数项自 MENU_ITEM_PARAM0 起按参数槽顺序排列 */
typedef enum
{
  MENU_ITEM_PROTOCOL = 0, /* 协议选择项 */
  MENU_ITEM_PARAM0,       /* 参数槽 0 */
  MENU_ITEM_PARAM1        /* 参数槽 1 */
} MenuItemId;

/** 交互模式 */
typedef enum
{
  MENU_MODE_BROWSE = 0, /* 只浏览/切项 */
  MENU_MODE_EDIT        /* 旋钮可改当前参数 */
} MenuMode;

/**
 * @brief 初始化菜单: 选取安全默认协议(PWM)并按默认值输出
 * @note  必须在 SignalOutput_Init 之后调用.
 */
void Menu_Init(void);

/**
 * @brief 处理按键事件
 * @param ev 事件(来自 Button_Poll)
 */
void Menu_HandleEvent(ButtonEvent ev);

/**
 * @brief 处理旋钮档位变化
 * @param level 归一化档位(0..KNOB_LEVEL_MAX)
 * @note  仅在编辑态且焦点为参数项时生效. 采用"相对位移"语义:
 *        进入编辑态时以当前旋钮位置为锚点, 之后按位移步进参数,
 *        因此进入编辑瞬间参数不会被旋钮的绝对位置"拽走".
 *        位移不足一个步进单位时会累计, 慢速微调同样能生效.
 */
void Menu_HandleKnob(uint16_t level);

/**
 * @brief 切换到指定注册索引的协议
 * @param proto_index SignalProtocol_At 的索引
 * @note  失败时保持原协议与原参数: 硬件层本就是"先算后写", 失败不产生半成品配置.
 */
void Menu_SelectProtocol(uint8_t proto_index);

/** 当前模式 */
MenuMode Menu_GetMode(void);

/** 当前焦点项(MenuItemId) */
uint8_t Menu_GetFocusItem(void);

/** 当前协议; 未选定时为 NULL */
const SignalProtocol *Menu_GetProtocol(void);

/** 当前协议在注册表中的索引 */
uint8_t Menu_GetProtocolIndex(void);

/** 第 idx 个参数槽的当前值; 越界返回 0 */
uint32_t Menu_GetParamValue(uint8_t idx);

/** 当前协议的参数个数 */
uint8_t Menu_GetParamCount(void);

/**
 * @brief 最近一次操作的简短结果(纯 ASCII, 供 OLED 显示)
 * @note  返回值始终非 NULL. 内容形如 "SW PWM OK" / "ERR RANGE".
 */
const char *Menu_GetStatusText(void);

#endif /* __MENU_H */
