// main/sudoku_model.h —— 数独纯逻辑模型(出题 + 解题 + 三键交互状态机)。
// 不依赖 ESP-IDF/LVGL,可在主机上编译测试。
//
// 交互模型(UI 把按键映射到这些调用):
//   NAVIGATE 模式:move(±1 / ±9) 移光标;ok() 进入 EDIT(题面给定格 → BLOCKED)。
//   EDIT 模式:cycle(±1) 在 0(空),1..9 间循环;ok() 提交并回到 NAVIGATE。
//   clear():任何模式下清空当前非给定格并回到 NAVIGATE。
//   cancel():EDIT 下放弃候选值回到 NAVIGATE,盘面不变。
// 所有 9 个空格填满且全部正确时 state 变为 SOLVED。
//
// 实现约束:求解/出题必须用显式栈的迭代回溯,不用递归——目标板上任务栈只有几 KB。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SUDOKU_N 9

typedef enum {
    SUDOKU_EASY = 0,   // 约 36..40 个给定数
    SUDOKU_MEDIUM,     // 约 30..34 个给定数
    SUDOKU_HARD,       // 约 24..28 个给定数
    SUDOKU_DIFFICULTY_COUNT,
} sudoku_difficulty_t;

typedef enum {
    SUDOKU_MODE_NAVIGATE = 0,
    SUDOKU_MODE_EDIT,
} sudoku_mode_t;

typedef enum {
    SUDOKU_PLAYING = 0,
    SUDOKU_SOLVED,
} sudoku_state_t;

typedef enum {
    SUDOKU_EVENT_NONE = 0,
    SUDOKU_EVENT_MOVED,
    SUDOKU_EVENT_EDIT_BEGIN,    // 进入 EDIT
    SUDOKU_EVENT_EDIT_CHANGED,  // EDIT 中候选值变化
    SUDOKU_EVENT_COMMITTED,     // 提交了一个值(含提交为空)
    SUDOKU_EVENT_CLEARED,
    SUDOKU_EVENT_CANCELLED,     // 放弃了 EDIT 中的候选值,盘面未改动
    SUDOKU_EVENT_BLOCKED,       // 给定格不可编辑 / 已解出后继续操作
    SUDOKU_EVENT_SOLVED,
} sudoku_event_t;

typedef struct {
    uint8_t given[SUDOKU_N][SUDOKU_N];    // 题面,0 = 空
    uint8_t cell[SUDOKU_N][SUDOKU_N];     // 当前盘面(含题面给定数)
    uint8_t solution[SUDOKU_N][SUDOKU_N]; // 唯一解
    uint8_t cursor_x;
    uint8_t cursor_y;
    sudoku_mode_t mode;
    uint8_t edit_value;                   // EDIT 模式下正在选择的值 0..9
    uint8_t empty_count;                  // 当前盘面空格数
    sudoku_state_t state;
    sudoku_difficulty_t difficulty;
    uint32_t rng;
} sudoku_model_t;

// 初始化 rng,不出题(cell 全 0,state PLAYING)。
void sudoku_model_init(sudoku_model_t *m, uint32_t seed);
// 生成一道唯一解的新题并重置交互状态。失败(极少)返回 false 并保持旧盘面。
bool sudoku_model_new_game(sudoku_model_t *m, sudoku_difficulty_t difficulty);

sudoku_event_t sudoku_model_move(sudoku_model_t *m, int delta);   // 行优先线性移动,回绕
sudoku_event_t sudoku_model_ok(sudoku_model_t *m);
sudoku_event_t sudoku_model_cycle(sudoku_model_t *m, int dir);    // EDIT 模式:±1 循环 0..9;NAVIGATE 下返回 BLOCKED
sudoku_event_t sudoku_model_clear(sudoku_model_t *m);
// 放弃当前 EDIT 候选值,回到 NAVIGATE,不改动盘面。非 EDIT 模式返回 BLOCKED。
sudoku_event_t sudoku_model_cancel(sudoku_model_t *m);

bool sudoku_model_is_given(const sudoku_model_t *m, int x, int y);
// 该格当前值与同行/同列/同宫的其他值冲突(值为 0 时 false)。UI 用来标红。
bool sudoku_model_conflicts(const sudoku_model_t *m, int x, int y);
bool sudoku_model_cursor_at(const sudoku_model_t *m, int x, int y);
// 伪随机 [0, limit)。limit 为 0 时返回 0。
uint32_t sudoku_model_random(sudoku_model_t *m, uint32_t limit);

// ---- 求解器/出题器内部函数,导出供测试 ----
// 原地求解;有解返回 true。迭代回溯,不递归。
bool sudoku_solve(uint8_t grid[SUDOKU_N][SUDOKU_N]);
// 统计解的个数,最多数到 limit 即返回(用于唯一性检查,limit 传 2)。
int  sudoku_count_solutions(const uint8_t grid[SUDOKU_N][SUDOKU_N], int limit);
// 盘面是否合法(无冲突;允许有空格)。
bool sudoku_grid_valid(const uint8_t grid[SUDOKU_N][SUDOKU_N]);
