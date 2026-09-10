// main/mines_model.h —— 扫雷纯逻辑模型。不依赖 ESP-IDF/LVGL,可在主机上编译测试。
//
// 棋盘 9×9、10 雷。光标按行优先线性移动(UP/DOWN = ±1,长按 = ±MINES_W),越界回绕。
// 首次翻开前不布雷,保证首次翻开既不踩雷、周围也无雷(3×3 安全区)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MINES_W     9
#define MINES_H     9
#define MINES_COUNT 10

// 每格一个字节:低 4 位 = 周围雷数 0..8;高位为状态标志。
#define MINES_CELL_ADJ_MASK  0x0F
#define MINES_CELL_MINE      0x10
#define MINES_CELL_REVEALED  0x20
#define MINES_CELL_FLAG      0x40
#define MINES_CELL_BOOM      0x80   // 踩中的那颗雷(仅 LOST 时置位,UI 高亮用)

typedef enum {
    MINES_PLAYING = 0,
    MINES_WON,
    MINES_LOST,
} mines_state_t;

typedef enum {
    MINES_EVENT_NONE = 0,
    MINES_EVENT_MOVED,      // 光标移动
    MINES_EVENT_REVEALED,   // 翻开了 ≥1 格(含连锁展开)
    MINES_EVENT_FLAGGED,    // 插旗/拔旗
    MINES_EVENT_BLOCKED,    // 无效操作(翻开已插旗格、已结束后继续操作等)
    MINES_EVENT_WON,
    MINES_EVENT_LOST,
} mines_event_t;

typedef struct {
    uint8_t  cell[MINES_H][MINES_W];
    uint8_t  cursor_x;
    uint8_t  cursor_y;
    bool     mines_placed;     // 首次翻开后才布雷
    uint8_t  flags_used;
    uint8_t  revealed_count;   // 已翻开的非雷格数
    mines_state_t state;
    uint32_t rng;              // 简单 LCG/xorshift 状态
} mines_model_t;

// 初始化 rng 并开新局。
void mines_model_init(mines_model_t *m, uint32_t seed);
// 开新局(保留 rng 状态,棋盘清空、光标归中、未布雷)。
void mines_model_reset(mines_model_t *m);

// 光标线性移动 delta 格(行优先,回绕)。任何状态下都可移动。
mines_event_t mines_model_move(mines_model_t *m, int delta);
// 翻开光标格。首次翻开触发布雷。翻开数字格且周围旗数==数字时执行"和弦"翻开邻格。
mines_event_t mines_model_reveal(mines_model_t *m);
// 在光标格插旗/拔旗。已翻开格返回 BLOCKED。
mines_event_t mines_model_toggle_flag(mines_model_t *m);

// 剩余雷数提示 = MINES_COUNT - flags_used(可为负)。
int mines_model_remaining(const mines_model_t *m);
// 光标是否在 (x, y)。
bool mines_model_cursor_at(const mines_model_t *m, int x, int y);
// 伪随机 [0, limit)。limit 为 0 时返回 0。
uint32_t mines_model_random(mines_model_t *m, uint32_t limit);
