// tests/test_sudoku_model.c —— 数独模型的主机侧单元测试。
//
// 只依赖 C 标准库,不需要 ESP-IDF。编译运行:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_sudoku_model.c main/sudoku_model.c -o /tmp/test_sudoku
//   /tmp/test_sudoku
// 全部断言通过时 main 返回 0。

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sudoku_model.h"

// ---------------------------------------------------------------------------
// 测试辅助
// ---------------------------------------------------------------------------

// 维基百科上的经典例题(行优先 81 字符,'.' 或 '0' 表示空格),已知唯一解。
static const char *const KNOWN_PUZZLE =
    "53..7...."
    "6..195..."
    ".98....6."
    "8...6...3"
    "4..8.3..1"
    "7...2...6"
    ".6....28."
    "...419..5"
    "....8..79";

// KNOWN_PUZZLE 的唯一解,用来逐格比对求解器输出。
static const char *const KNOWN_SOLUTION =
    "534678912"
    "672195348"
    "198342567"
    "859761423"
    "426853791"
    "713924856"
    "961537284"
    "287419635"
    "345286179";

/** 把 81 字符的行优先字符串解析成 9×9 盘面;'.' 与 '0' 都算空格。 */
static void parse_grid(const char *text, uint8_t grid[SUDOKU_N][SUDOKU_N])
{
    assert(strlen(text) == (size_t)(SUDOKU_N * SUDOKU_N));
    for (int i = 0; i < SUDOKU_N * SUDOKU_N; i++) {
        char ch = text[i];
        grid[i / SUDOKU_N][i % SUDOKU_N] =
            (ch == '.' || ch == '0') ? 0U : (uint8_t)(ch - '0');
    }
}

/** 盘面是否已填满(无 0)。 */
static bool grid_full(const uint8_t grid[SUDOKU_N][SUDOKU_N])
{
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            if (grid[r][c] == 0U) {
                return false;
            }
        }
    }
    return true;
}

/** 统计题面里的给定数(非空格)个数。 */
static int count_given(const uint8_t grid[SUDOKU_N][SUDOKU_N])
{
    int n = 0;
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            n += (grid[r][c] != 0U);
        }
    }
    return n;
}

/** 在 NAVIGATE 模式下把光标移到 (x, y),内部走公开的 move() 接口。 */
static void goto_cell(sudoku_model_t *m, int x, int y)
{
    assert(m->mode == SUDOKU_MODE_NAVIGATE);
    int cur = (int)m->cursor_y * SUDOKU_N + (int)m->cursor_x;
    int dst = y * SUDOKU_N + x;
    sudoku_model_move(m, dst - cur);
    assert(sudoku_model_cursor_at(m, x, y));
}

/** 在 EDIT 模式下把候选值 cycle 到 value(只按 +1 方向转,最多 10 步)。 */
static void cycle_to(sudoku_model_t *m, uint8_t value)
{
    assert(m->mode == SUDOKU_MODE_EDIT);
    for (int i = 0; i < SUDOKU_N + 1 && m->edit_value != value; i++) {
        sudoku_event_t ev = sudoku_model_cycle(m, +1);
        assert(ev == SUDOKU_EVENT_EDIT_CHANGED);
    }
    assert(m->edit_value == value);
}

/** 走完整交互链路(定位 → 进入 EDIT → 选值 → 提交)在 (x, y) 填入 value。 */
static sudoku_event_t play_cell(sudoku_model_t *m, int x, int y, uint8_t value)
{
    goto_cell(m, x, y);
    sudoku_event_t ev = sudoku_model_ok(m);
    assert(ev == SUDOKU_EVENT_EDIT_BEGIN);
    cycle_to(m, value);
    return sudoku_model_ok(m);
}

// ---------------------------------------------------------------------------
// 求解器 / 校验器
// ---------------------------------------------------------------------------

static void test_solve_known_puzzle(void)
{
    uint8_t grid[SUDOKU_N][SUDOKU_N];
    uint8_t expect[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, grid);
    parse_grid(KNOWN_SOLUTION, expect);

    assert(sudoku_solve(grid));
    assert(grid_full(grid));
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])grid));
    assert(memcmp(grid, expect, sizeof(expect)) == 0);
}

static void test_solve_rejects_unsolvable(void)
{
    // 第一行放 1..8 与一个 9 之外的空位,同列再制造矛盾:构造一个无解盘面。
    uint8_t grid[SUDOKU_N][SUDOKU_N];
    memset(grid, 0, sizeof(grid));
    for (int c = 0; c < 8; c++) {
        grid[0][c] = (uint8_t)(c + 1);
    }
    grid[1][8] = 9U;   // (0,8) 只能填 9,但同列已经有 9 → 无解
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])grid, 2) == 0);
    assert(sudoku_solve(grid) == false);
}

static void test_count_solutions(void)
{
    uint8_t puzzle[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, puzzle);
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])puzzle, 2) == 1);
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])puzzle, 1) == 1);

    // 空盘有天文数字个解,limit=2 时必须及时刹车返回 2。
    uint8_t empty[SUDOKU_N][SUDOKU_N];
    memset(empty, 0, sizeof(empty));
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])empty, 2) == 2);
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])empty, 1) == 1);
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])empty, 0) == 0);

    // 终盘(已填满且合法)恰好一个解:它自己。
    uint8_t full[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_SOLUTION, full);
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])full, 2) == 1);

    // 从唯一解题面再挖掉一格,常常会退化成多解;这里挖掉一整宫保证多解。
    uint8_t loose[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, loose);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            loose[r][c] = 0U;
        }
    }
    for (int r = 3; r < 6; r++) {
        for (int c = 3; c < 6; c++) {
            loose[r][c] = 0U;
        }
    }
    assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])loose, 2) == 2);
}

static void test_grid_valid(void)
{
    uint8_t grid[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, grid);
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])grid));   // 允许有空格

    uint8_t empty[SUDOKU_N][SUDOKU_N];
    memset(empty, 0, sizeof(empty));
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])empty));

    uint8_t row_dup[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, row_dup);
    row_dup[0][8] = 5U;   // 第 0 行已有 5
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])row_dup) == false);

    uint8_t col_dup[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, col_dup);
    col_dup[8][0] = 5U;   // 第 0 列已有 5
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])col_dup) == false);

    uint8_t box_dup[SUDOKU_N][SUDOKU_N];
    parse_grid(KNOWN_PUZZLE, box_dup);
    box_dup[2][2] = 5U;   // 左上宫已有 5
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])box_dup) == false);

    uint8_t out_of_range[SUDOKU_N][SUDOKU_N];
    memset(out_of_range, 0, sizeof(out_of_range));
    out_of_range[4][4] = 10U;
    assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])out_of_range) == false);
}

// ---------------------------------------------------------------------------
// 随机数与出题
// ---------------------------------------------------------------------------

static void test_random(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 12345U);
    assert(sudoku_model_random(&m, 0U) == 0U);   // limit 为 0 时约定返回 0
    for (int i = 0; i < 1000; i++) {
        assert(sudoku_model_random(&m, 9U) < 9U);
    }
    assert(sudoku_model_random(&m, 1U) == 0U);

    // 种子为 0 时必须换成非零常量,否则 xorshift32 会锁死在 0 上。
    sudoku_model_t z;
    sudoku_model_init(&z, 0U);
    assert(z.rng != 0U);
    bool saw_nonzero = false;
    for (int i = 0; i < 32; i++) {
        saw_nonzero |= (sudoku_model_random(&z, 1000U) != 0U);
    }
    assert(saw_nonzero);
}

static void test_init_state(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 7U);
    assert(m.rng == 7U);
    assert(m.state == SUDOKU_PLAYING);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);
    assert(m.cursor_x == 0U && m.cursor_y == 0U);
    assert(m.empty_count == SUDOKU_N * SUDOKU_N);
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            assert(m.cell[r][c] == 0U && m.given[r][c] == 0U);
        }
    }
}

// 各难度的给定数区间,必须与 sudoku_model.h 里 sudoku_difficulty_t 的注释一致。
static const int EXPECT_GIVEN_RANGE[SUDOKU_DIFFICULTY_COUNT][2] = {
    { 36, 40 },  // SUDOKU_EASY
    { 30, 34 },  // SUDOKU_MEDIUM
    { 24, 28 },  // SUDOKU_HARD
};

static void test_new_game_all_difficulties(void)
{
    for (int d = 0; d < SUDOKU_DIFFICULTY_COUNT; d++) {
        sudoku_difficulty_t diff = (sudoku_difficulty_t)d;
        for (int round = 0; round < 3; round++) {
            sudoku_model_t m;
            sudoku_model_init(&m, (uint32_t)(0xA5A50000U + (uint32_t)(d * 16 + round)));
            assert(sudoku_model_new_game(&m, diff));

            assert(m.difficulty == diff);
            assert(m.state == SUDOKU_PLAYING);
            assert(m.mode == SUDOKU_MODE_NAVIGATE);
            assert(m.cursor_x == 0U && m.cursor_y == 0U);
            assert(m.edit_value == 0U);

            // 终盘必须是填满的合法盘面。
            assert(grid_full(m.solution));
            assert(sudoku_grid_valid((const uint8_t (*)[SUDOKU_N])m.solution));

            // 题面必须唯一解,且给定数落在该难度的区间里。
            int given = count_given(m.given);
            assert(given >= EXPECT_GIVEN_RANGE[d][0]);
            assert(given <= EXPECT_GIVEN_RANGE[d][1]);
            assert(sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])m.given, 2) == 1);

            // 开局盘面等于题面,空格数与给定数互补。
            assert(memcmp(m.cell, m.given, sizeof(m.cell)) == 0);
            assert(m.empty_count == SUDOKU_N * SUDOKU_N - given);

            // 每个给定数都必须与唯一解一致(题面是从终盘挖出来的)。
            for (int r = 0; r < SUDOKU_N; r++) {
                for (int c = 0; c < SUDOKU_N; c++) {
                    if (m.given[r][c] != 0U) {
                        assert(m.given[r][c] == m.solution[r][c]);
                    }
                }
            }
        }
    }
}

static void test_new_game_rejects_bad_difficulty(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 999U);
    assert(sudoku_model_new_game(&m, SUDOKU_EASY));
    uint8_t snapshot[SUDOKU_N][SUDOKU_N];
    memcpy(snapshot, m.given, sizeof(snapshot));

    // 非法难度必须失败,并且保持旧盘面不变。
    assert(sudoku_model_new_game(&m, (sudoku_difficulty_t)SUDOKU_DIFFICULTY_COUNT) == false);
    assert(memcmp(snapshot, m.given, sizeof(snapshot)) == 0);
}

static void test_new_game_deterministic(void)
{
    // 同一个种子 + 同一难度必须产出完全相同的题目,方便复现问题与回归测试。
    sudoku_model_t a;
    sudoku_model_t b;
    sudoku_model_init(&a, 0xDEADBEEFU);
    sudoku_model_init(&b, 0xDEADBEEFU);
    assert(sudoku_model_new_game(&a, SUDOKU_MEDIUM));
    assert(sudoku_model_new_game(&b, SUDOKU_MEDIUM));
    assert(memcmp(a.given, b.given, sizeof(a.given)) == 0);
    assert(memcmp(a.solution, b.solution, sizeof(a.solution)) == 0);
    assert(a.empty_count == b.empty_count);
    assert(a.rng == b.rng);

    // 不同种子应当给出不同题目(概率上几乎必然)。
    sudoku_model_t c;
    sudoku_model_init(&c, 0x12345678U);
    assert(sudoku_model_new_game(&c, SUDOKU_MEDIUM));
    assert(memcmp(a.given, c.given, sizeof(a.given)) != 0);
}

// ---------------------------------------------------------------------------
// 交互状态机
// ---------------------------------------------------------------------------

static void test_move_wraps(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 4242U);

    // 行优先线性移动:-1 从 (0,0) 回绕到 (8,8)。
    assert(sudoku_model_move(&m, -1) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 8, 8));
    assert(sudoku_model_move(&m, +1) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 0, 0));

    // ±9 上下移动一行,同样回绕。
    assert(sudoku_model_move(&m, +9) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 0, 1));
    assert(sudoku_model_move(&m, -9) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 0, 0));
    assert(sudoku_model_move(&m, -9) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 0, 8));

    // 行末 +1 进入下一行行首。
    goto_cell(&m, 8, 0);
    assert(sudoku_model_move(&m, +1) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 0, 1));

    // 走满一圈(81 步)回到原处,视为"没动"。
    goto_cell(&m, 3, 4);
    assert(sudoku_model_move(&m, 81) == SUDOKU_EVENT_NONE);
    assert(sudoku_model_cursor_at(&m, 3, 4));
    assert(sudoku_model_move(&m, 0) == SUDOKU_EVENT_NONE);
    assert(sudoku_model_cursor_at(&m, 3, 4));

    // 大步长也要正确取模。
    assert(sudoku_model_move(&m, -170) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, 4, 3));
}

/** 在题面里找一个给定格,写回 (*x, *y)。 */
static void find_given_cell(const sudoku_model_t *m, int *x, int *y)
{
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            if (m->given[r][c] != 0U) {
                *x = c;
                *y = r;
                return;
            }
        }
    }
    assert(0 && "题面里应当存在给定格");
}

/** 在题面里找一个空格(非给定格),写回 (*x, *y)。 */
static void find_empty_cell(const sudoku_model_t *m, int *x, int *y)
{
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            if (m->given[r][c] == 0U) {
                *x = c;
                *y = r;
                return;
            }
        }
    }
    assert(0 && "题面里应当存在空格");
}

static void test_given_cells_blocked(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 5150U);
    assert(sudoku_model_new_game(&m, SUDOKU_EASY));

    int gx = 0;
    int gy = 0;
    find_given_cell(&m, &gx, &gy);
    goto_cell(&m, gx, gy);
    assert(sudoku_model_is_given(&m, gx, gy));

    uint8_t before = m.cell[gy][gx];
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_BLOCKED);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);        // 没有进入 EDIT
    assert(sudoku_model_clear(&m) == SUDOKU_EVENT_BLOCKED);
    assert(m.cell[gy][gx] == before);              // 给定值没被清掉

    // NAVIGATE 模式下 cycle 不合法。
    assert(sudoku_model_cycle(&m, +1) == SUDOKU_EVENT_BLOCKED);

    // 越界坐标查询要安全返回 false,不能越界读。
    assert(sudoku_model_is_given(&m, -1, 0) == false);
    assert(sudoku_model_is_given(&m, 0, SUDOKU_N) == false);
    assert(sudoku_model_conflicts(&m, -1, -1) == false);
}

static void test_edit_cycle_commit_clear(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 0x0BADF00DU);
    assert(sudoku_model_new_game(&m, SUDOKU_MEDIUM));

    int ex = 0;
    int ey = 0;
    find_empty_cell(&m, &ex, &ey);
    goto_cell(&m, ex, ey);
    uint8_t empty_before = m.empty_count;

    // 进入 EDIT:候选值初始化为当前格的值(空格 → 0)。
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_EDIT_BEGIN);
    assert(m.mode == SUDOKU_MODE_EDIT);
    assert(m.edit_value == 0U);

    // cycle 在 0..9 之间循环:+1 十次回到原点。
    for (uint8_t expect = 1; expect <= SUDOKU_N; expect++) {
        assert(sudoku_model_cycle(&m, +1) == SUDOKU_EVENT_EDIT_CHANGED);
        assert(m.edit_value == expect);
    }
    assert(sudoku_model_cycle(&m, +1) == SUDOKU_EVENT_EDIT_CHANGED);
    assert(m.edit_value == 0U);            // 9 → 0 回绕
    assert(sudoku_model_cycle(&m, -1) == SUDOKU_EVENT_EDIT_CHANGED);
    assert(m.edit_value == 9U);            // 0 → 9 反向回绕
    assert(sudoku_model_cycle(&m, 0) == SUDOKU_EVENT_NONE);
    assert(m.edit_value == 9U);

    // EDIT 模式下 move 不是合法动作(±1 已经映射给 cycle)。
    assert(sudoku_model_move(&m, +1) == SUDOKU_EVENT_BLOCKED);
    assert(sudoku_model_cursor_at(&m, ex, ey));

    // 提交:回到 NAVIGATE,盘面与空格数同步更新。
    cycle_to(&m, 4U);
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_COMMITTED);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);
    assert(m.cell[ey][ex] == 4U);
    assert(m.empty_count == empty_before - 1U);
    assert(m.given[ey][ex] == 0U);         // 题面不受影响

    // 再次进入 EDIT 时,候选值从当前格已有的值开始。
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_EDIT_BEGIN);
    assert(m.edit_value == 4U);

    // EDIT 中 clear:清空并直接回到 NAVIGATE。
    assert(sudoku_model_clear(&m) == SUDOKU_EVENT_CLEARED);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);
    assert(m.cell[ey][ex] == 0U);
    assert(m.empty_count == empty_before);

    // 提交 0 等价于把格子留空,同样算一次 COMMITTED。
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_EDIT_BEGIN);
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_COMMITTED);
    assert(m.cell[ey][ex] == 0U);
    assert(m.empty_count == empty_before);
}

static void test_cancel_discards_edit(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 0x5A5AC0DEU);
    assert(sudoku_model_new_game(&m, SUDOKU_MEDIUM));

    // NAVIGATE 模式下没有候选值可放弃。
    assert(m.mode == SUDOKU_MODE_NAVIGATE);
    assert(sudoku_model_cancel(&m) == SUDOKU_EVENT_BLOCKED);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);

    int ex = 0;
    int ey = 0;
    find_empty_cell(&m, &ex, &ey);
    goto_cell(&m, ex, ey);

    // 场景一:在空格上进入 EDIT,cycle 几次后 cancel,盘面必须保持原样。
    uint8_t cell_before = m.cell[ey][ex];
    uint8_t empty_before = m.empty_count;
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_EDIT_BEGIN);
    for (int i = 0; i < 3; i++) {
        assert(sudoku_model_cycle(&m, +1) == SUDOKU_EVENT_EDIT_CHANGED);
    }
    assert(m.edit_value == 3U);
    assert(sudoku_model_cancel(&m) == SUDOKU_EVENT_CANCELLED);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);
    assert(m.edit_value == 0U);
    assert(m.cell[ey][ex] == cell_before);       // 盘面没被写入候选值
    assert(m.empty_count == empty_before);
    assert(m.state == SUDOKU_PLAYING);

    // cancel 之后光标还在原处,move 立刻恢复正常(这正是 UI 长按跨行的用法)。
    assert(sudoku_model_cursor_at(&m, ex, ey));
    assert(sudoku_model_move(&m, +9) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, ex, (ey + 1) % SUDOKU_N));
    assert(sudoku_model_move(&m, -9) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_cursor_at(&m, ex, ey));

    // 场景二:格子里已有值时 cancel,原值必须留住,不能被当成"清空"。
    assert(play_cell(&m, ex, ey, 7U) == SUDOKU_EVENT_COMMITTED);
    assert(m.cell[ey][ex] == 7U);
    uint8_t empty_with_value = m.empty_count;
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_EDIT_BEGIN);
    assert(m.edit_value == 7U);                  // 从当前值开始
    assert(sudoku_model_cycle(&m, +1) == SUDOKU_EVENT_EDIT_CHANGED);
    assert(m.edit_value == 8U);
    assert(sudoku_model_cancel(&m) == SUDOKU_EVENT_CANCELLED);
    assert(m.cell[ey][ex] == 7U);                // 改到一半放弃,保留原来的 7
    assert(m.empty_count == empty_with_value);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);

    // 连续 cancel:第二次已经不在 EDIT,应当 BLOCKED。
    assert(sudoku_model_cancel(&m) == SUDOKU_EVENT_BLOCKED);
    assert(m.cell[ey][ex] == 7U);

    // 收尾:把这格清掉,不影响其它测试对该模型的假设。
    goto_cell(&m, ex, ey);
    assert(sudoku_model_clear(&m) == SUDOKU_EVENT_CLEARED);
    assert(m.cell[ey][ex] == 0U);
}

static void test_conflicts(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 24680U);
    assert(sudoku_model_new_game(&m, SUDOKU_MEDIUM));

    // 找一行里同时有给定数和空格的位置,把给定数抄进空格 → 必然同行冲突。
    int row = -1;
    int hole = -1;
    uint8_t dup = 0U;
    for (int r = 0; r < SUDOKU_N && row < 0; r++) {
        int found_hole = -1;
        uint8_t found_given = 0U;
        for (int c = 0; c < SUDOKU_N; c++) {
            if (m.given[r][c] == 0U && found_hole < 0) {
                found_hole = c;
            } else if (m.given[r][c] != 0U && found_given == 0U) {
                found_given = m.given[r][c];
            }
        }
        if (found_hole >= 0 && found_given != 0U) {
            row = r;
            hole = found_hole;
            dup = found_given;
        }
    }
    assert(row >= 0);

    assert(sudoku_model_conflicts(&m, hole, row) == false);   // 空格不算冲突
    assert(play_cell(&m, hole, row, dup) == SUDOKU_EVENT_COMMITTED);
    assert(sudoku_model_conflicts(&m, hole, row));            // 与同行的给定数撞车

    // 冲突是对称的:被撞的那个给定格也应当标红。
    int other = -1;
    for (int c = 0; c < SUDOKU_N; c++) {
        if (c != hole && m.cell[row][c] == dup) {
            other = c;
            break;
        }
    }
    assert(other >= 0);
    assert(sudoku_model_conflicts(&m, other, row));

    // 清掉以后双方都不再冲突。
    goto_cell(&m, hole, row);
    assert(sudoku_model_clear(&m) == SUDOKU_EVENT_CLEARED);
    assert(sudoku_model_conflicts(&m, hole, row) == false);
    assert(sudoku_model_conflicts(&m, other, row) == false);

    // conflicts 只看同行/列/宫,不与 solution 比较:
    // 填一个"行列宫都合法但不是正确答案"的值,不应被判为冲突。
    int wx = -1;
    int wy = -1;
    uint8_t wrong = 0U;
    for (int r = 0; r < SUDOKU_N && wx < 0; r++) {
        for (int c = 0; c < SUDOKU_N && wx < 0; c++) {
            if (m.given[r][c] != 0U) {
                continue;
            }
            for (uint8_t v = 1; v <= SUDOKU_N; v++) {
                if (v == m.solution[r][c]) {
                    continue;
                }
                m.cell[r][c] = v;
                bool bad = sudoku_model_conflicts(&m, c, r);
                m.cell[r][c] = 0U;
                if (!bad) {
                    wx = c;
                    wy = r;
                    wrong = v;
                    break;
                }
            }
        }
    }
    assert(wx >= 0 && wrong != m.solution[wy][wx]);
    assert(play_cell(&m, wx, wy, wrong) == SUDOKU_EVENT_COMMITTED);
    assert(sudoku_model_conflicts(&m, wx, wy) == false);   // 值错但不冲突 → 不标红
    assert(m.state == SUDOKU_PLAYING);
    goto_cell(&m, wx, wy);
    assert(sudoku_model_clear(&m) == SUDOKU_EVENT_CLEARED);
}

static void test_fill_solution_reaches_solved(void)
{
    sudoku_model_t m;
    sudoku_model_init(&m, 0xC0FFEEU);
    assert(sudoku_model_new_game(&m, SUDOKU_HARD));

    int remaining = m.empty_count;
    assert(remaining > 0);

    // 先故意填错一格再改回来,验证"填满但不全对"不会误判成 SOLVED。
    int fx = 0;
    int fy = 0;
    find_empty_cell(&m, &fx, &fy);
    uint8_t right = m.solution[fy][fx];
    uint8_t wrong = (uint8_t)(right % SUDOKU_N + 1U);   // 任意一个不等于 right 的 1..9
    assert(wrong != right);
    assert(play_cell(&m, fx, fy, wrong) == SUDOKU_EVENT_COMMITTED);
    assert(m.state == SUDOKU_PLAYING);

    // 把剩下的空格全部按 solution 填上,最后一格之前都不能变成 SOLVED。
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            if (m.given[r][c] != 0U || m.cell[r][c] == m.solution[r][c]) {
                continue;
            }
            sudoku_event_t ev = play_cell(&m, c, r, m.solution[r][c]);
            if (m.empty_count == 0U && memcmp(m.cell, m.solution, sizeof(m.cell)) == 0) {
                assert(ev == SUDOKU_EVENT_SOLVED);
            } else {
                assert(ev == SUDOKU_EVENT_COMMITTED);
                assert(m.state == SUDOKU_PLAYING);
            }
        }
    }

    // 最后那格(填错的 fx, fy)改回正确值时才应触发 SOLVED。
    if (m.state != SUDOKU_SOLVED) {
        assert(play_cell(&m, fx, fy, right) == SUDOKU_EVENT_SOLVED);
    }
    assert(m.state == SUDOKU_SOLVED);
    assert(m.empty_count == 0U);
    assert(memcmp(m.cell, m.solution, sizeof(m.cell)) == 0);

    // SOLVED 之后所有编辑动作都被拒,只有移动光标还允许。
    uint8_t snapshot[SUDOKU_N][SUDOKU_N];
    memcpy(snapshot, m.cell, sizeof(snapshot));
    assert(sudoku_model_ok(&m) == SUDOKU_EVENT_BLOCKED);
    assert(sudoku_model_cycle(&m, +1) == SUDOKU_EVENT_BLOCKED);
    assert(sudoku_model_clear(&m) == SUDOKU_EVENT_BLOCKED);
    assert(sudoku_model_cancel(&m) == SUDOKU_EVENT_BLOCKED);
    assert(m.mode == SUDOKU_MODE_NAVIGATE);
    assert(memcmp(snapshot, m.cell, sizeof(snapshot)) == 0);

    assert(sudoku_model_move(&m, +1) == SUDOKU_EVENT_MOVED);
    assert(sudoku_model_move(&m, -10) == SUDOKU_EVENT_MOVED);
    assert(m.state == SUDOKU_SOLVED);

    // 开新局应当把一切复位。
    assert(sudoku_model_new_game(&m, SUDOKU_EASY));
    assert(m.state == SUDOKU_PLAYING);
    assert(m.empty_count > 0U);
}

int main(void)
{
    test_solve_known_puzzle();
    test_solve_rejects_unsolvable();
    test_count_solutions();
    test_grid_valid();
    test_random();
    test_init_state();
    test_new_game_all_difficulties();
    test_new_game_rejects_bad_difficulty();
    test_new_game_deterministic();
    test_move_wraps();
    test_given_cells_blocked();
    test_edit_cycle_commit_clear();
    test_cancel_discards_edit();
    test_conflicts();
    test_fill_solution_reaches_solved();
    printf("all sudoku model tests passed\n");
    return 0;
}
