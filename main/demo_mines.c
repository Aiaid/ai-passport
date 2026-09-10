// main/demo_mines.c —— 扫雷演示页:9×9 / 10 雷,三键操作。
//
// 本页只负责"输入 → 模型 → 重画",棋盘逻辑全在 mines_model.c,
// 像素绘制全在 puzzle_ui.c(81 格塞不下 81 个 lv_label,格内内容都画进 I4 画布)。
//
// 按键映射(OK 长按已被 main.c 拦截去返回菜单,这里不会收到):
//   进行中  UP/DOWN 短按 = 光标上/下一格      UP/DOWN 长按 = 上/下一行
//           OK 短按      = 翻开(数字格上等价于和弦)
//           OK 双击      = 插旗/拔旗
//   已分出胜负  OK 短按 = 开新局并重新计时;UP/DOWN 仍可移动光标;OK 双击忽略
//
// 线程上下文:enter/exit/key 全部由 main.c 的按键 drain 定时器在 LVGL 任务里调用
// (按键回调只往队列投递,不碰 LVGL),tick 本身也是 lv_timer —— 三者都已隐式持有
// LVGL 锁,所以本文件不自己加锁、不建任务/队列。
// 模型调用(布雷 + 连锁展开)在 9×9 上是微秒级,直接在按键回调里同步做。
#include "demo.h"
#include "bsp_button.h"
#include "mines_model.h"
#include "puzzle_ui.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "mines";

#define MINES_TICK_MS      250                       // 刷新周期:够跟上秒表,又不空转
#define MINES_BATT_TICKS   (5000 / MINES_TICK_MS)    // 电量每 5 秒读一次
#define MINES_TIME_MAX     999                       // 面板只有三位,超了就钳住

static mines_model_t s_model;

static lv_obj_t   *s_scr;
static lv_obj_t   *s_canvas;
static lv_obj_t   *s_info;                           // 面板里的两行文本
static lv_obj_t   *s_batt;
static lv_timer_t *s_timer;

static uint64_t s_start_ms;                          // 首次翻开(布雷)那一刻
static uint32_t s_elapsed_s;                         // 面板上正显示的秒数
static bool     s_timing;                            // 计时中?胜负已分即冻结
static uint32_t s_batt_ticks;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

// 当前应显示的秒数:计时中按真实时间算,已冻结则沿用冻结值。
static uint32_t elapsed_now(void)
{
    if (!s_timing) return s_elapsed_s;
    uint64_t secs = (now_ms() - s_start_ms) / 1000ULL;
    if (secs > MINES_TIME_MAX) secs = MINES_TIME_MAX;
    return (uint32_t)secs;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

static void render_board(void)
{
    if (!s_canvas) return;                           // 画布建失败:只留标题和面板

    // 1) 格底与旗/雷这些贴在格底上的图案。
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            uint8_t c = s_model.cell[y][x];

            // 输局时模型会把所有雷置 REVEALED,标对的雷同时保留 FLAG 位 ——
            // 那种格子要继续画成旗子(经典扫雷的表现),所以走"未翻开"分支。
            bool as_revealed = (c & MINES_CELL_REVEALED) && !(c & MINES_CELL_FLAG);

            if (as_revealed) {
                if (c & MINES_CELL_MINE) {
                    puzzle_ui_cell_fill(x, y, (c & MINES_CELL_BOOM) ? PUZZLE_C_BOOM
                                                                   : PUZZLE_C_PAPER);
                    puzzle_ui_cell_mine(x, y, PUZZLE_C_INK, PUZZLE_C_PAPER);
                } else {
                    puzzle_ui_cell_fill(x, y, PUZZLE_C_PAPER);
                }
            } else {
                puzzle_ui_cell_raised(x, y);
                if (c & MINES_CELL_FLAG) {
                    puzzle_ui_cell_flag(x, y, PUZZLE_C_RED, PUZZLE_C_INK);
                    // 输了才揭晓插错的旗,进行中不能剧透。
                    if (s_model.state == MINES_LOST && !(c & MINES_CELL_MINE)) {
                        puzzle_ui_cell_cross(x, y, PUZZLE_C_INK);
                    }
                } else if (s_model.state == MINES_LOST && (c & MINES_CELL_MINE)) {
                    puzzle_ui_cell_mine(x, y, PUZZLE_C_INK, PUZZLE_C_COVER_HI);
                }
            }
        }
    }

    // 2) 格线。扫雷没有 3×3 宫,粗线传 false。
    puzzle_ui_grid_lines(PUZZLE_C_INK, PUZZLE_C_INK, false);

    // 3) 光标框画在格线之后,否则会被格线盖掉。
    puzzle_ui_cell_frame(s_model.cursor_x, s_model.cursor_y, 2, PUZZLE_C_CURSOR);

    // 4) 数字最后画。scale 3 的字形在格里占 y 1..21,光标框压在头尾几行上,
    //    先画框后画字才不会啃掉数字的顶行和底行。
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            uint8_t c = s_model.cell[y][x];
            if (!(c & MINES_CELL_REVEALED) || (c & MINES_CELL_FLAG)) continue;
            if (c & MINES_CELL_MINE) continue;

            // 低 4 位取值域是 0..15,而 PUZZLE_ADJ_COLOR 只有 9 项:钳住下标,
            // 别让一个意外的高位值变成越界读。
            int adj = c & MINES_CELL_ADJ_MASK;
            if (adj > 8) adj = 8;
            if (adj > 0) puzzle_ui_cell_digit(x, y, adj, 3, PUZZLE_ADJ_COLOR[adj]);
        }
    }

    puzzle_ui_flush();
}

// 面板两行:第一行雷数/秒表,第二行随状态给操作提示或结果。
// 可用宽度约 206 px,montserrat_14 一行最多约 30 字符,别写更长。
static void update_panel(void)
{
    if (!s_info) return;

    if (!s_canvas) {                                  // 画布建失败:别再假装能玩
        lv_label_set_text(s_info, "CANVAS UNAVAILABLE\nhold OK to go back");
        return;
    }

    const char *hint;
    switch (s_model.state) {
        case MINES_WON:  hint = "YOU WIN!  OK = new game"; break;
        case MINES_LOST: hint = "BOOM!  OK = new game";    break;
        default:         hint = "OK dig   2xOK flag";      break;
    }

    // 用 elapsed_now() 而不是 s_elapsed_s:按键触发的重画可能发生在 tick 之前,
    // 拿缓存值会让秒表比实际慢一拍。
    lv_label_set_text_fmt(s_info, "MINES %02d   TIME %03u\n%s",
                          mines_model_remaining(&s_model),
                          (unsigned)elapsed_now(), hint);
}

// ---------------------------------------------------------------------------
// 定时器:只在秒数真的变了才重设标签文本,避免每 250 ms 白重排一次。
// ---------------------------------------------------------------------------

static void tick(lv_timer_t *t)
{
    (void)t;

    uint32_t secs = elapsed_now();
    if (secs != s_elapsed_s) {
        s_elapsed_s = secs;
        update_panel();
    }

    if (++s_batt_ticks >= MINES_BATT_TICKS) {
        s_batt_ticks = 0;
        puzzle_ui_battery_refresh(s_batt);
    }
}

// ---------------------------------------------------------------------------
// 页面生命周期
// ---------------------------------------------------------------------------

static void timer_reset(void)
{
    s_start_ms  = 0;
    s_elapsed_s = 0;
    s_timing    = false;
}

void demo_mines_enter(void)
{
    mines_model_init(&s_model, (uint32_t)esp_timer_get_time());
    timer_reset();
    s_batt_ticks = 0;

    s_scr    = ui_pixel_screen_create("MINES");
    s_canvas = puzzle_ui_canvas_create(s_scr);
    if (!s_canvas) ESP_LOGE(TAG, "画布不可用,本页降级为只显示提示文字");

    lv_obj_t *panel = puzzle_ui_panel_create(s_scr);
    s_info = lv_label_create(panel);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_line_space(s_info, -1, 0);   // 两行 14 号字挤进 46 px 面板
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 0, 0);

    s_batt = puzzle_ui_battery_create(s_scr);

    render_board();
    update_panel();

    s_timer = lv_timer_create(tick, MINES_TICK_MS, NULL);
    lv_screen_load(s_scr);

    ESP_LOGI(TAG, "进入扫雷:%dx%d,%d 雷", MINES_W, MINES_H, MINES_COUNT);
}

void demo_mines_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr)   { lv_obj_delete(s_scr);     s_scr = NULL; }
    s_canvas = NULL;
    s_info   = NULL;
    s_batt   = NULL;
    puzzle_ui_canvas_release();               // 画布对象随 screen 已删,这里只清指针
    timer_reset();
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------

// UP/DOWN 在任何状态下都能移动光标(模型允许),短按走一格、长按走一行。
static bool handle_move(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    int dir = (btn == BSP_BTN_UP) ? -1 : 1;
    if (ev == BSP_BTN_CLICK) { mines_model_move(&s_model, dir);            return true; }
    if (ev == BSP_BTN_LONG)  { mines_model_move(&s_model, dir * MINES_W);  return true; }
    return false;
}

static bool handle_reveal(void)
{
    bool was_placed = s_model.mines_placed;
    mines_event_t ev = mines_model_reveal(&s_model);

    // 首次翻开才布雷,秒表从这一刻起跑。
    if (!was_placed && s_model.mines_placed) {
        s_start_ms  = now_ms();
        s_elapsed_s = 0;
        s_timing    = true;
    }

    if (ev == MINES_EVENT_WON || ev == MINES_EVENT_LOST) {
        s_elapsed_s = elapsed_now();          // 冻住结束时刻的秒数
        s_timing    = false;
        ESP_LOGI(TAG, "%s,用时 %u 秒",
                 (ev == MINES_EVENT_WON) ? "扫雷胜利" : "踩雷失败",
                 (unsigned)s_elapsed_s);
    }
    return true;
}

void demo_mines_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr || !s_canvas) return;          // 画布不可用时本页不响应按键
    if (ev == BSP_BTN_PRESS) return;          // 只认 CLICK / DOUBLE / LONG

    bool dirty = false;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        dirty = handle_move(btn, ev);
    } else {                                  // BSP_BTN_OK,LONG 已被 main.c 拦截
        if (s_model.state != MINES_PLAYING) {
            if (ev == BSP_BTN_CLICK) {        // 胜负已分:一键开新局
                mines_model_reset(&s_model);
                timer_reset();
                dirty = true;
                ESP_LOGI(TAG, "开新局");
            }
        } else if (ev == BSP_BTN_CLICK) {
            dirty = handle_reveal();
        } else if (ev == BSP_BTN_DOUBLE) {
            mines_model_toggle_flag(&s_model);
            dirty = true;
        }
    }

    if (dirty) {                              // 9×9 全画一次很快,不做局部重绘
        render_board();
        update_panel();
    }
}
