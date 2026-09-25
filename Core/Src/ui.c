/**
 * @file ui.c
 * @brief OLED 界面渲染实现
 *
 * 版面(128x64, 统一使用 afont8x6, 字宽 6px -> 每行最多 21 字符):
 *   y=0   : PROTO <协议名>            焦点在协议项时行首为 '>'
 *   y=12  : <参数值+单位> [EDIT|BROW] 行尾右对齐显示量程百分比
 *   y=24  : OUT <实际生效输出>        行尾右对齐显示同一百分比
 *   y=32  : <级别字母> <最新日志>     ASCII 过滤后
 *   UI_BAR_Y (40)  : 条形图, 描边 + 按比例填充
 *   UI_WAVE_Y (52): 历史波形折线
 *
 * 关于日志行放在 y=32 的原因:
 *   app_config.h 固定 UI_BAR_Y=40 / UI_WAVE_Y=52, 即 y=40..63 已被条形图与
 *   波形占满; 32..39 是条形图上方最后一段空闲的 8 像素, 而 8px 正是 afont8x6
 *   的字高, 因此日志行放这里既不重叠也不越界.
 *
 * 关于百分比画在文本行行尾的原因:
 *   UI_BAR_W=120 只给右侧留下 8px, 连 "%100%" 的 4 字符(24px)都放不下.
 *   于是把"焦点参数的量程百分比"同时画在 y=12 与 y=24 的行尾(右对齐),
 *   两行的正文因此限制在 15 字符(90px)以内, 与百分比字段互不重叠.
 */
#include "ui.h"
#include "app_config.h"
#include "oled.h"
#include "menu.h"
#include "knob.h"
#include "signal_output.h"
#include "signal_log.h"

#include "main.h"

/* ========================== 版面常量 ========================== */

/** 全部文本使用 8 高 6 宽字体: 128 / 6 = 21 字符 */
#define UI_FONT (&afont8x6)

#define UI_ROW1_Y 0U  /* 协议行 */
#define UI_ROW2_Y 12U /* 参数行 */
#define UI_ROW3_Y 24U /* 实际输出行 */
#define UI_ROW4_Y 32U /* 日志行 */

/** 正文安全宽度: 15 字符 * 6px = 90px, 右侧留给百分比 */
#define UI_TEXT_COLS 15U

/** 百分比字段右边界 x(不含) */
#define UI_PCT_RIGHT_X 126U

/** 波形相邻采样点的横向步长 */
#define UI_WAVE_STEP ((uint8_t)(UI_WAVE_W / UI_WAVE_POINTS))

/* ========================== 内部状态 ========================== */

/** 波形环形缓冲: 保存最近 UI_WAVE_POINTS 个归一化档位 */
static uint16_t s_wave[UI_WAVE_POINTS];
static uint8_t  s_wave_head; /* 下一个待写入位置 */
static uint8_t  s_wave_len;  /* 已填充点数 */

/* ========================== 文本工具 ========================== */

/**
 * @brief 把任意字符串过滤为可显示 ASCII 并截断
 * @param src  源串, 允许为 NULL
 * @param dst  目标缓冲, 容量必须 >= cols + 1
 * @param cols 最多保留字符数
 * @note  OLED 字库仅覆盖 0x20..0x7B, 越界字节会按 ch-' ' 索引到字库之外,
 *        读出的图案是乱码. 这里统一替换为 '.', 从根上杜绝越界索引.
 */
static void ui_text_sanitize(const char *src, char *dst, uint8_t cols)
{
  uint8_t n = 0U;

  if (src != NULL)
  {
    while ((*src != '\0') && (n < cols))
    {
      char ch = *src;

      if ((ch < 0x20) || (ch > 0x7B))
      {
        ch = '.';
      }
      dst[n] = ch;
      n++;
      src++;
    }
  }

  dst[n] = '\0';
}

/** 追加十进制无符号数(无前导 0), 返回新的写入位置 */
static uint8_t ui_append_u32(char *dst, uint8_t pos, uint8_t cols, uint32_t value)
{
  char    tmp[11];
  uint8_t len = 0U;
  uint8_t i;

  if (value == 0U)
  {
    tmp[0] = '0';
    len    = 1U;
  }
  else
  {
    while ((value > 0U) && (len < (uint8_t)sizeof(tmp)))
    {
      tmp[len] = (char)('0' + (value % 10U));
      len++;
      value /= 10U;
    }
  }

  /* 上面是低位到高位, 逆序写出 */
  for (i = len; i > 0U; i--)
  {
    if (pos < cols)
    {
      dst[pos] = tmp[i - 1U];
      pos++;
    }
  }
  dst[pos] = '\0';

  return pos;
}

/** 追加一段字面量, 返回新的写入位置 */
static uint8_t ui_append_str(char *dst, uint8_t pos, uint8_t cols, const char *text)
{
  if (text == NULL)
  {
    return pos;
  }

  while ((*text != '\0') && (pos < cols))
  {
    dst[pos] = *text;
    pos++;
    text++;
  }
  dst[pos] = '\0';

  return pos;
}

/**
 * @brief 在行尾右对齐绘制百分比
 * @param y        目标行的 y(与主文本同高)
 * @param permille 0..1000, 表示 0..100.0%
 * @note  OLED_PrintASCIIString 只能从左向右打印, 因此先算出字符串宽度,
 *        再反推起点实现右对齐. 字段宽度上限 4 字符("100%"), 放不下时截断左侧.
 */
static void ui_draw_percent(uint8_t y, uint16_t permille)
{
  char    buf[8];
  uint8_t pos = 0U;
  uint8_t len;

  pos = ui_append_u32(buf, pos, 6U, (uint32_t)(permille / 10U));
  pos = ui_append_str(buf, pos, 6U, "%");

  len = pos;
  if (len > 4U)
  {
    /* 只保留最右侧 4 个字符, 保证右端始终对齐 */
    buf[0] = buf[len - 4U];
    buf[1] = buf[len - 3U];
    buf[2] = buf[len - 2U];
    buf[3] = buf[len - 1U];
    len    = 4U;
  }
  buf[len] = '\0';

  OLED_PrintASCIIString((uint8_t)(UI_PCT_RIGHT_X - (uint8_t)(len * UI_FONT->w)), y,
                        buf, UI_FONT, OLED_COLOR_NORMAL);
}

/* ========================== 参数呈现 ========================== */

/**
 * @brief 按参数物理含义格式化数值
 * @param kind  参数种类
 * @param value 参数值(单位与 SignalParamKind 一致)
 * @param buf   输出缓冲
 * @param cols  最多字符数
 * @note  全部用整数运算: 需要一个小数位时按 value/10 与 value%10 拼装,
 *        避免在目标端引入浮点库(-Wall 下 float 格式化还会带来大量代码体积).
 */
static void ui_format_param(SignalParamKind kind, uint32_t value, char *buf, uint8_t cols)
{
  uint8_t pos = 0U;

  if ((buf == NULL) || (cols == 0U))
  {
    return;
  }
  buf[0] = '\0';

  switch (kind)
  {
    case SIG_PARAM_FREQ_HZ:
      pos = ui_append_u32(buf, pos, cols, value);
      (void)ui_append_str(buf, pos, cols, " Hz");
      break;

    case SIG_PARAM_DUTY_PCT_X10:
      pos = ui_append_u32(buf, pos, cols, value / 10U);
      pos = ui_append_str(buf, pos, cols, ".");
      pos = ui_append_u32(buf, pos, cols, value % 10U);
      (void)ui_append_str(buf, pos, cols, " %");
      break;

    case SIG_PARAM_PULSE_US:
      pos = ui_append_u32(buf, pos, cols, value);
      (void)ui_append_str(buf, pos, cols, " us");
      break;

    case SIG_PARAM_THROTTLE:
      pos = ui_append_u32(buf, pos, cols, value);
      (void)ui_append_str(buf, pos, cols, "");
      break;

    case SIG_PARAM_NONE:
    default:
      (void)ui_append_str(buf, 0U, cols, "--");
      break;
  }
}

/**
 * @brief 计算焦点参数在其 min..max 区间内的位置
 * @param permille 输出 0..1000
 * @return 1 = 焦点是参数项且可计算; 0 = 不可计算(焦点在协议项等)
 */
static uint8_t ui_focus_permille(uint16_t *permille)
{
  const SignalProtocol *proto = Menu_GetProtocol();
  const SignalParam    *param;
  uint8_t               focus = Menu_GetFocusItem();
  uint8_t               idx;
  uint32_t              value;
  uint32_t              span;

  if ((proto == NULL) || (permille == NULL))
  {
    return 0U;
  }
  if (focus == MENU_ITEM_PROTOCOL)
  {
    return 0U;
  }

  idx   = (uint8_t)(focus - 1U);
  param = SignalProtocol_ParamAt(proto, idx);
  if (param == NULL)
  {
    return 0U;
  }

  span = param->max - param->min;
  if (span == 0U)
  {
    return 0U;
  }

  /* 先按协议把值夹进合法区间, 避免脏数据算出 >100% 的填充宽度 */
  value = SignalProtocol_ClampParam(proto, idx, Menu_GetParamValue(idx));

  *permille = (uint16_t)((((uint32_t)(value - param->min)) * 1000UL) / span);
  if (*permille > 1000U)
  {
    *permille = 1000U;
  }

  return 1U;
}

/* ========================== 各行绘制 ========================== */

/** 第一行: 焦点标记 + 协议名 */
static void ui_draw_protocol_row(void)
{
  const SignalProtocol *proto = Menu_GetProtocol();
  char                  buf[24];
  uint8_t               pos = 0U;
  uint8_t               focus = Menu_GetFocusItem();

  /*
   * 焦点标记放在行首: 覆盖的是前缀的第一个字符位置, 因此正文仍从同一
   * 偏移开始, 焦点移动时协议名不会左右跳动.
   * 不聚焦时用空格占位, 保持两行文本对齐.
   */
  pos = ui_append_str(buf, pos, UI_TEXT_COLS,
                      (focus == MENU_ITEM_PROTOCOL) ? ">PROTO " : " PROTO ");
  (void)ui_append_str(buf, pos, UI_TEXT_COLS, (proto != NULL) ? proto->name : "NONE");

  OLED_PrintASCIIString(0U, UI_ROW1_Y, buf, UI_FONT, OLED_COLOR_NORMAL);
}

/** 第二行: 当前参数值 + 模式标记 + 百分比 */
static void ui_draw_param_row(void)
{
  const SignalProtocol *proto = Menu_GetProtocol();
  const SignalParam    *param = NULL;
  char                  buf[24];
  char                  num[16];
  uint8_t               pos = 0U;
  uint8_t               focus = Menu_GetFocusItem();
  uint8_t               idx   = 0U;
  uint16_t              permille = 0U;

  buf[0] = '\0';

  if ((proto != NULL) && (focus != MENU_ITEM_PROTOCOL))
  {
    idx   = (uint8_t)(focus - 1U);
    param = SignalProtocol_ParamAt(proto, idx);
  }

  if (param != NULL)
  {
    ui_format_param(param->kind, Menu_GetParamValue(idx), num, 12U);
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, num);
  }
  else
  {
    /* 焦点在协议项: 该行展示该协议的参数个数, 避免出现空白行 */
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, "params ");
    (void)ui_append_u32(buf, pos, UI_TEXT_COLS, Menu_GetParamCount());
    /* ui_append_u32 已把结尾 0 写在数字之后, 无需再调整 pos */
    pos = 0U;
    while ((buf[pos] != '\0') && (pos < UI_TEXT_COLS))
    {
      pos++;
    }
  }

  (void)ui_append_str(buf, pos, UI_TEXT_COLS,
                      (Menu_GetMode() == MENU_MODE_EDIT) ? " E" : " B");

  OLED_PrintASCIIString(0U, UI_ROW2_Y, buf, UI_FONT, OLED_COLOR_NORMAL);

  if (ui_focus_permille(&permille) != 0U)
  {
    ui_draw_percent(UI_ROW2_Y, permille);
  }
}

/** 第三行: 硬件实际生效的输出 */
static void ui_draw_out_row(void)
{
  const SignalOutputInfo *info = SignalOutput_GetInfo();
  char                    buf[24];
  uint8_t                 pos = 0U;
  uint16_t                permille = 0U;

  buf[0] = '\0';
  pos = ui_append_str(buf, pos, UI_TEXT_COLS, "OUT ");

  if (info->active_proto == NULL)
  {
    (void)ui_append_str(buf, pos, UI_TEXT_COLS, "IDLE");
  }
  else if (info->active_proto->wave == SIG_WAVE_DSHOT)
  {
    /* DShot 的"频率"就是帧率, 对用户意义不大; 显示速率与油门更直观 */
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, info->active_proto->detail);
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, " ");
    (void)ui_append_u32(buf, pos, UI_TEXT_COLS, info->param_values[0]);
  }
  else if (info->active_proto->wave == SIG_WAVE_ONESHOT)
  {
    pos = ui_append_u32(buf, pos, UI_TEXT_COLS, info->param_values[0]);
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, "us/");
    pos = ui_append_u32(buf, pos, UI_TEXT_COLS, info->period_us);
    (void)ui_append_str(buf, pos, UI_TEXT_COLS, "us");
  }
  else
  {
    pos = ui_append_u32(buf, pos, UI_TEXT_COLS, info->actual_hz);
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, "Hz ");
    pos = ui_append_u32(buf, pos, UI_TEXT_COLS, info->param_values[1] / 10U);
    pos = ui_append_str(buf, pos, UI_TEXT_COLS, ".");
    pos = ui_append_u32(buf, pos, UI_TEXT_COLS, info->param_values[1] % 10U);
    (void)ui_append_str(buf, pos, UI_TEXT_COLS, "%");
  }

  OLED_PrintASCIIString(0U, UI_ROW3_Y, buf, UI_FONT, OLED_COLOR_NORMAL);

  if (ui_focus_permille(&permille) != 0U)
  {
    ui_draw_percent(UI_ROW3_Y, permille);
  }
}

/** 第四行: 级别字母 + 最新日志 */
static void ui_draw_log_row(void)
{
  const SignalLogEntry *entry = SignalLog_Latest();
  char                  buf[24];
  char                  text[24];
  uint8_t               pos = 0U;

  buf[0] = '\0';

  if (entry == NULL)
  {
    /*
     * 还没有日志(理论上仅出现在极早期)时退回菜单状态文本, 保证该行
     * 始终有内容, 而不是一片空白让人怀疑屏幕坏了.
     */
    const char *status = Menu_GetStatusText();

    if ((status != NULL) && (status[0] != '\0'))
    {
      ui_text_sanitize(status, text, UI_TEXT_COLS);
      (void)ui_append_str(buf, 0U, UI_TEXT_COLS, text);
    }
    else
    {
      (void)ui_append_str(buf, 0U, UI_TEXT_COLS, "- (no log)");
    }
  }
  else
  {
    switch (entry->level)
    {
      case SIGLOG_ERROR:
        pos = ui_append_str(buf, pos, UI_TEXT_COLS, "E ");
        break;
      case SIGLOG_WARN:
        pos = ui_append_str(buf, pos, UI_TEXT_COLS, "W ");
        break;
      case SIGLOG_INFO:
      default:
        pos = ui_append_str(buf, pos, UI_TEXT_COLS, "I ");
        break;
    }

    /* 日志内容先过 ASCII 过滤, 中文/多字节会被替换为 '.' */
    ui_text_sanitize(entry->msg, text, (uint8_t)(UI_TEXT_COLS - pos));
    (void)ui_append_str(buf, pos, UI_TEXT_COLS, text);
  }

  OLED_PrintASCIIString(0U, UI_ROW4_Y, buf, UI_FONT, OLED_COLOR_NORMAL);
}

/** 条形图: 描边 + 按焦点参数比例填充 */
static void ui_draw_bar(void)
{
  uint16_t permille = 0U;
  uint16_t inner_w  = (uint16_t)UI_BAR_W - 2U;
  uint16_t inner_h  = (uint16_t)UI_BAR_H - 2U;
  uint16_t filled;

  OLED_DrawRectangle(UI_BAR_X, UI_BAR_Y, UI_BAR_W, UI_BAR_H, OLED_COLOR_NORMAL);

  if (ui_focus_permille(&permille) == 0U)
  {
    return; /* 焦点不在参数项: 只留边框 */
  }

  /*
   * 填充按内区(去掉 1px 边框)计算, 避免填充盖住描边.
   * UI_BAR_W 是 uint8 常量, 相乘前先提升到 uint32 防止溢出.
   */
  filled = (uint16_t)(((uint32_t)permille * inner_w) / 1000UL);

  if (filled > 0U)
  {
    /* 高度用内高而非内宽: 内宽(约 118)远大于条形图高度, 会画出区域之外 */
    OLED_DrawFilledRectangle((uint8_t)(UI_BAR_X + 1U), (uint8_t)(UI_BAR_Y + 1U),
                             (uint8_t)filled, (uint8_t)inner_h,
                             OLED_COLOR_NORMAL);
  }
}

/** 把一个档位样本加入波形环形缓冲 */
static void ui_wave_push(uint16_t value)
{
  if (value > KNOB_LEVEL_MAX)
  {
    value = KNOB_LEVEL_MAX;
  }

  s_wave[s_wave_head] = value;
  s_wave_head         = (uint8_t)((s_wave_head + 1U) % UI_WAVE_POINTS);
  if (s_wave_len < UI_WAVE_POINTS)
  {
    s_wave_len++;
  }
}

/** 档位 -> 波形区域内的 y 像素(档位越高越靠上) */
static uint8_t ui_wave_y(uint16_t value)
{
  uint32_t span = (uint32_t)UI_WAVE_H - 1U;

  return (uint8_t)((uint32_t)UI_WAVE_Y + span -
                   (((uint32_t)value * span) / KNOB_LEVEL_MAX));
}

/** 波形: 把环形缓冲里的历史点按时间顺序用直线连成折线 */
static void ui_draw_wave(void)
{
  uint8_t i;
  uint8_t prev_x    = 0U;
  uint8_t prev_y    = 0U;
  uint8_t have_prev = 0U;

  if (s_wave_len == 0U)
  {
    return;
  }

  /* 环形缓冲里最旧的一个点在 head - len 处 */
  for (i = 0U; i < s_wave_len; i++)
  {
    uint8_t base  = (uint8_t)((s_wave_head + UI_WAVE_POINTS - s_wave_len) % UI_WAVE_POINTS);
    uint8_t index = (uint8_t)((base + i) % UI_WAVE_POINTS);
    uint8_t x     = (uint8_t)(UI_WAVE_X + (uint8_t)(i * UI_WAVE_STEP));
    uint8_t y     = ui_wave_y(s_wave[index]);

    if (have_prev != 0U)
    {
      OLED_DrawLine(prev_x, prev_y, x, y, OLED_COLOR_NORMAL);
    }
    else
    {
      /* 折线的第一个点没有前驱, 直接画点 */
      OLED_SetPixel(x, y, OLED_COLOR_NORMAL);
    }

    prev_x    = x;
    prev_y    = y;
    have_prev = 1U;
  }
}

/* ========================== 对外接口 ========================== */

void UI_Init(void)
{
  uint8_t i;

  for (i = 0U; i < UI_WAVE_POINTS; i++)
  {
    s_wave[i] = 0U;
  }
  s_wave_head = 0U;
  s_wave_len  = 0U;

  /*
   * OLED 上电稳定比 MCU 慢, 先等 20ms 再发初始化命令, 否则命令可能在
   * 器件就绪前被丢弃(表现为黑屏). 用 HAL_Delay(SysTick 已由 HAL_Init 启动)
   * 而非空循环, 使等待时间与主频无关, 后续改主频也不会失效.
   */
  HAL_Delay(20U);

  OLED_Init();
}

void UI_Refresh(void)
{
  /*
   * 波形数据源取归一化档位(而不是原始 ADC): 它与条形图使用同一量纲,
   * 因此旋钮转动时波形与条形图的变化可以互相印证, 便于判断采样是否正常.
   */
  ui_wave_push(Knob_GetLevel());

  OLED_NewFrame();

  ui_draw_protocol_row();
  ui_draw_param_row();
  ui_draw_out_row();
  ui_draw_log_row();
  ui_draw_bar();
  ui_draw_wave();

  OLED_ShowFrame();
}
