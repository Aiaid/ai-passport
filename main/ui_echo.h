// main/ui_echo.h —— ECHO 全彩 HUD 的配色与通用控件,三屏(菜单/ECHO/RADAR)复用。
//
// 设计源见 design/Main.dc.html、Discovery.dc.html、Logo.dc.html。设备端只有
// montserrat 字体(无 Silkscreen/Share Tech Mono 像素/等宽字),故字体与切角/
// 扫描线等 CSS 效果按 LVGL 能力近似;配色严格对齐设计稿。
#pragma once

#include "lvgl.h"

// —— 安全区(屏幕可视区为圆角矩形:外圈留白、四角不放内容)——
#define ECHO_SAFE    10   // 左右安全内边距
#define ECHO_HDR_Y   8    // header 顶部留白
#define ECHO_HDR_H   28   // header 高度(底边 = 8 + 28 = 36)
#define ECHO_BODY_Y  40   // 内容起始 y(在 header 之下)
#define ECHO_BODY_W  220  // 内容宽度(240 - 2*ECHO_SAFE)

// —— HUD 基础色 ——
#define ECHO_BG       0x0B1224  // 背景深蓝(渐变顶)
#define ECHO_BG2      0x0A0F1C  // 背景深蓝(渐变底)
#define ECHO_PANEL    0x121A2E  // 面板
#define ECHO_STROKE   0x243352  // 描边
#define ECHO_TEXT     0xE8EEF5  // 主文本
#define ECHO_MUTED    0x8FA6C8  // 次要文本
#define ECHO_AMBER    0xFFB627  // 品牌琥珀
#define ECHO_AMBER2   0xFF9A1F  // 品牌琥珀(深)
#define ECHO_HDRTEXT  0x0B1020  // 琥珀 header 上的深色文字
#define ECHO_FOOT     0x070B14  // 底栏
#define ECHO_ARCTRK   0x1B2540  // 弧/条的未填充轨道

// —— 运动分色阶(阈值)——
#define ECHO_GREEN    0x51D07F
#define ECHO_YELLOW   0xFFD928
#define ECHO_ORANGE   0xFF7A18
#define ECHO_RED      0xE8431F

// —— 设备类型色 ——
#define ECHO_T_PHONE  0x51D07F
#define ECHO_T_PC     0x4AA3FF
#define ECHO_T_IOT    0xFFB627
#define ECHO_T_AP     0xA678FF

// 深蓝底 + 顶部琥珀 header(左侧 title)的屏幕。返回 screen。
lv_obj_t *ui_echo_screen(const char *title);

// 在 header 右侧加一个可动态更新的小标签(深色字),返回该标签;parent 传屏幕。
lv_obj_t *ui_echo_header_right(lv_obj_t *scr, const char *text);

// #121A2E 面板 + #243352 描边 + 小圆角,绝对定位。
lv_obj_t *ui_echo_panel(lv_obj_t *parent, int x, int y, int w, int h);

// 便捷 label。
lv_obj_t *ui_echo_label(lv_obj_t *parent, const char *text,
                        const lv_font_t *font, uint32_t color);

// 运动分(0..100)→ 阈值色:<40 绿 / <70 琥珀 / 否则红。
uint32_t ui_echo_motion_color(int score);

// 归一化幅度/强度(0..100)→ 热力色:蓝→青→绿→黄→橙。
uint32_t ui_echo_heat_color(int level);
