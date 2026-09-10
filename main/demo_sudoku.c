// main/demo_sudoku.c —— 数独演示页:纯像素画布 + 三键交互。
//
// 页面职责:把 sudoku_model 的状态渲染到 puzzle_ui 的共享 I4 画布上,
// 并把三个按键翻译成模型的 move / cycle / ok / clear 调用。棋盘里的数字
// 全部按点阵画进 canvas(LVGL 的 24 KB malloc 池放不下 81 个 lv_label)。
//
// 按键映射(PLAYING):
//   UP   短按   NAVIGATE: 光标前移一格 / EDIT: 候选值 -1
//   DOWN 短按   NAVIGATE: 光标后移一格 / EDIT: 候选值 +1
//   UP   长按   光标上移一行(-9);EDIT 下先放弃候选值再跳行
//   DOWN 长按   光标下移一行(+9);EDIT 下先放弃候选值再跳行
//   确定 短按   NAVIGATE: 进入 EDIT / EDIT: 提交并回 NAVIGATE
//   确定 双击   清空当前格
//   确定 长按   返回菜单(被 main.c 统一拦截,本页收不到)
// 解出后(SOLVED):确定短按 = 切到下一档难度并重新出题,其余按键忽略。
//
// 线程上下文:enter / exit / key 全部由 main.c 的按键 drain 定时器在 LVGL 任务里
// 调用(按键回调只往队列投递,不碰 LVGL),tick 本身也是 lv_timer,因此三者都已
// 隐式持有 LVGL 锁,可以直接操作 lv_* 对象,页面自己不再加锁。
// 出题(sudoku_model_new_game)可能耗时几百毫秒,绝不能在按键回调里同步做:
// 按键只置 s_new_game_pending,真正的出题延后到 tick 里,让 "GENERATING..."
// 有机会先刷上屏。
#include "demo.h"
#include "bsp_button.h"
#include "puzzle_ui.h"
#include "sudoku_model.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sudoku";

static const char *const DIFF_NAME[SUDOKU_DIFFICULTY_COUNT] = {
    "EASY", "MEDIUM", "HARD",
};

#define BATT_TICKS 50                       // tick 周期 100ms → 约 5 秒刷一次电量

static lv_obj_t   *s_scr;
static lv_obj_t   *s_canvas;
static lv_obj_t   *s_panel;
static lv_obj_t   *s_info;                  // 面板里的两行文字
static lv_obj_t   *s_batt;
static lv_timer_t *s_timer;

static sudoku_model_t      s_model;
static sudoku_difficulty_t s_difficulty = SUDOKU_EASY;
static bool s_new_game_pending;
static int  s_pending_delay;                // 出题前空转的 tick 数,先让面板刷出来
static bool s_generate_failed;
static int  s_batt_ticks;

// ---------------------------------------------------------------- 面板文字

// 面板可用宽度约 206 px,montserrat_14 下一行最多约 30 个字符。
static void render_panel(void)
{
    if (!s_info) return;

    if (!s_canvas) {                        // 画布建失败:别再假装能玩
        lv_label_set_text(s_info, "CANVAS UNAVAILABLE\nhold OK to go back");
        return;
    }
    if (s_new_game_pending) {
        lv_label_set_text(s_info, "GENERATING...\n");
        return;
    }
    if (s_generate_failed) {
        lv_label_set_text(s_info, "GENERATE FAILED\nOK retry");
        return;
    }

    const char *hint;
    if (s_model.state == SUDOKU_SOLVED) {
        hint = "SOLVED!  OK = new game";
    } else if (s_model.mode == SUDOKU_MODE_EDIT) {
        hint = "UP/DN pick  OK set";
    } else {
        hint = "UP/DN move  OK edit";
    }

    lv_label_set_text_fmt(s_info, "%-6s EMPTY %d\n%s",
                          DIFF_NAME[s_model.difficulty],
                          (int)s_model.empty_count, hint);
}

// ---------------------------------------------------------------- 棋盘绘制

static void render_board(void)
{
    if (!s_canvas) return;

    const int cx = s_model.cursor_x;
    const int cy = s_model.cursor_y;
    const bool editing = (s_model.mode == SUDOKU_MODE_EDIT);

    // 1) 底色:光标格高亮,其余纸色。
    for (int y = 0; y < SUDOKU_N; y++) {
        for (int x = 0; x < SUDOKU_N; x++) {
            puzzle_ui_cell_fill(x, y,
                sudoku_model_cursor_at(&s_model, x, y) ? PUZZLE_C_YELLOW
                                                       : PUZZLE_C_PAPER);
        }
    }

    // 2) 细格线 + 3×3 宫粗线。
    puzzle_ui_grid_lines(PUZZLE_C_LINE, PUZZLE_C_INK, true);

    // 3) 光标框画在格线之后,否则会被格线盖掉。
    if (editing) puzzle_ui_cell_frame(cx, cy, 3, PUZZLE_C_GREEN);
    else         puzzle_ui_cell_frame(cx, cy, 2, PUZZLE_C_CURSOR);

    // 4) 数字最后画。scale 3 的字形在格里占 y 1..21,EDIT 的 3 px 光标框正好压在
    //    第 0..2 行和第 21..23 行上,先画框后画字才不会把数字的头尾啃掉。
    //    注意模型的坐标接口是 (m, x, y),而盘面数组下标是 cell[y][x]。
    for (int y = 0; y < SUDOKU_N; y++) {
        for (int x = 0; x < SUDOKU_N; x++) {
            int     value = s_model.cell[y][x];
            uint8_t color;

            if (editing && x == cx && y == cy) {
                value = s_model.edit_value;         // EDIT 中显示正在挑的候选值
                color = PUZZLE_C_BLUE;
            } else if (sudoku_model_is_given(&s_model, x, y)) {
                color = PUZZLE_C_INK;
            } else if (sudoku_model_conflicts(&s_model, x, y)) {
                color = PUZZLE_C_RED;
            } else {
                color = PUZZLE_C_BLUE;
            }

            if (value == 0) continue;               // 空格不画
            puzzle_ui_cell_digit(x, y, value, 3, color);
        }
    }

    puzzle_ui_flush();
}

// ---------------------------------------------------------------- 出题

static void request_new_game(void)
{
    s_new_game_pending = true;
    s_pending_delay    = 1;                 // 空转一帧,先把 "GENERATING..." 刷上屏
    s_generate_failed  = false;
    render_panel();
}

static void run_new_game(void)
{
    int64_t t0 = esp_timer_get_time();
    bool ok = sudoku_model_new_game(&s_model, s_difficulty);
    int64_t took_us = esp_timer_get_time() - t0;

    // 真机耗时从串口读:出题跑在 LVGL 任务里,这条数字决定要不要再拆帧。
    ESP_LOGI(TAG, "new_game difficulty=%d took %lld us",
             (int)s_difficulty, (long long)took_us);

    if (!ok) {
        s_generate_failed = true;
        ESP_LOGW(TAG, "出题失败,难度 %s", DIFF_NAME[s_difficulty]);
        render_panel();
        return;
    }

    ESP_LOGI(TAG, "出题完成:难度 %s,空格 %d",
             DIFF_NAME[s_difficulty], (int)s_model.empty_count);
    render_board();
    render_panel();
}

// ---------------------------------------------------------------- 定时器

// 跑在 LVGL 任务里,已持有锁,可直接操作对象。
static void tick(lv_timer_t *t)
{
    (void)t;

    if (s_new_game_pending) {
        if (s_pending_delay > 0) { s_pending_delay--; return; }
        s_new_game_pending = false;
        run_new_game();
    }

    if (++s_batt_ticks >= BATT_TICKS) {
        s_batt_ticks = 0;
        puzzle_ui_battery_refresh(s_batt);
    }
}

// ---------------------------------------------------------------- 页面接口

void demo_sudoku_enter(void)
{
    ESP_LOGI(TAG, "进入数独页,难度 %s", DIFF_NAME[s_difficulty]);

    sudoku_model_init(&s_model, (uint32_t)esp_timer_get_time());
    s_generate_failed = false;
    s_batt_ticks      = 0;

    s_scr    = ui_pixel_screen_create("SUDOKU");
    s_canvas = puzzle_ui_canvas_create(s_scr);
    s_panel  = puzzle_ui_panel_create(s_scr);
    if (!s_canvas) ESP_LOGE(TAG, "画布不可用,本页降级为只显示提示文字");

    s_info = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_line_space(s_info, -1, 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 0, 0);

    s_batt = puzzle_ui_battery_create(s_scr);

    if (s_canvas) {
        // 出题前先把空棋盘的格线画出来,别让 "GENERATING..." 期间是一块白板。
        puzzle_ui_grid_lines(PUZZLE_C_LINE, PUZZLE_C_INK, true);
        puzzle_ui_flush();
        // 出题交给 tick 做,这样 "GENERATING..." 能先渲染出来。
        request_new_game();
    } else {
        render_panel();
    }

    s_timer = lv_timer_create(tick, 100, NULL);
    lv_screen_load(s_scr);
}

void demo_sudoku_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr)   { lv_obj_delete(s_scr); s_scr = NULL; }
    s_canvas = NULL;
    s_panel  = NULL;
    s_info   = NULL;
    s_batt   = NULL;
    s_new_game_pending = false;
    s_pending_delay    = 0;
    puzzle_ui_canvas_release();
}

void demo_sudoku_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr || !s_canvas) return;             // 画布不可用时本页不响应按键
    if (ev == BSP_BTN_PRESS) return;            // 只认 CLICK / DOUBLE / LONG
    if (s_new_game_pending) return;             // 正在出题,吃掉按键

    // 出题失败:OK 短按重试,其余忽略。
    if (s_generate_failed) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) request_new_game();
        return;
    }

    // 已解出:OK 短按换下一档难度并重新出题,其余忽略。
    if (s_model.state == SUDOKU_SOLVED) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            s_difficulty = (sudoku_difficulty_t)((s_difficulty + 1) %
                                                 SUDOKU_DIFFICULTY_COUNT);
            ESP_LOGI(TAG, "切换难度 → %s", DIFF_NAME[s_difficulty]);
            request_new_game();
        }
        return;
    }

    const bool editing = (s_model.mode == SUDOKU_MODE_EDIT);

    switch (btn) {
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) {
            if (editing) sudoku_model_cycle(&s_model, -1);
            else         sudoku_model_move(&s_model, -1);
        } else if (ev == BSP_BTN_LONG) {
            // EDIT 下 move 会被模型拒掉,先 cancel 放弃候选值回 NAVIGATE 再跳行。
            if (editing) sudoku_model_cancel(&s_model);
            sudoku_model_move(&s_model, -9);
        }
        break;

    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) {
            if (editing) sudoku_model_cycle(&s_model, +1);
            else         sudoku_model_move(&s_model, +1);
        } else if (ev == BSP_BTN_LONG) {
            if (editing) sudoku_model_cancel(&s_model);
            sudoku_model_move(&s_model, +9);
        }
        break;

    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK)       sudoku_model_ok(&s_model);
        else if (ev == BSP_BTN_DOUBLE) sudoku_model_clear(&s_model);
        // LONG 由 main.c 拦截去返回菜单,这里不会收到。
        break;

    default:
        break;
    }

    // 9×9 全量重画只是几万次字节写,直接整屏刷,省掉脏格跟踪。
    render_board();
    render_panel();
}
