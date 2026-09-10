// main/puzzle_ui.c —— 见 puzzle_ui.h。数独与扫雷共用的 I4 画布绘制层。
#include "puzzle_ui.h"

#include "bsp_battery.h"
#include "ui_pixel.h"

#include "esp_log.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "puzzle_ui";

// I4:每字节 2 像素,高 nibble 是左边那个像素(与 lv_draw_buf_goto_xy 的排布一致)。
// 216×216 → stride 108 B,像素区 23328 B,加调色板与对齐约 23.4 KB 静态 RAM。
LV_DRAW_BUF_DEFINE_STATIC(puzzle_canvas_buf, PUZZLE_CANVAS_SIZE, PUZZLE_CANVAS_SIZE,
                          LV_COLOR_FORMAT_I4);

static const uint32_t PUZZLE_PALETTE[PUZZLE_C_COUNT] = {
    [PUZZLE_C_PAPER]    = 0xF4F4EA,
    [PUZZLE_C_LINE]     = 0xB4BDB8,
    [PUZZLE_C_INK]      = 0x17202A,
    [PUZZLE_C_BLUE]     = 0x1B62C8,
    [PUZZLE_C_RED]      = 0xE43B2F,
    [PUZZLE_C_YELLOW]   = 0xFFD928,
    [PUZZLE_C_GREEN]    = 0x1E7A34,
    [PUZZLE_C_COVER]    = 0x93A7B1,
    [PUZZLE_C_COVER_HI] = 0xC7D5DC,
    [PUZZLE_C_COVER_LO] = 0x5C7480,
    [PUZZLE_C_PURPLE]   = 0x7B2FA8,
    [PUZZLE_C_MAROON]   = 0x8E1B1B,
    [PUZZLE_C_TEAL]     = 0x00838F,
    [PUZZLE_C_ORANGE]   = 0xE07800,
    [PUZZLE_C_BOOM]     = 0xFF6A55,
    [PUZZLE_C_CURSOR]   = 0xFF7A00,
};

const uint8_t PUZZLE_ADJ_COLOR[9] = {
    PUZZLE_C_PAPER,                         // 0:不画
    PUZZLE_C_BLUE, PUZZLE_C_GREEN, PUZZLE_C_RED, PUZZLE_C_PURPLE,
    PUZZLE_C_MAROON, PUZZLE_C_TEAL, PUZZLE_C_INK, PUZZLE_C_COVER_LO,
};

#define GLYPH_W 5
#define GLYPH_H 7

// 5×7 点阵数字。'#' = 落笔。两个游戏都用它显示 1..9。
static const char *const GLYPH[10][GLYPH_H] = {
    { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." },  // 0
    { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." },  // 1
    { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" },  // 2
    { "####.", "....#", "....#", ".###.", "....#", "....#", "####." },  // 3
    { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." },  // 4
    { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." },  // 5
    { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." },  // 6
    { "#####", "....#", "...#.", "..#..", "..#..", ".#...", ".#..." },  // 7
    { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." },  // 8
    { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." },  // 9
};

#define SPRITE_W 9
#define SPRITE_H 9

// '1' = 雷体,'2' = 高光,'.' = 透明。
static const char *const SPRITE_MINE[SPRITE_H] = {
    "....1....",
    "1...1...1",
    ".1.111.1.",
    "..12111..",
    "111111111",
    "..11111..",
    ".1.111.1.",
    "1...1...1",
    "....1....",
};

// '1' = 旗面,'2' = 旗杆与底座,'.' = 透明。
static const char *const SPRITE_FLAG[SPRITE_H] = {
    ".....2...",
    "..1112...",
    ".11112...",
    "..1112...",
    ".....2...",
    ".....2...",
    "....222..",
    "...22222.",
    "..2222222",
};

static lv_obj_t *s_canvas;

// ---------------------------------------------------------------------------
// 底层像素写入
// ---------------------------------------------------------------------------

// 在一行内填 [x, x+w) 的像素。x 已相对行首,越界由调用方裁剪后再进来。
static void row_span(uint8_t *row, int x, int w, uint8_t color)
{
    int x_end = x + w;
    if (x < 0) x = 0;
    if (x_end > PUZZLE_CANVAS_SIZE) x_end = PUZZLE_CANVAS_SIZE;
    if (x >= x_end) return;

    uint8_t nibble = (uint8_t)(color & 0x0F);
    if (x & 1) {                                     // 起始半字节落在低 nibble
        row[x >> 1] = (uint8_t)((row[x >> 1] & 0xF0) | nibble);
        x++;
    }
    int full_bytes = (x_end - x) >> 1;
    if (full_bytes > 0) {
        memset(row + (x >> 1), (uint8_t)((nibble << 4) | nibble), (size_t)full_bytes);
        x += full_bytes * 2;
    }
    if (x < x_end) {                                 // 结尾单像素落在高 nibble
        row[x >> 1] = (uint8_t)((row[x >> 1] & 0x0F) | (uint8_t)(nibble << 4));
    }
}

void puzzle_ui_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (!s_canvas || w <= 0 || h <= 0) return;
    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(s_canvas);
    if (!draw_buf) return;

    int y_end = y + h;
    if (y < 0) y = 0;
    if (y_end > PUZZLE_CANVAS_SIZE) y_end = PUZZLE_CANVAS_SIZE;
    for (int row_y = y; row_y < y_end; row_y++) {
        uint8_t *row = lv_draw_buf_goto_xy(draw_buf, 0, (uint32_t)row_y);
        if (!row) continue;
        row_span(row, x, w, color);
    }
}

void puzzle_ui_px(int x, int y, uint8_t color)
{
    puzzle_ui_fill_rect(x, y, 1, 1, color);
}

void puzzle_ui_fill(uint8_t color)
{
    puzzle_ui_fill_rect(0, 0, PUZZLE_CANVAS_SIZE, PUZZLE_CANVAS_SIZE, color);
}

void puzzle_ui_frame(int x, int y, int w, int h, int thickness, uint8_t color)
{
    if (thickness <= 0 || w <= 0 || h <= 0) return;
    if (thickness * 2 > w) thickness = w / 2 > 0 ? w / 2 : 1;
    if (thickness * 2 > h) thickness = h / 2 > 0 ? h / 2 : 1;
    puzzle_ui_fill_rect(x, y, w, thickness, color);
    puzzle_ui_fill_rect(x, y + h - thickness, w, thickness, color);
    puzzle_ui_fill_rect(x, y + thickness, thickness, h - 2 * thickness, color);
    puzzle_ui_fill_rect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
}

// ---------------------------------------------------------------------------
// 格级绘制
// ---------------------------------------------------------------------------

static bool cell_valid(int cx, int cy)
{
    return cx >= 0 && cx < PUZZLE_GRID && cy >= 0 && cy < PUZZLE_GRID;
}

void puzzle_ui_cell_fill(int cx, int cy, uint8_t color)
{
    if (!cell_valid(cx, cy)) return;
    puzzle_ui_fill_rect(cx * PUZZLE_CELL, cy * PUZZLE_CELL, PUZZLE_CELL, PUZZLE_CELL, color);
}

void puzzle_ui_cell_frame(int cx, int cy, int thickness, uint8_t color)
{
    if (!cell_valid(cx, cy)) return;
    puzzle_ui_frame(cx * PUZZLE_CELL, cy * PUZZLE_CELL, PUZZLE_CELL, PUZZLE_CELL,
                    thickness, color);
}

void puzzle_ui_cell_raised(int cx, int cy)
{
    if (!cell_valid(cx, cy)) return;
    int x = cx * PUZZLE_CELL;
    int y = cy * PUZZLE_CELL;
    puzzle_ui_fill_rect(x, y, PUZZLE_CELL, PUZZLE_CELL, PUZZLE_C_COVER);
    puzzle_ui_fill_rect(x + 1, y + 1, PUZZLE_CELL - 2, 3, PUZZLE_C_COVER_HI);
    puzzle_ui_fill_rect(x + 1, y + 1, 3, PUZZLE_CELL - 2, PUZZLE_C_COVER_HI);
    puzzle_ui_fill_rect(x + 1, y + PUZZLE_CELL - 4, PUZZLE_CELL - 2, 3, PUZZLE_C_COVER_LO);
    puzzle_ui_fill_rect(x + PUZZLE_CELL - 4, y + 1, 3, PUZZLE_CELL - 2, PUZZLE_C_COVER_LO);
}

// 把 '.'/'0' 以外的笔画按 scale 倍放大画到 (x, y)。code_color[c] 给出每种笔画的颜色。
static void blit_sprite(int x, int y, const char *const *rows, int w, int h, int scale,
                        const uint8_t *code_color, int code_count)
{
    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            char pixel = rows[sy][sx];
            if (pixel == '.' || pixel == '0' || pixel == '\0') continue;
            int code = (pixel == '#') ? 1 : (pixel - '0');
            if (code < 0 || code >= code_count) continue;
            puzzle_ui_fill_rect(x + sx * scale, y + sy * scale, scale, scale,
                                code_color[code]);
        }
    }
}

void puzzle_ui_cell_digit(int cx, int cy, int digit, int scale, uint8_t color)
{
    if (!cell_valid(cx, cy) || digit < 0 || digit > 9 || scale <= 0) return;
    const uint8_t code_color[2] = { 0, color };
    int x = cx * PUZZLE_CELL + (PUZZLE_CELL - GLYPH_W * scale) / 2;
    int y = cy * PUZZLE_CELL + (PUZZLE_CELL - GLYPH_H * scale) / 2;
    blit_sprite(x, y, GLYPH[digit], GLYPH_W, GLYPH_H, scale, code_color, 2);
}

void puzzle_ui_cell_mine(int cx, int cy, uint8_t body, uint8_t shine)
{
    if (!cell_valid(cx, cy)) return;
    const uint8_t code_color[3] = { 0, body, shine };
    int x = cx * PUZZLE_CELL + (PUZZLE_CELL - SPRITE_W * 2) / 2;
    int y = cy * PUZZLE_CELL + (PUZZLE_CELL - SPRITE_H * 2) / 2;
    blit_sprite(x, y, SPRITE_MINE, SPRITE_W, SPRITE_H, 2, code_color, 3);
}

void puzzle_ui_cell_flag(int cx, int cy, uint8_t cloth, uint8_t pole)
{
    if (!cell_valid(cx, cy)) return;
    const uint8_t code_color[3] = { 0, cloth, pole };
    int x = cx * PUZZLE_CELL + (PUZZLE_CELL - SPRITE_W * 2) / 2;
    int y = cy * PUZZLE_CELL + (PUZZLE_CELL - SPRITE_H * 2) / 2;
    blit_sprite(x, y, SPRITE_FLAG, SPRITE_W, SPRITE_H, 2, code_color, 3);
}

void puzzle_ui_cell_cross(int cx, int cy, uint8_t color)
{
    if (!cell_valid(cx, cy)) return;
    int x = cx * PUZZLE_CELL;
    int y = cy * PUZZLE_CELL;
    for (int i = 4; i < PUZZLE_CELL - 4; i++) {
        puzzle_ui_fill_rect(x + i, y + i, 2, 2, color);
        puzzle_ui_fill_rect(x + PUZZLE_CELL - 1 - i, y + i, 2, 2, color);
    }
}

void puzzle_ui_grid_lines(uint8_t thin, uint8_t bold, bool blocks)
{
    for (int k = 1; k < PUZZLE_GRID; k++) {
        int pos = k * PUZZLE_CELL;
        puzzle_ui_fill_rect(pos, 0, 1, PUZZLE_CANVAS_SIZE, thin);
        puzzle_ui_fill_rect(0, pos, PUZZLE_CANVAS_SIZE, 1, thin);
    }
    if (blocks) {
        for (int k = 3; k < PUZZLE_GRID; k += 3) {
            int pos = k * PUZZLE_CELL - 1;
            puzzle_ui_fill_rect(pos, 0, 2, PUZZLE_CANVAS_SIZE, bold);
            puzzle_ui_fill_rect(0, pos, PUZZLE_CANVAS_SIZE, 2, bold);
        }
    }
    puzzle_ui_frame(0, 0, PUZZLE_CANVAS_SIZE, PUZZLE_CANVAS_SIZE, 2, bold);
}

void puzzle_ui_flush(void)
{
    if (s_canvas) lv_obj_invalidate(s_canvas);
}

// ---------------------------------------------------------------------------
// 画布与页面骨架
// ---------------------------------------------------------------------------

lv_obj_t *puzzle_ui_canvas_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    LV_DRAW_BUF_INIT_STATIC(puzzle_canvas_buf);

    // LVGL 把调色板放在 draw buf 的头部并按 lv_color32_t 写入,缓冲首地址必须
    // 4 字节对齐。LV_ATTRIBUTE_MEM_ALIGN 默认为空,靠编译器给大数组的自然对齐;
    // 当前工具链给的是 4 对齐,但换工具链后一旦退化,RV32IMC 上非对齐 32 位写
    // 是 LoadStoreAlignment 异常直接 panic,不是"慢一点"。所以这里不建画布、
    // 返回 NULL 让页面降级,而不是打一条告警继续跑。
    if (((uintptr_t)puzzle_canvas_buf.data & 0x03U) != 0) {
        ESP_LOGE(TAG, "画布缓冲未 4 字节对齐 (%p),放弃建画布",
                 (void *)puzzle_canvas_buf.data);
        return NULL;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    if (!canvas) return NULL;
    lv_canvas_set_draw_buf(canvas, &puzzle_canvas_buf);
    for (uint8_t i = 0; i < PUZZLE_C_COUNT; i++) {
        lv_canvas_set_palette(canvas, i,
                              lv_color_to_32(lv_color_hex(PUZZLE_PALETTE[i]), LV_OPA_COVER));
    }
    lv_obj_set_pos(canvas, PUZZLE_CANVAS_X, PUZZLE_CANVAS_Y);

    s_canvas = canvas;
    puzzle_ui_fill(PUZZLE_C_PAPER);
    return canvas;
}

void puzzle_ui_canvas_release(void)
{
    s_canvas = NULL;
}

lv_obj_t *puzzle_ui_panel_create(lv_obj_t *parent)
{
    lv_obj_t *panel = ui_pixel_panel_create(parent, PUZZLE_PANEL_X, PUZZLE_PANEL_Y,
                                            PUZZLE_PANEL_W, PUZZLE_PANEL_H, UI_PAPER);
    // 默认 pad 7 只放得下一行 14 号字,这里收窄到 3 以容纳两行。
    // 余量只有 1 px:内容区高 46 - 2*4(border) - 2*3(pad) = 32,两行 montserrat_14
    // 按 line_space -1 是 16*2 - 1 = 31。改字号、改 line_space、改 PUZZLE_PANEL_H
    // 或加第三行都会立刻裁字,动之前先重算这三个数。
    lv_obj_set_style_pad_all(panel, 3, 0);
    return panel;
}

lv_obj_t *puzzle_ui_battery_create(lv_obj_t *parent)
{
    lv_obj_t *label = ui_pixel_label(parent, "", &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_pos(label, PUZZLE_BATT_X, PUZZLE_BATT_Y);
    lv_obj_set_width(label, PUZZLE_BATT_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    puzzle_ui_battery_refresh(label);
    return label;
}

void puzzle_ui_battery_refresh(lv_obj_t *label)
{
    if (!label) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {                                   // 电量计不可用:不画假数字
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(label, "%d%%", soc);
    lv_obj_set_style_text_color(label, lv_color_hex(soc < 20 ? UI_RED : 0xFFFFFF), 0);
}
