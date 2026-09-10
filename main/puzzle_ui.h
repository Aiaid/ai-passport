// main/puzzle_ui.h —— 数独 / 扫雷共用的像素绘制层。
//
// 两个游戏不会同时活动,因此共用一块静态 I4 画布缓冲(216×216,9×9 格、每格 24 px)。
// LVGL 的 malloc 池只有 24 KB(sdkconfig.defaults: CONFIG_LV_MEM_SIZE_KILOBYTES=24),
// 放不下 81 个 lv_label,所以格内的数字、雷、旗、光标全部按像素画进 canvas。
//
// 线程上下文:所有函数都直接操作 LVGL 对象,只能在 LVGL 任务里
// (lv_timer 回调)或已持有 bsp_lvgl_lock() 的上下文中调用。
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define PUZZLE_GRID         9                                  // 9×9 格
#define PUZZLE_CELL         24                                 // 每格边长(px)
#define PUZZLE_CANVAS_SIZE  (PUZZLE_GRID * PUZZLE_CELL)        // 216

// 屏幕布局(240×320 竖屏):标题牌 y 8..41,草地 y 286 起,右上角白云 x188 y8..25。
#define PUZZLE_CANVAS_X     12
#define PUZZLE_CANVAS_Y     48                                 // 画布 y 48..263
#define PUZZLE_PANEL_X      10
#define PUZZLE_PANEL_Y      266                                // 信息面板压在草地上
#define PUZZLE_PANEL_W      220
#define PUZZLE_PANEL_H      46
#define PUZZLE_BATT_X       160                                // 电量:白云下方的空闲蓝天
#define PUZZLE_BATT_Y       27
#define PUZZLE_BATT_W       74

// I4 调色板索引(≤16 色,两个游戏共用一套)。
enum {
    PUZZLE_C_PAPER = 0,    // 空格/已翻开格底色
    PUZZLE_C_LINE,         // 细格线
    PUZZLE_C_INK,          // 粗格线、题面给定数、雷体、扫雷 "7"
    PUZZLE_C_BLUE,         // 玩家填入的数、扫雷 "1"
    PUZZLE_C_RED,          // 冲突数、旗面、扫雷 "3"
    PUZZLE_C_YELLOW,       // 光标格底色
    PUZZLE_C_GREEN,        // EDIT 模式光标框、扫雷 "2"
    PUZZLE_C_COVER,        // 未翻开格face
    PUZZLE_C_COVER_HI,     // 未翻开格高光(上/左)
    PUZZLE_C_COVER_LO,     // 未翻开格阴影(下/右)、扫雷 "8"
    PUZZLE_C_PURPLE,       // 扫雷 "4"
    PUZZLE_C_MAROON,       // 扫雷 "5"
    PUZZLE_C_TEAL,         // 扫雷 "6"
    PUZZLE_C_ORANGE,       // 提示/强调
    PUZZLE_C_BOOM,         // 踩中的雷格底色
    PUZZLE_C_CURSOR,       // 光标边框
    PUZZLE_C_COUNT,
};

// lv_draw_buf_set_palette 不做下标检查,写越界会直接踩到像素数据。
_Static_assert(PUZZLE_C_COUNT <= 16, "I4 调色板最多 16 色");

// 扫雷数字 1..8 的经典配色(索引 0 未使用)。
extern const uint8_t PUZZLE_ADJ_COLOR[9];

// 建画布:绑定共享静态缓冲、下发调色板、放到 (PUZZLE_CANVAS_X, PUZZLE_CANVAS_Y)。
// 同一时刻只允许存在一个画布。失败返回 NULL,调用方必须降级(别再调其它绘制函数)。
lv_obj_t *puzzle_ui_canvas_create(lv_obj_t *parent);
// 页面删屏后调用:只清内部指针,不删对象(对象随 screen 一起被删)。
void puzzle_ui_canvas_release(void);

// ---- 像素级绘制。坐标以画布左上角为原点,越界自动裁剪。----
void puzzle_ui_fill(uint8_t color);                                     // 整块画布
void puzzle_ui_fill_rect(int x, int y, int w, int h, uint8_t color);
void puzzle_ui_px(int x, int y, uint8_t color);
void puzzle_ui_frame(int x, int y, int w, int h, int thickness, uint8_t color);

// ---- 格级绘制。cx / cy 为 0..8 的格坐标。----
void puzzle_ui_cell_fill(int cx, int cy, uint8_t color);
void puzzle_ui_cell_frame(int cx, int cy, int thickness, uint8_t color);
// 未翻开格的凸起效果(底色 + 上左高光 + 下右阴影)。
void puzzle_ui_cell_raised(int cx, int cy);
// 内嵌 5×7 点阵数字,scale 倍放大后居中在格里。digit 取 0..9,其余忽略。
void puzzle_ui_cell_digit(int cx, int cy, int digit, int scale, uint8_t color);
void puzzle_ui_cell_mine(int cx, int cy, uint8_t body, uint8_t shine);
void puzzle_ui_cell_flag(int cx, int cy, uint8_t cloth, uint8_t pole);
void puzzle_ui_cell_cross(int cx, int cy, uint8_t color);               // 错误插旗的叉

// 细格线 + 2 px 外框;blocks 为 true 时另画数独的 3×3 宫粗线。
void puzzle_ui_grid_lines(uint8_t thin, uint8_t bold, bool blocks);

// 把画布内容推到屏幕(内部就是 lv_obj_invalidate)。
void puzzle_ui_flush(void);

// ---- 页面骨架 ----
// 画布下方的信息面板(墨色描边,压在草地上),内部 pad 已调小以容纳两行 14 号字。
lv_obj_t *puzzle_ui_panel_create(lv_obj_t *parent);
// 右上角电量标签(避开白云)。返回的 label 由调用方随 screen 一起销毁。
lv_obj_t *puzzle_ui_battery_create(lv_obj_t *parent);
// 刷新电量;bsp_battery_soc() 返回 -1 时隐藏标签而不是画一个假数字。
void puzzle_ui_battery_refresh(lv_obj_t *label);
