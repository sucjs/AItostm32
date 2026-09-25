/**
 * @file ui.h
 * @brief OLED 界面渲染(SSD1306 128x64, 全部 ASCII)
 * @note  硬约束: OLED 字库只有 ASCII 字形(英文 0x20..0x7B),
 *        任何非 ASCII 字节都会落到字库之外并显示为乱码.
 *        因此本模块在渲染前对文本做一次 ASCII 过滤(见 ui.c 的 ui_text_sanitize),
 *        即使上游(日志/状态串)混入中文或多字节内容也不会污染屏幕.
 */
#ifndef __UI_H
#define __UI_H

#include <stdint.h>

/**
 * @brief 初始化显示
 * @note  OLED 上电比 MCU 慢, 内部先等待约 20ms 再 OLED_Init,
 *        否则初始化命令可能在器件就绪前发出而被丢弃(表现为黑屏).
 */
void UI_Init(void);

/**
 * @brief 渲染一帧
 * @note  调用频率由 app.c 用 UI_REFRESH_MS 限制. 本函数内部完成
 *        NewFrame -> 绘制 -> ShowFrame, 其中的 I2C 传输会阻塞若干毫秒,
 *        所以不能放在中断里, 也不能每 1ms 调用一次.
 */
void UI_Refresh(void);

#endif /* __UI_H */
