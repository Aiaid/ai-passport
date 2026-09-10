// tests/test_mines_model.c —— 扫雷纯逻辑模型的主机端单元测试。
//
// 只依赖标准库,用 assert 断言,全部通过时 main 返回 0。编译运行:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_mines_model.c main/mines_model.c -o /tmp/test_mines && /tmp/test_mines
//
// 测试策略:
//  - 随机布雷部分只断言"不变量"(雷数恰好 10、安全区无雷、adjacent 数与独立重算一致);
//  - 需要确定性行为的场景(和弦、踩雷、胜利)则由测试自己手工摆一个已知雷区,
//    直接置 mines_placed = true 并填 cell 数组,绕开随机布雷。

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "mines_model.h"

// 棋盘总格数,与模型内部保持一致。
#define TOTAL (MINES_W * MINES_H)

// ---------------------------------------------------------------- 测试辅助 ----

/**
 * 把光标直接放到 (x, y),不经过 move 的回绕逻辑,便于构造测试场景。
 */
static void set_cursor(mines_model_t *m, int x, int y) {
    m->cursor_x = (uint8_t)x;
    m->cursor_y = (uint8_t)y;
}

/**
 * 独立重算某格 8 邻域雷数(不复用模型内部实现),用于交叉验证 adjacent 字段。
 */
static int ref_adjacent(const mines_model_t *m, int x, int y) {
    int count = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx;
            int ny = y + dy;
            if (nx < 0 || nx >= MINES_W || ny < 0 || ny >= MINES_H) continue;
            if (m->cell[ny][nx] & MINES_CELL_MINE) count++;
        }
    }
    return count;
}

/** 统计全盘雷数。 */
static int count_mines(const mines_model_t *m) {
    int count = 0;
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (m->cell[y][x] & MINES_CELL_MINE) count++;
        }
    }
    return count;
}

/** 统计已翻开的格子数(含雷格,用于失败后的可见性检查)。 */
static int count_revealed(const mines_model_t *m) {
    int count = 0;
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (m->cell[y][x] & MINES_CELL_REVEALED) count++;
        }
    }
    return count;
}

/**
 * 用 9 行、每行 9 字符的布局串手工摆雷:'*' 表示雷,其它字符表示空格。
 * 会同时填好 adjacent 低 4 位并置 mines_placed = true,让后续 reveal 不再触发随机布雷。
 */
static void setup_board(mines_model_t *m, const char *layout[MINES_H]) {
    mines_model_reset(m);
    for (int y = 0; y < MINES_H; y++) {
        assert(strlen(layout[y]) == (size_t)MINES_W);
        for (int x = 0; x < MINES_W; x++) {
            m->cell[y][x] = (layout[y][x] == '*') ? MINES_CELL_MINE : 0;
        }
    }
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            m->cell[y][x] = (uint8_t)(m->cell[y][x] | (uint8_t)ref_adjacent(m, x, y));
        }
    }
    m->mines_placed = true;
}

// ---------------------------------------------------------------- 测试用例 ----

/** init/reset 后的初始状态:空盘、光标居中、计数清零、状态 PLAYING。 */
static void test_init_and_reset(void) {
    mines_model_t m;
    mines_model_init(&m, 12345);

    assert(m.state == MINES_PLAYING);
    assert(m.cursor_x == 4 && m.cursor_y == 4);
    assert(mines_model_cursor_at(&m, 4, 4));
    assert(!mines_model_cursor_at(&m, 4, 3));
    assert(m.mines_placed == false);
    assert(m.flags_used == 0);
    assert(m.revealed_count == 0);
    assert(mines_model_remaining(&m) == MINES_COUNT);
    assert(count_mines(&m) == 0);
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) assert(m.cell[y][x] == 0);
    }

    // seed 为 0 时必须被替换成非零常量,否则 xorshift32 会永远输出 0。
    mines_model_t zero;
    mines_model_init(&zero, 0);
    assert(zero.rng != 0);
    assert(mines_model_random(&zero, 100) < 100);
    assert(mines_model_random(&zero, 0) == 0);

    // reset 保留 rng,不重复同一盘;其余字段全部复位。
    mines_model_reveal(&m);
    mines_model_move(&m, 3);
    mines_model_toggle_flag(&m);
    uint32_t rng_before = m.rng;
    mines_model_reset(&m);
    assert(m.rng == rng_before);
    assert(m.state == MINES_PLAYING);
    assert(m.cursor_x == 4 && m.cursor_y == 4);
    assert(m.mines_placed == false);
    assert(m.flags_used == 0 && m.revealed_count == 0);
    assert(count_mines(&m) == 0);
}

/** 光标线性移动:正负回绕、±MINES_W(上下行)、跨首尾、极端 delta。 */
static void test_move_wrap(void) {
    mines_model_t m;
    mines_model_init(&m, 999);

    assert(mines_model_move(&m, 1) == MINES_EVENT_MOVED);
    assert(m.cursor_x == 5 && m.cursor_y == 4);
    assert(mines_model_move(&m, -1) == MINES_EVENT_MOVED);
    assert(m.cursor_x == 4 && m.cursor_y == 4);

    // ±MINES_W 相当于上下移动一行,列不变。
    mines_model_move(&m, MINES_W);
    assert(m.cursor_x == 4 && m.cursor_y == 5);
    mines_model_move(&m, -MINES_W);
    assert(m.cursor_x == 4 && m.cursor_y == 4);

    // 首格向左回绕到末格。
    set_cursor(&m, 0, 0);
    mines_model_move(&m, -1);
    assert(m.cursor_x == MINES_W - 1 && m.cursor_y == MINES_H - 1);
    // 末格向右回绕到首格。
    mines_model_move(&m, 1);
    assert(m.cursor_x == 0 && m.cursor_y == 0);
    // 首行向上回绕到末行。
    mines_model_move(&m, -MINES_W);
    assert(m.cursor_x == 0 && m.cursor_y == MINES_H - 1);
    mines_model_move(&m, MINES_W);
    assert(m.cursor_x == 0 && m.cursor_y == 0);

    // 整周 delta 等价于原地不动。
    set_cursor(&m, 3, 6);
    mines_model_move(&m, TOTAL);
    assert(m.cursor_x == 3 && m.cursor_y == 6);
    mines_model_move(&m, -TOTAL);
    assert(m.cursor_x == 3 && m.cursor_y == 6);

    // 任意大 delta(含负数)按 81 取模,结果始终落在盘内。
    set_cursor(&m, 0, 0);
    mines_model_move(&m, 1000);  // 1000 % 81 = 28
    assert(m.cursor_y * MINES_W + m.cursor_x == 28);
    set_cursor(&m, 0, 0);
    mines_model_move(&m, -1000);  // 回绕到 81 - 28 = 53
    assert(m.cursor_y * MINES_W + m.cursor_x == 53);

    // 极端值不得溢出,只要求结果仍在盘内。
    set_cursor(&m, 4, 4);
    mines_model_move(&m, INT_MAX);
    assert(m.cursor_x < MINES_W && m.cursor_y < MINES_H);
    mines_model_move(&m, INT_MIN);
    assert(m.cursor_x < MINES_W && m.cursor_y < MINES_H);

    // 每个 delta 从每个起点出发,结果必须等于线性索引取模。
    for (int start = 0; start < TOTAL; start++) {
        for (int delta = -200; delta <= 200; delta++) {
            set_cursor(&m, start % MINES_W, start / MINES_W);
            mines_model_move(&m, delta);
            int expect = ((start + delta) % TOTAL + TOTAL) % TOTAL;
            assert(m.cursor_y * MINES_W + m.cursor_x == expect);
        }
    }
}

/** 首次翻开布雷:恰好 10 颗雷、3×3 安全区无雷、adjacent 与独立重算一致、必定连锁展开。 */
static void test_first_reveal_places_mines(void) {
    for (uint32_t seed = 1; seed <= 200; seed++) {
        mines_model_t m;
        mines_model_init(&m, seed * 7919U);

        // 光标覆盖中心、边、角三类位置,确保安全区裁剪在边界上也正确。
        int spots[3][2] = { { 4, 4 }, { 0, 0 }, { MINES_W - 1, 3 } };
        int spot = (int)(seed % 3);
        set_cursor(&m, spots[spot][0], spots[spot][1]);
        int cx = m.cursor_x;
        int cy = m.cursor_y;

        mines_event_t ev = mines_model_reveal(&m);
        assert(ev == MINES_EVENT_REVEALED);
        assert(m.mines_placed);
        assert(m.state == MINES_PLAYING);
        assert(count_mines(&m) == MINES_COUNT);

        // 3×3 安全区内不得有雷,因此首翻格必是 0 格。
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx;
                int ny = cy + dy;
                if (nx < 0 || nx >= MINES_W || ny < 0 || ny >= MINES_H) continue;
                assert(!(m.cell[ny][nx] & MINES_CELL_MINE));
            }
        }
        assert((m.cell[cy][cx] & MINES_CELL_ADJ_MASK) == 0);

        // 逐格重算 adjacent 与模型写入的低 4 位对比。
        for (int y = 0; y < MINES_H; y++) {
            for (int x = 0; x < MINES_W; x++) {
                assert((m.cell[y][x] & MINES_CELL_ADJ_MASK) == (uint8_t)ref_adjacent(&m, x, y));
            }
        }

        // 首翻是 0 格,连锁展开至少覆盖整个安全区(角落时为 4 格)。
        assert(m.revealed_count >= 4);
        assert(m.revealed_count == count_revealed(&m));
        // 已翻开的格子里不能有雷。
        for (int y = 0; y < MINES_H; y++) {
            for (int x = 0; x < MINES_W; x++) {
                if (m.cell[y][x] & MINES_CELL_REVEALED) assert(!(m.cell[y][x] & MINES_CELL_MINE));
            }
        }
    }
}

/** 连锁展开:0 格向外扩散到数字边界为止,插旗格是屏障。 */
static void test_flood_fill(void) {
    // 左上 4×4 区域完全空,右下角集中放雷。
    const char *layout[MINES_H] = {
        ".........",
        ".........",
        ".........",
        ".........",
        ".....*...",
        "......**.",
        ".....*.*.",
        "....*.*..",
        ".....***.",
    };
    mines_model_t m;
    mines_model_init(&m, 4242);
    setup_board(&m, layout);
    assert(count_mines(&m) == MINES_COUNT);

    set_cursor(&m, 0, 0);
    assert(mines_model_reveal(&m) == MINES_EVENT_REVEALED);
    // (0,0) 是 0 格,应一路展开到右下雷区周边的数字格。
    assert(m.revealed_count > 20);
    assert(m.revealed_count == count_revealed(&m));
    assert(m.cell[0][0] & MINES_CELL_REVEALED);
    // 雷格绝不会被连锁展开翻开。
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (m.cell[y][x] & MINES_CELL_MINE) assert(!(m.cell[y][x] & MINES_CELL_REVEALED));
        }
    }
    // 数字格是展开边界:被翻开的数字格,其邻居可以未翻开。
    assert((m.cell[3][4] & MINES_CELL_REVEALED) && (m.cell[3][4] & MINES_CELL_ADJ_MASK) == 1);
    // 已翻开的 0 格再点一次是无效操作。
    set_cursor(&m, 0, 0);
    assert(mines_model_reveal(&m) == MINES_EVENT_BLOCKED);
    // 已翻开的数字格、周围旗数不等于数字时同样无效。
    set_cursor(&m, 4, 3);
    assert(mines_model_reveal(&m) == MINES_EVENT_BLOCKED);

    // 插旗当屏障:重开一局,在 0 格区域中间插旗,展开不会翻开它。
    setup_board(&m, layout);
    set_cursor(&m, 2, 2);
    assert(mines_model_toggle_flag(&m) == MINES_EVENT_FLAGGED);
    set_cursor(&m, 0, 0);
    assert(mines_model_reveal(&m) == MINES_EVENT_REVEALED);
    assert(!(m.cell[2][2] & MINES_CELL_REVEALED));
    assert(m.cell[2][2] & MINES_CELL_FLAG);
}

/** 插旗/拔旗与剩余雷数提示;插旗格拒绝翻开;已翻开格拒绝插旗。 */
static void test_flag_and_remaining(void) {
    mines_model_t m;
    mines_model_init(&m, 20260910);

    set_cursor(&m, 1, 1);
    assert(mines_model_toggle_flag(&m) == MINES_EVENT_FLAGGED);
    assert(m.cell[1][1] & MINES_CELL_FLAG);
    assert(m.flags_used == 1);
    assert(mines_model_remaining(&m) == MINES_COUNT - 1);

    // 插旗格拒绝翻开。
    assert(mines_model_reveal(&m) == MINES_EVENT_BLOCKED);
    assert(!(m.cell[1][1] & MINES_CELL_REVEALED));
    assert(!m.mines_placed);  // 被拒绝的翻开不应触发布雷。

    // 拔旗后恢复。
    assert(mines_model_toggle_flag(&m) == MINES_EVENT_FLAGGED);
    assert(!(m.cell[1][1] & MINES_CELL_FLAG));
    assert(m.flags_used == 0);
    assert(mines_model_remaining(&m) == MINES_COUNT);

    // 旗数可超过雷数,remaining 允许为负。
    for (int i = 0; i < MINES_COUNT + 2; i++) {
        set_cursor(&m, i % MINES_W, i / MINES_W);
        assert(mines_model_toggle_flag(&m) == MINES_EVENT_FLAGGED);
    }
    assert(m.flags_used == MINES_COUNT + 2);
    assert(mines_model_remaining(&m) == -2);

    // 已翻开的格子不能插旗。
    mines_model_reset(&m);
    set_cursor(&m, 4, 4);
    assert(mines_model_reveal(&m) == MINES_EVENT_REVEALED);
    assert(mines_model_toggle_flag(&m) == MINES_EVENT_BLOCKED);
    assert(m.flags_used == 0);
}

/** 和弦(chord)翻开:旗数与数字相符时批量翻开邻格;旗插错则踩雷。 */
static void test_chord_reveal(void) {
    const char *layout[MINES_H] = {
        "*........",
        ".........",
        ".........",
        ".........",
        ".........",
        "....**...",
        "......**.",
        ".....*.*.",
        "....***..",
    };
    mines_model_t m;
    mines_model_init(&m, 777);
    setup_board(&m, layout);
    assert(count_mines(&m) == MINES_COUNT);

    // (1,1) 的 adjacent = 1(左上角那颗雷)。
    set_cursor(&m, 1, 1);
    assert(mines_model_reveal(&m) == MINES_EVENT_REVEALED);
    assert((m.cell[1][1] & MINES_CELL_ADJ_MASK) == 1);
    int revealed_before = m.revealed_count;

    // 旗数不足(0 ≠ 1)时和弦被拒绝。
    assert(mines_model_reveal(&m) == MINES_EVENT_BLOCKED);
    assert(m.revealed_count == revealed_before);

    // 给 (0,0) 插旗后旗数相符,和弦生效并连锁展开一大片。
    set_cursor(&m, 0, 0);
    assert(mines_model_toggle_flag(&m) == MINES_EVENT_FLAGGED);
    set_cursor(&m, 1, 1);
    assert(mines_model_reveal(&m) == MINES_EVENT_REVEALED);
    assert(m.revealed_count > revealed_before);
    assert(m.state == MINES_PLAYING);
    assert(!(m.cell[0][0] & MINES_CELL_REVEALED));  // 插旗格不会被和弦翻开。

    // 邻格都已翻开或插旗时,和弦没有新翻开的格子,视为无效操作。
    assert(mines_model_reveal(&m) == MINES_EVENT_BLOCKED);

    // 旗插错位置时和弦会踩雷。
    setup_board(&m, layout);
    set_cursor(&m, 1, 1);
    assert(mines_model_reveal(&m) == MINES_EVENT_REVEALED);
    set_cursor(&m, 2, 2);  // 空格,却被误插旗
    assert(mines_model_toggle_flag(&m) == MINES_EVENT_FLAGGED);
    set_cursor(&m, 1, 1);
    assert(mines_model_reveal(&m) == MINES_EVENT_LOST);
    assert(m.state == MINES_LOST);
    assert(m.cell[0][0] & MINES_CELL_BOOM);
}

/** 踩雷:BOOM 位落在被踩的那颗雷上,全部雷可见,状态转 LOST。 */
static void test_step_on_mine(void) {
    const char *layout[MINES_H] = {
        "*........",
        ".......*.",
        "....*....",
        ".........",
        "..*...*..",
        ".........",
        "....*.*..",
        ".........",
        "*..*...*.",
    };
    mines_model_t m;
    mines_model_init(&m, 31337);
    setup_board(&m, layout);
    assert(count_mines(&m) == MINES_COUNT);

    set_cursor(&m, 4, 2);
    assert(mines_model_reveal(&m) == MINES_EVENT_LOST);
    assert(m.state == MINES_LOST);
    assert(m.cell[2][4] & MINES_CELL_BOOM);
    assert(m.cell[2][4] & MINES_CELL_REVEALED);
    assert(m.revealed_count == 0);  // 雷格不计入非雷格翻开数。

    // 所有雷都应变为可见,且 BOOM 位只有一处。
    int boom = 0;
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (m.cell[y][x] & MINES_CELL_MINE) assert(m.cell[y][x] & MINES_CELL_REVEALED);
            if (m.cell[y][x] & MINES_CELL_BOOM) boom++;
        }
    }
    assert(boom == 1);
    assert(count_revealed(&m) == MINES_COUNT);
}

/** 胜利:翻开全部非雷格后状态转 WON,剩余雷自动插旗,remaining 归零。 */
static void test_win(void) {
    const char *layout[MINES_H] = {
        "*........",
        ".......*.",
        "....*....",
        ".........",
        "..*...*..",
        ".........",
        "....*.*..",
        ".........",
        "*..*...*.",
    };
    mines_model_t m;
    mines_model_init(&m, 20240101);
    setup_board(&m, layout);

    mines_event_t last = MINES_EVENT_NONE;
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (m.cell[y][x] & MINES_CELL_MINE) continue;
            set_cursor(&m, x, y);
            mines_event_t ev = mines_model_reveal(&m);
            if (ev != MINES_EVENT_BLOCKED) last = ev;
        }
    }

    assert(last == MINES_EVENT_WON);
    assert(m.state == MINES_WON);
    assert(m.revealed_count == TOTAL - MINES_COUNT);
    // 胜利时剩余雷自动插旗,便于 UI 直接呈现完局画面。
    assert(m.flags_used == MINES_COUNT);
    assert(mines_model_remaining(&m) == 0);
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (m.cell[y][x] & MINES_CELL_MINE) {
                assert(m.cell[y][x] & MINES_CELL_FLAG);
                assert(!(m.cell[y][x] & MINES_CELL_BOOM));
            } else {
                assert(m.cell[y][x] & MINES_CELL_REVEALED);
            }
        }
    }
}

/** 游戏结束后:reveal / toggle_flag 一律 BLOCKED,move 仍然允许。 */
static void test_blocked_after_game_over(void) {
    const char *layout[MINES_H] = {
        "*........",
        ".......*.",
        "....*....",
        ".........",
        "..*...*..",
        ".........",
        "....*.*..",
        ".........",
        "*..*...*.",
    };

    // LOST 之后。
    mines_model_t lost;
    mines_model_init(&lost, 555);
    setup_board(&lost, layout);
    set_cursor(&lost, 0, 0);
    assert(mines_model_reveal(&lost) == MINES_EVENT_LOST);
    set_cursor(&lost, 5, 5);
    assert(mines_model_reveal(&lost) == MINES_EVENT_BLOCKED);
    assert(mines_model_toggle_flag(&lost) == MINES_EVENT_BLOCKED);
    assert(lost.flags_used == 0);
    assert(mines_model_move(&lost, 1) == MINES_EVENT_MOVED);
    assert(lost.cursor_x == 6 && lost.cursor_y == 5);

    // WON 之后。
    mines_model_t won;
    mines_model_init(&won, 556);
    setup_board(&won, layout);
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (won.cell[y][x] & MINES_CELL_MINE) continue;
            set_cursor(&won, x, y);
            mines_model_reveal(&won);
        }
    }
    assert(won.state == MINES_WON);
    uint8_t flags_after_win = won.flags_used;
    set_cursor(&won, 1, 1);
    assert(mines_model_reveal(&won) == MINES_EVENT_BLOCKED);
    assert(mines_model_toggle_flag(&won) == MINES_EVENT_BLOCKED);
    assert(won.flags_used == flags_after_win);
    assert(mines_model_move(&won, -1) == MINES_EVENT_MOVED);

    // reset 之后可以重新开局。
    mines_model_reset(&won);
    assert(won.state == MINES_PLAYING);
    assert(mines_model_reveal(&won) == MINES_EVENT_REVEALED);
}

/** 同一 seed + 同一操作序列必须得到完全相同的棋盘(可复现)。 */
static void test_deterministic_with_seed(void) {
    mines_model_t a;
    mines_model_t b;
    mines_model_init(&a, 0xABCDEF01U);
    mines_model_init(&b, 0xABCDEF01U);

    const int deltas[] = { 0, 5, -12, 30, 7, -3, 19 };
    for (size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
        mines_model_move(&a, deltas[i]);
        mines_model_move(&b, deltas[i]);
        mines_model_reveal(&a);
        mines_model_reveal(&b);
    }

    assert(memcmp(a.cell, b.cell, sizeof(a.cell)) == 0);
    assert(a.state == b.state);
    assert(a.revealed_count == b.revealed_count);
    assert(a.flags_used == b.flags_used);
    assert(a.rng == b.rng);

    // 不同 seed 应当得到不同布局(极小概率巧合,这里用固定的一对已知种子)。
    mines_model_t c;
    mines_model_init(&c, 0x12345678U);
    mines_model_reveal(&c);
    mines_model_t d;
    mines_model_init(&d, 0xABCDEF01U);
    mines_model_reveal(&d);
    assert(memcmp(c.cell, d.cell, sizeof(c.cell)) != 0);

    // 连续 reset 沿用 rng,后续布局不会与上一局完全一致。
    mines_model_t e;
    mines_model_init(&e, 0x5A5A5A5AU);
    mines_model_reveal(&e);
    uint8_t first_round[MINES_H][MINES_W];
    memcpy(first_round, e.cell, sizeof(first_round));
    mines_model_reset(&e);
    mines_model_reveal(&e);
    assert(memcmp(first_round, e.cell, sizeof(first_round)) != 0);
}

int main(void) {
    test_init_and_reset();
    test_move_wrap();
    test_first_reveal_places_mines();
    test_flood_fill();
    test_flag_and_remaining();
    test_chord_reveal();
    test_step_on_mine();
    test_win();
    test_blocked_after_game_over();
    test_deterministic_with_seed();
    puts("mines_model: all tests passed");
    return 0;
}
