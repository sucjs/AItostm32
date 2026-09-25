/**
 * @file menu.c
 * @brief 菜单状态机实现(协议选择 + 逐参数编辑)
 *
 * 交互模型(单按键, 详见 menu.h 的说明):
 *   BROWSE + SHORT  焦点在协议项 -> 切换到下一个可用协议(循环)
 *                   焦点在参数项 -> 焦点移到下一项(循环, 会绕回协议项)
 *   BROWSE + LONG   焦点在协议项 -> 焦点下移到第一个参数项
 *                   焦点在参数项 -> 进入 EDIT
 *   EDIT   + SHORT  提交当前值并停留在 EDIT(便于继续微调)
 *   EDIT   + LONG   提交当前值并返回 BROWSE
 *   REPEAT          一律忽略: 长按的"进入/退出"由松开沿的 LONG 完成,
 *                   保持期间不需要中间动作, 避免同一个长按产生多次状态跳变.
 *
 * 每协议一套参数: 切换协议时按新协议默认值重建参数数组.
 */
#include "menu.h"
#include "app_config.h"
#include "signal_log.h"

#include <string.h>

/* ========================== 常量 ========================== */

/** 旋钮档位满量程对应的最大步进数(对数步进参数用) */
#define MENU_KNOB_LOG_STEPS 64U

/* ========================== 状态 ========================== */

static const SignalProtocol *s_proto;      /* 当前协议; NULL 表示未选定 */
static uint8_t               s_proto_index;/* 当前协议在注册表中的索引 */
static uint32_t              s_param[SIG_MAX_PARAMS]; /* 当前参数值(每协议独立) */
static MenuMode              s_mode;       /* 当前交互模式 */
static uint8_t               s_focus;      /* 当前焦点项(MenuItemId) */

/* 旋钮锚点: 进入 EDIT 后第一次收到旋钮值时以此建立基准, 避免参数被绝对位置拽走 */
static uint8_t  s_knob_ref_valid;
static uint16_t s_knob_ref;

/** 最近一次操作结果, 供 UI 底部状态行显示 */
static char s_status[MENU_STATUS_LEN];

/* ========================== 内部工具 ========================== */

/** 状态行文本置空 */
static void menu_status_reset(void)
{
  s_status[0] = '\0';
}

/** 追加一段 ASCII 文本, 超长自动截断(状态行宽度有限) */
static void menu_status_append(const char *text)
{
  size_t len = strlen(s_status);

  if (text == NULL)
  {
    return;
  }

  while ((*text != '\0') && (len < (MENU_STATUS_LEN - 1U)))
  {
    s_status[len] = *text;
    len++;
    text++;
  }
  s_status[len] = '\0';
}

/** 组装 "PREFIX BODY" 形式的状态文本 */
static void menu_status_set(const char *prefix, const char *body)
{
  menu_status_reset();
  menu_status_append(prefix);
  menu_status_append(body);
}

/** 组装 "PREFIX BODY SUFFIX" 形式的状态文本 */
static void menu_status_set3(const char *prefix, const char *body, const char *suffix)
{
  menu_status_reset();
  menu_status_append(prefix);
  menu_status_append(body);
  menu_status_append(suffix);
}

/** 当前菜单项总数 = 1 个协议项 + 参数项, 上限 MENU_MAX_ITEMS */
static uint8_t menu_item_count(void)
{
  uint8_t count = 1U;

  if (s_proto != NULL)
  {
    count = (uint8_t)(1U + s_proto->param_count);
  }

  if (count > MENU_MAX_ITEMS)
  {
    count = MENU_MAX_ITEMS;
  }

  return count;
}

/** 菜单项 -> 参数槽; 协议项无参数槽, 返回无效值 */
static uint8_t menu_item_param_index(uint8_t item)
{
  if ((item == MENU_ITEM_PROTOCOL) || (item < 1U))
  {
    return 0xFFU;
  }
  return (uint8_t)(item - 1U);
}

/**
 * @brief 计算某个参数"每步进一次"所需的旋钮档位位移阈值
 * @note  目标是让一次完整旋钮行程大致覆盖整个参数范围:
 *          - 线性参数: 步数 = (max-min)/step, 阈值 = KNOB_LEVEL_MAX/步数
 *            (量程相对步长越大, 阈值越小, 保证量程可走完)
 *          - 对数参数(step 随值增长): 步数随量程按对数增长, 取固定经验值
 *        阈值至少为 1, 否则参数永远无法被旋钮推动.
 */
static uint16_t menu_knob_threshold(const SignalParam *param)
{
  uint32_t steps;
  uint32_t threshold;

  if ((param == NULL) || (param->max <= param->min))
  {
    return 1U;
  }

  if (param->log_step != 0U)
  {
    steps = MENU_KNOB_LOG_STEPS;
  }
  else
  {
    uint32_t step = (param->step == 0U) ? 1U : param->step;

    steps = (param->max - param->min) / step;
    if (steps == 0U)
    {
      steps = 1U;
    }
  }

  threshold = KNOB_LEVEL_MAX / steps;
  if (threshold == 0U)
  {
    threshold = 1U;
  }

  return (uint16_t)threshold;
}

/** 把当前参数数组推给硬件层, 并按结果更新状态行与日志 */
static void menu_apply_params(void)
{
  SignalStatus status;

  status = SignalOutput_Apply(s_param);
  if (status == SIG_OK)
  {
    menu_status_set("SET ", SignalStatus_Text(status));
  }
  else
  {
    /*
     * 应用失败: 硬件层保持上一次成功配置, 因此这里只需如实反馈失败原因,
     * 不需要回滚参数数组 —— 参数值本身仍保留, 用户可继续朝反方向调回可用区间.
     */
    menu_status_set3("ERR ", SignalStatus_Text(status), "");
    SignalLog_Printf(SIGLOG_ERROR, "apply %s", SignalStatus_Text(status));
  }
}

/**
 * @brief 记录一次"用户提交参数"的日志
 * @note  格式形如 "set PWM 1000 500", 内容足够短以适配 21 字符的日志行.
 *        只写关键信息(协议名 + 参数值), 便于在屏幕上直接读出提交结果.
 */
static void menu_log_commit(void)
{
  const char *name = (s_proto != NULL) ? s_proto->name : "?";

  /*
   * 只带一个参数值即可覆盖所有协议: PWM 频率/占空比、OneShot 脉宽、
   * DShot 油门都落在 s_param[0], 且 21 字符内能完整显示.
   */
  SignalLog_Printf(SIGLOG_INFO, "set %s %lu", name, (unsigned long)s_param[0]);
}

/** 焦点在参数项时, 按方向步进一次并立即应用 */
static void menu_step_param(int8_t direction)
{
  uint8_t            idx = menu_item_param_index(s_focus);
  const SignalParam *param;
  uint32_t           next;

  if (s_proto == NULL)
  {
    return;
  }

  param = SignalProtocol_ParamAt(s_proto, idx);
  if (param == NULL)
  {
    return;
  }

  next = SignalProtocol_StepParam(s_proto, idx, s_param[idx], direction);
  if (next != s_param[idx])
  {
    s_param[idx] = next;
    menu_apply_params();
  }
}

/* ========================== 协议切换 ========================== */

void Menu_SelectProtocol(uint8_t proto_index)
{
  const SignalProtocol *proto = SignalProtocol_At(proto_index);
  uint32_t              params[SIG_MAX_PARAMS] = {0};
  uint8_t               i;
  SignalStatus          status;

  if (proto == NULL)
  {
    menu_status_set3("ERR ", SignalStatus_Text(SIG_ERR_UNKNOWN_ID), "");
    SignalLog_Push(SIGLOG_ERROR, "no such proto");
    return;
  }

  /* 每协议一套参数: 一律按新协议的默认值重建, 不继承旧协议的值 */
  for (i = 0U; (i < proto->param_count) && (i < SIG_MAX_PARAMS); i++)
  {
    params[i] = SignalProtocol_DefaultParam(proto, i);
  }

  status = SignalOutput_Select(proto, params);
  if (status != SIG_OK)
  {
    /*
     * 失败保持原协议: 硬件层是"先算后写", 此刻仍在输出旧信号,
     * 因此这里不动 s_proto / s_param, 界面与硬件不会失配.
     */
    menu_status_set3("ERR ", SignalStatus_Text(status), "");
    SignalLog_Printf(SIGLOG_ERROR, "sw %s %s", proto->name, SignalStatus_Text(status));
    return;
  }

  s_proto       = proto;
  s_proto_index = proto_index;
  for (i = 0U; i < SIG_MAX_PARAMS; i++)
  {
    s_param[i] = (i < proto->param_count) ? params[i] : 0U;
  }

  /* 新协议参数个数可能更少, 把焦点夹回合法范围 */
  if (s_focus >= menu_item_count())
  {
    s_focus = MENU_ITEM_PROTOCOL;
  }

  /* 参数值整体换了, 旋钮锚点必须重建, 否则首次旋动会误步进 */
  s_knob_ref_valid = 0U;

  menu_status_set3("SW ", proto->name, " OK");
  SignalLog_Printf(SIGLOG_INFO, "sw %s ok", proto->name);
}

/**
 * @brief 在浏览态按协议项时, 切换到下一个"可用"协议
 * @note  用 SignalOutput_Probe 过滤掉本硬件跑不了的协议, 用户不会落到不可用档位;
 *        全部不可用时保持原位并记录错误.
 */
static void menu_cycle_protocol(void)
{
  uint8_t count = SignalProtocol_Count();
  uint8_t offset;
  uint8_t base  = (s_proto == NULL) ? 0U : s_proto_index;

  if (count == 0U)
  {
    menu_status_set3("ERR ", SignalStatus_Text(SIG_ERR_UNSUPPORTED), "");
    return;
  }

  for (offset = 1U; offset <= count; offset++)
  {
    uint8_t               candidate = (uint8_t)((base + offset) % count);
    const SignalProtocol *proto     = SignalProtocol_At(candidate);

    if (proto == NULL)
    {
      continue;
    }
    if (SignalOutput_Probe(proto) != SIG_OK)
    {
      continue;
    }

    Menu_SelectProtocol(candidate);
    return;
  }

  menu_status_set("ERR ", "NO USABLE");
  SignalLog_Push(SIGLOG_ERROR, "no usable proto");
}

/* ========================== 初始化 ========================== */

void Menu_Init(void)
{
  uint8_t count = SignalProtocol_Count();
  uint8_t i;
  uint8_t target   = 0U;
  uint8_t found    = 0U;

  s_proto          = NULL;
  s_proto_index    = 0U;
  s_mode           = MENU_MODE_BROWSE;
  s_focus          = MENU_ITEM_PROTOCOL;
  s_knob_ref_valid = 0U;
  s_knob_ref       = 0U;

  menu_status_reset();

  /*
   * 默认协议选标准 PWM(id=1): 它只用定时器不用 DMA, 参数范围最宽,
   * 上电即可输出, 是最安全的默认值. 找不到 PWM 时退回注册表首项, 保证
   * 总有一个协议被选中而不是停在"无协议"状态.
   */
  for (i = 0U; i < count; i++)
  {
    const SignalProtocol *proto = SignalProtocol_At(i);

    if ((proto != NULL) && (proto->id == 1U))
    {
      target = i;
      found  = 1U;
      break;
    }
  }

  if (found == 0U)
  {
    target = 0U; /* 无 PWM 时用首项 */
  }

  if (count > 0U)
  {
    Menu_SelectProtocol(target);
  }
  else
  {
    menu_status_set3("ERR ", SignalStatus_Text(SIG_ERR_UNSUPPORTED), "");
    SignalLog_Push(SIGLOG_ERROR, "no proto");
  }

  /* 初始化后焦点回到协议项, 从"选择协议"开始 */
  s_focus = MENU_ITEM_PROTOCOL;
}

/* ========================== 事件处理 ========================== */

void Menu_HandleEvent(ButtonEvent ev)
{
  switch (ev)
  {
    case BTN_EVENT_SHORT:
      if (s_mode == MENU_MODE_BROWSE)
      {
        if (s_focus == MENU_ITEM_PROTOCOL)
        {
          menu_cycle_protocol();
        }
        else
        {
          /* 参数项: 焦点前移一项, 到末尾后绕回协议项 */
          s_focus = (uint8_t)((s_focus + 1U) % menu_item_count());
        }
      }
      else
      {
        /*
         * 编辑态短按 = 立即提交. 提交时补一条日志, 因为参数在旋钮过程中已经
         * 生效(OUT 行会跟着变), 这里记下"用户确认了这次修改", 使底部状态行
         * 也能反映提交动作, 而不是只有失败才有反馈.
         */
        menu_apply_params();
        menu_log_commit();
      }
      break;

    case BTN_EVENT_LONG:
      if (s_mode == MENU_MODE_BROWSE)
      {
        if (s_focus == MENU_ITEM_PROTOCOL)
        {
          /* 协议项没有可调数值, 长按下移到第一个参数项(无参数则留在原地) */
          if (menu_item_count() > 1U)
          {
            s_focus = MENU_ITEM_PARAM0;
          }
        }
        else
        {
          s_mode           = MENU_MODE_EDIT;
          s_knob_ref_valid = 0U; /* 以进入编辑后的第一个旋钮值为锚点 */
        }
      }
      else
      {
        /* 编辑态长按 = 提交并退回浏览态 */
        menu_apply_params();
        menu_log_commit();
        s_mode           = MENU_MODE_BROWSE;
        s_knob_ref_valid = 0U;
      }
      break;

    case BTN_EVENT_REPEAT:
    case BTN_EVENT_NONE:
    default:
      /* 长按保持期间不做额外动作, 避免同一长按产生多次状态跳变 */
      break;
  }
}

void Menu_HandleKnob(uint16_t level)
{
  const SignalParam *param;
  uint8_t            idx;
  uint16_t           threshold;
  int32_t            delta;

  /* 浏览态: 旋钮不改变任何参数, 避免误触 */
  if (s_mode != MENU_MODE_EDIT)
  {
    return;
  }

  idx = menu_item_param_index(s_focus);
  if (idx == 0xFFU)
  {
    return; /* 焦点在协议项, 无可调参数 */
  }

  param = SignalProtocol_ParamAt(s_proto, idx);
  if (param == NULL)
  {
    return;
  }

  if (s_knob_ref_valid == 0U)
  {
    /* 锚点: 记录进入编辑后的第一个档位, 之后只按"相对位移"步进 */
    s_knob_ref       = level;
    s_knob_ref_valid = 1U;
    return;
  }

  threshold = menu_knob_threshold(param);
  delta     = (int32_t)level - (int32_t)s_knob_ref;

  /*
   * 只按阈值的整数倍前进锚点, 余量留在 delta 之外继续累计,
   * 因此"每帧位移都不足一个阈值"的慢速微调最终仍会触发一次步进.
   */
  while (delta >= (int32_t)threshold)
  {
    menu_step_param(1);
    s_knob_ref = (uint16_t)(s_knob_ref + threshold);
    delta -= (int32_t)threshold;
  }

  while (delta <= -(int32_t)threshold)
  {
    menu_step_param(-1);
    s_knob_ref = (uint16_t)(s_knob_ref - threshold);
    delta += (int32_t)threshold;
  }
}

/* ========================== 只读访问 ========================== */

MenuMode Menu_GetMode(void)
{
  return s_mode;
}

uint8_t Menu_GetFocusItem(void)
{
  return s_focus;
}

const SignalProtocol *Menu_GetProtocol(void)
{
  return s_proto;
}

uint8_t Menu_GetProtocolIndex(void)
{
  return s_proto_index;
}

uint32_t Menu_GetParamValue(uint8_t idx)
{
  if (idx >= SIG_MAX_PARAMS)
  {
    return 0U;
  }
  return s_param[idx];
}

uint8_t Menu_GetParamCount(void)
{
  return (s_proto == NULL) ? 0U : s_proto->param_count;
}

const char *Menu_GetStatusText(void)
{
  return s_status;
}
