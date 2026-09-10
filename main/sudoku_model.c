// main/sudoku_model.c —— 数独纯逻辑模型实现(出题 + 解题 + 三键交互状态机)。
//
// 设计要点:
//   1. 只依赖 C 标准库(string.h),不引用 ESP-IDF / LVGL,可直接在主机上编译测试。
//   2. 求解器与出题器一律使用**显式栈的迭代回溯**,不使用递归。原因见头文件:
//      目标板 ESP32-C3 无 PSRAM,LVGL/UI 任务栈只有几 KB,81 层递归的调用帧
//      (每帧保存寄存器 + 局部变量,RISC-V 上轻易 >64 B)会直接冲栈。
//      本实现所有回溯状态都放在两个固定长度数组里(pos[81] + tried[81] = 162 B),
//      加上盘面副本 81 B 与三组位掩码 54 B,单次求解的栈占用 ~360 B,恒定且可预测。
//   3. 约束检查用位掩码(row/col/box 各 9 个 uint16_t,bit v 表示数字 v 已占用),
//      避免每次落子都扫描 27 个格子;配合 MRV(最少候选优先)选格,
//      一道普通题的回溯节点数通常只有几十个,满足出题时反复做唯一性检查的性能预算。

#include "sudoku_model.h"

#include <string.h>

// 棋盘线性化后的格子总数(9×9)。求解器内部一律用 0..80 的线性下标,
// 避免二维下标带来的乘法与边界判断开销。
#define SUDOKU_CELLS (SUDOKU_N * SUDOKU_N)

// 各难度对应的"给定数(题面非空格)"目标区间 [下限, 上限],与头文件注释保持一致。
// 数值来源:头文件 sudoku_difficulty_t 的注释;修改时两处必须同步。
static const uint8_t GIVEN_RANGE[SUDOKU_DIFFICULTY_COUNT][2] = {
    { 36, 40 },  // SUDOKU_EASY
    { 30, 34 },  // SUDOKU_MEDIUM
    { 24, 28 },  // SUDOKU_HARD
};

// 出题时"挖空"这一步允许重试的次数上限。
// 单趟随机顺序挖空通常能挖到 22..28 个给定数,足以覆盖三档难度;
// 只有极少数情况下(随机顺序不利)单趟结束时给定数仍高于难度上限,才需要重来。
// 达到上限仍失败时 sudoku_model_new_game() 返回 false(头文件已声明这种"极少"失败)。
#define SUDOKU_DIG_ATTEMPTS 6

// RNG 种子为 0 时的替代值。xorshift32 的状态一旦为 0 会永远输出 0,
// 因此任何可能写入状态的地方都必须保证非零。取值为常见的 32 位奇数魔数,无特殊含义。
#define SUDOKU_RNG_FALLBACK 0x9E3779B9U

// ---------------------------------------------------------------------------
// 随机数
// ---------------------------------------------------------------------------

/**
 * 推进 xorshift32 状态并返回新的 32 位随机数。
 *
 * @param state  RNG 状态指针,原地更新;调用后保证非零。
 * @return       [1, 2^32-1] 范围内的伪随机数。
 *
 * 说明:xorshift32 周期 2^32-1,质量足够做出题的洗牌与挖空顺序,
 * 且只用三次移位异或,在 160 MHz 的 RISC-V 上开销可忽略。不适合密码学用途。
 */
static uint32_t rng_next(uint32_t *state)
{
    uint32_t x = *state ? *state : SUDOKU_RNG_FALLBACK;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : SUDOKU_RNG_FALLBACK;
    return *state;
}

uint32_t sudoku_model_random(sudoku_model_t *m, uint32_t limit)
{
    if (m == NULL || limit == 0U) {
        return 0U;
    }
    // 直接取模会有轻微的模偏差(limit 不整除 2^32 时),
    // 对出题的随机性无实质影响,换取实现简单与恒定耗时。
    return rng_next(&m->rng) % limit;
}

/**
 * 对 uint8_t 数组做 Fisher-Yates 洗牌(原地)。
 *
 * @param arr    待洗牌数组,长度 >= n。
 * @param n      元素个数;n <= 1 时无操作。
 * @param m      提供 RNG 状态的模型;其 rng 字段会被推进。
 */
static void shuffle_u8(uint8_t *arr, int n, sudoku_model_t *m)
{
    for (int i = n - 1; i > 0; i--) {
        int j = (int)sudoku_model_random(m, (uint32_t)(i + 1));
        uint8_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

// ---------------------------------------------------------------------------
// 求解器(显式栈迭代回溯)
// ---------------------------------------------------------------------------

/**
 * 统计位掩码里还剩多少个可用数字(候选数)。
 *
 * @param used  bit v(v ∈ 1..9)置位表示数字 v 已被同行/列/宫占用。
 * @return      0..9,该格当前的候选数个数。
 */
static int candidate_count(uint16_t used)
{
    int cnt = 0;
    for (uint8_t v = 1; v <= SUDOKU_N; v++) {
        if ((used & (uint16_t)(1U << v)) == 0U) {
            cnt++;
        }
    }
    return cnt;
}

/**
 * 数独求解核心:显式栈迭代回溯,最多找 limit 个解。
 *
 * @param in     输入盘面(只读),0 表示空格;含非法值(>9)或已有冲突时视为无解。
 * @param out    非 NULL 时,把**第一个**找到的解写入这里;为 NULL 则只计数不输出。
 * @param limit  最多数到几个解就返回;<= 0 时直接返回 0。唯一性检查传 2。
 * @return       找到的解的个数,上限为 limit。
 *
 * 副作用:无(不修改 in;只写 out)。不分配堆内存,不阻塞,可在任意任务上下文调用。
 *
 * 栈用量:g[81] + pos[81] + tried[81] + 三组掩码 9*2*3 = 81+81+81+54 = 297 B 局部数据,
 * 加上少量标量,单帧 ~360 B,与题目难度无关(回溯深度不占额外栈)。
 *
 * 算法:
 *   - pos[] 保存所有空格的线性下标;level 是"已确定的空格个数",也就是显式栈深度。
 *   - tried[level] 保存该层当前落下的值(0 = 该层刚进入,尚未落子),
 *     它同时充当"这是新进入的一层还是从下一层回溯回来的"标志。
 *   - 每次新进入一层,用 MRV(minimum remaining values)在 pos[level..n-1] 里
 *     挑候选数最少的格子换到 pos[level],候选数为 0 时立即回溯。这个剪枝把
 *     一道普通题的回溯节点数从上万降到几十,是出题阶段反复做唯一性检查的性能前提。
 */
static int solve_core(const uint8_t in[SUDOKU_N][SUDOKU_N],
                      uint8_t out[SUDOKU_N][SUDOKU_N],
                      int limit)
{
    if (in == NULL || limit <= 0) {
        return 0;
    }

    uint8_t  g[SUDOKU_CELLS];      // 工作盘面(线性),0 = 空
    uint16_t rmask[SUDOKU_N] = { 0 };  // 每行已占用数字的位掩码
    uint16_t cmask[SUDOKU_N] = { 0 };  // 每列已占用数字的位掩码
    uint16_t bmask[SUDOKU_N] = { 0 };  // 每宫已占用数字的位掩码(宫号 = (r/3)*3 + c/3)

    // 载入盘面并建立掩码;顺带校验输入合法性——发现越界值或重复值直接判无解,
    // 这样上层的 sudoku_count_solutions() 对冲突盘面会自然返回 0。
    for (int i = 0; i < SUDOKU_CELLS; i++) {
        uint8_t v = in[i / SUDOKU_N][i % SUDOKU_N];
        g[i] = v;
        if (v == 0U) {
            continue;
        }
        if (v > SUDOKU_N) {
            return 0;
        }
        int r = i / SUDOKU_N;
        int c = i % SUDOKU_N;
        int b = (r / 3) * 3 + c / 3;
        uint16_t bit = (uint16_t)(1U << v);
        if (((rmask[r] | cmask[c] | bmask[b]) & bit) != 0U) {
            return 0;  // 题面自身冲突
        }
        rmask[r] |= bit;
        cmask[c] |= bit;
        bmask[b] |= bit;
    }

    uint8_t pos[SUDOKU_CELLS];    // 待填空格的线性下标;pos[0..level-1] 已定,pos[level..n-1] 待定
    uint8_t tried[SUDOKU_CELLS];  // 每层当前落下的值,0 = 本层尚未落子
    int n = 0;
    for (int i = 0; i < SUDOKU_CELLS; i++) {
        if (g[i] == 0U) {
            pos[n++] = (uint8_t)i;
        }
    }

    int count = 0;
    int level = 0;
    if (n > 0) {
        tried[0] = 0U;
    }

    while (level >= 0) {
        if (level >= n) {
            // 所有空格都填满 —— 得到一个解。
            count++;
            if (count == 1 && out != NULL) {
                for (int i = 0; i < SUDOKU_CELLS; i++) {
                    out[i / SUDOKU_N][i % SUDOKU_N] = g[i];
                }
            }
            if (count >= limit) {
                break;  // 达到计数上限,不必继续搜索
            }
            level--;    // 回到最后一层,撤销它并尝试下一个值,继续找别的解
            continue;
        }

        uint8_t v = tried[level];
        if (v == 0U) {
            // 新进入的一层:用 MRV 选出候选最少的格子换到 pos[level]。
            int best = level;
            int best_cnt = SUDOKU_N + 1;
            for (int k = level; k < n; k++) {
                int idx = pos[k];
                int r = idx / SUDOKU_N;
                int c = idx % SUDOKU_N;
                int b = (r / 3) * 3 + c / 3;
                int cnt = candidate_count((uint16_t)(rmask[r] | cmask[c] | bmask[b]));
                if (cnt < best_cnt) {
                    best_cnt = cnt;
                    best = k;
                    if (cnt <= 1) {
                        break;  // 候选 <=1 已经是最优,不必再扫
                    }
                }
            }
            uint8_t swap_tmp = pos[level];
            pos[level] = pos[best];
            pos[best] = swap_tmp;
            // 交换只改变 pos[level..n-1] 的排列、不改变集合,
            // 因此回溯时无需还原顺序——上层看到的仍是同一批待填格。
            if (best_cnt == 0) {
                tried[level] = 0U;
                level--;   // 有空格无候选值,本分支死路
                continue;
            }
        } else {
            // 从下一层回溯回来:先撤销本层已落的值,再尝试更大的值。
            int idx = pos[level];
            int r = idx / SUDOKU_N;
            int c = idx % SUDOKU_N;
            int b = (r / 3) * 3 + c / 3;
            uint16_t bit = (uint16_t)(1U << v);
            rmask[r] &= (uint16_t)~bit;
            cmask[c] &= (uint16_t)~bit;
            bmask[b] &= (uint16_t)~bit;
            g[idx] = 0U;
        }

        int idx = pos[level];
        int r = idx / SUDOKU_N;
        int c = idx % SUDOKU_N;
        int b = (r / 3) * 3 + c / 3;
        uint16_t used = (uint16_t)(rmask[r] | cmask[c] | bmask[b]);

        // 从 v+1 开始找下一个可用数字(v 为 0 时即从 1 开始),保证同一层不重复尝试。
        uint8_t next_v = 0U;
        for (uint8_t t = (uint8_t)(v + 1U); t <= SUDOKU_N; t++) {
            if ((used & (uint16_t)(1U << t)) == 0U) {
                next_v = t;
                break;
            }
        }

        if (next_v == 0U) {
            tried[level] = 0U;
            level--;   // 本层已试完 1..9,回溯
            continue;
        }

        tried[level] = next_v;
        g[idx] = next_v;
        uint16_t bit = (uint16_t)(1U << next_v);
        rmask[r] |= bit;
        cmask[c] |= bit;
        bmask[b] |= bit;
        level++;
        if (level < n) {
            tried[level] = 0U;  // 标记下一层为"新进入",触发 MRV 选格
        }
    }

    return count;
}

bool sudoku_solve(uint8_t grid[SUDOKU_N][SUDOKU_N])
{
    if (grid == NULL) {
        return false;
    }
    uint8_t solved[SUDOKU_N][SUDOKU_N];
    // 显式加 const 转换:C 不允许 uint8_t(*)[9] 隐式转成 const uint8_t(*)[9]
    // (嵌套指针的限定符转换),在 -Werror 下会被拒绝,故这里显式转型。
    if (solve_core((const uint8_t (*)[SUDOKU_N])grid, solved, 1) != 1) {
        return false;
    }
    memcpy(grid, solved, sizeof(solved));
    return true;
}

int sudoku_count_solutions(const uint8_t grid[SUDOKU_N][SUDOKU_N], int limit)
{
    return solve_core(grid, NULL, limit);
}

bool sudoku_grid_valid(const uint8_t grid[SUDOKU_N][SUDOKU_N])
{
    if (grid == NULL) {
        return false;
    }
    uint16_t rmask[SUDOKU_N] = { 0 };
    uint16_t cmask[SUDOKU_N] = { 0 };
    uint16_t bmask[SUDOKU_N] = { 0 };
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            uint8_t v = grid[r][c];
            if (v == 0U) {
                continue;   // 允许空格
            }
            if (v > SUDOKU_N) {
                return false;  // 越界值一律判非法
            }
            int b = (r / 3) * 3 + c / 3;
            uint16_t bit = (uint16_t)(1U << v);
            if (((rmask[r] | cmask[c] | bmask[b]) & bit) != 0U) {
                return false;
            }
            rmask[r] |= bit;
            cmask[c] |= bit;
            bmask[b] |= bit;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 出题
// ---------------------------------------------------------------------------

/**
 * 随机生成一个合法终盘(9×9 全填满且无冲突)。
 *
 * @param m     提供 RNG 的模型(rng 状态会被推进)。
 * @param full  输出终盘。
 * @return      成功返回 true;理论上不会失败,求解器异常时返回 false。
 *
 * 做法:三个对角宫(左上/中/右下)互不共享行、列、宫,可以各自独立填入 1..9 的
 * 随机排列而永远不冲突;剩余 54 格交给迭代求解器补全,必然有解。
 * 补全过程是确定性的(总取最小可用值),因此最后再对整盘做一次随机数字重标号
 * (1..9 的随机置换),把"终盘只由三个对角宫决定"的偏置进一步打散。
 */
static bool generate_full_grid(sudoku_model_t *m, uint8_t full[SUDOKU_N][SUDOKU_N])
{
    memset(full, 0, SUDOKU_N * SUDOKU_N);

    for (int b = 0; b < 3; b++) {
        uint8_t digits[SUDOKU_N] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        shuffle_u8(digits, SUDOKU_N, m);
        int base = b * 3;   // 对角宫左上角的行/列坐标相同
        for (int k = 0; k < SUDOKU_N; k++) {
            full[base + k / 3][base + k % 3] = digits[k];
        }
    }

    if (!sudoku_solve(full)) {
        return false;
    }

    // 数字重标号:perm[d-1] 是数字 d 的新名字。置换保持所有行/列/宫的约束不变。
    uint8_t perm[SUDOKU_N] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    shuffle_u8(perm, SUDOKU_N, m);
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            full[r][c] = perm[full[r][c] - 1U];
        }
    }
    return true;
}

/**
 * 从终盘挖空,生成一道保证唯一解的题面。
 *
 * @param m       提供 RNG 的模型。
 * @param full    已生成的终盘(只读)。
 * @param puzzle  输出题面,0 表示空格。
 * @param target  目标给定数;挖到这个数量就提前停止。
 * @return        最终的给定数(题面非空格数量)。
 *
 * 每挖掉一格就用 sudoku_count_solutions(limit=2) 验一次唯一性,不唯一则立刻填回。
 * 按随机顺序遍历全部 81 格,遍历完或达到 target 即停止,因此最坏情况恰好做 81 次
 * 唯一性检查——这是整个 new_game 的耗时主项,也是必须用 MRV 剪枝的原因。
 */
static int dig_holes(sudoku_model_t *m,
                     const uint8_t full[SUDOKU_N][SUDOKU_N],
                     uint8_t puzzle[SUDOKU_N][SUDOKU_N],
                     int target)
{
    memcpy(puzzle, full, SUDOKU_N * SUDOKU_N);

    uint8_t order[SUDOKU_CELLS];
    for (int i = 0; i < SUDOKU_CELLS; i++) {
        order[i] = (uint8_t)i;
    }
    shuffle_u8(order, SUDOKU_CELLS, m);

    int given = SUDOKU_CELLS;
    for (int k = 0; k < SUDOKU_CELLS && given > target; k++) {
        int idx = order[k];
        int r = idx / SUDOKU_N;
        int c = idx % SUDOKU_N;
        uint8_t backup = puzzle[r][c];
        if (backup == 0U) {
            continue;   // 已经挖过(理论上不会,order 无重复)
        }
        puzzle[r][c] = 0U;
        if (sudoku_count_solutions((const uint8_t (*)[SUDOKU_N])puzzle, 2) == 1) {
            given--;
        } else {
            puzzle[r][c] = backup;  // 解不唯一,撤销这一挖
        }
    }
    return given;
}

bool sudoku_model_new_game(sudoku_model_t *m, sudoku_difficulty_t difficulty)
{
    if (m == NULL || (int)difficulty < 0 || (int)difficulty >= (int)SUDOKU_DIFFICULTY_COUNT) {
        return false;
    }
    int lo = GIVEN_RANGE[difficulty][0];
    int hi = GIVEN_RANGE[difficulty][1];

    uint8_t full[SUDOKU_N][SUDOKU_N];
    uint8_t puzzle[SUDOKU_N][SUDOKU_N];
    int given = 0;
    bool ok = false;

    for (int attempt = 0; attempt < SUDOKU_DIG_ATTEMPTS && !ok; attempt++) {
        if (!generate_full_grid(m, full)) {
            continue;
        }
        // 目标给定数在难度区间内随机取,让同一难度的题目稀疏程度也有变化。
        int target = lo + (int)sudoku_model_random(m, (uint32_t)(hi - lo + 1));
        given = dig_holes(m, full, puzzle, target);
        // 挖到 target 时 given 恰好等于 target(每次成功挖空减 1);
        // 若遍历完仍高于 target,只要还落在 [lo, hi] 区间内同样接受。
        ok = (given >= lo && given <= hi);
    }
    if (!ok) {
        return false;   // 保持旧盘面不变(全部中间结果都在局部变量里)
    }

    // 全部成功后再一次性提交到模型,保证失败路径不会留下半成品盘面。
    memcpy(m->given, puzzle, sizeof(m->given));
    memcpy(m->cell, puzzle, sizeof(m->cell));
    memcpy(m->solution, full, sizeof(m->solution));
    m->cursor_x = 0U;
    m->cursor_y = 0U;
    m->mode = SUDOKU_MODE_NAVIGATE;
    m->edit_value = 0U;
    m->empty_count = (uint8_t)(SUDOKU_CELLS - given);
    m->state = SUDOKU_PLAYING;
    m->difficulty = difficulty;
    return true;
}

// ---------------------------------------------------------------------------
// 模型状态与交互
// ---------------------------------------------------------------------------

void sudoku_model_init(sudoku_model_t *m, uint32_t seed)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->rng = seed ? seed : SUDOKU_RNG_FALLBACK;
    m->mode = SUDOKU_MODE_NAVIGATE;
    m->state = SUDOKU_PLAYING;
    m->difficulty = SUDOKU_EASY;
    // cell 全 0 ⇒ 81 个空格,保持 empty_count 与盘面一致。
    m->empty_count = (uint8_t)SUDOKU_CELLS;
}

/**
 * 重新统计当前盘面的空格数并写回 m->empty_count。
 * 每次提交/清空后调用,避免增量维护出错(81 格全扫的开销可以忽略)。
 */
static void recount_empty(sudoku_model_t *m)
{
    uint8_t empty = 0U;
    for (int r = 0; r < SUDOKU_N; r++) {
        for (int c = 0; c < SUDOKU_N; c++) {
            if (m->cell[r][c] == 0U) {
                empty++;
            }
        }
    }
    m->empty_count = empty;
}

/**
 * 判断当前盘面是否已经完全等于唯一解。
 * 只在盘面填满(empty_count == 0)后调用。
 */
static bool board_matches_solution(const sudoku_model_t *m)
{
    return memcmp(m->cell, m->solution, sizeof(m->cell)) == 0;
}

sudoku_event_t sudoku_model_move(sudoku_model_t *m, int delta)
{
    if (m == NULL) {
        return SUDOKU_EVENT_NONE;
    }
    // 按头文件的交互模型,move 是 NAVIGATE 模式的动作;EDIT 模式下同样的按键映射到
    // cycle,若仍调用 move 则视为非法操作。已解出(SOLVED)后仍允许移动光标看盘面。
    if (m->mode != SUDOKU_MODE_NAVIGATE) {
        return SUDOKU_EVENT_BLOCKED;
    }

    int old_index = (int)m->cursor_y * SUDOKU_N + (int)m->cursor_x;
    // 行优先线性移动 + 回绕。delta 可为任意整数(含负数、绝对值大于 81),
    // 先取模再补正,保证结果落在 [0, 80]。
    int step = delta % SUDOKU_CELLS;
    int new_index = ((old_index + step) % SUDOKU_CELLS + SUDOKU_CELLS) % SUDOKU_CELLS;
    if (new_index == old_index) {
        return SUDOKU_EVENT_NONE;   // delta 为 0 或 81 的整数倍,光标没动
    }
    m->cursor_x = (uint8_t)(new_index % SUDOKU_N);
    m->cursor_y = (uint8_t)(new_index / SUDOKU_N);
    return SUDOKU_EVENT_MOVED;
}

sudoku_event_t sudoku_model_ok(sudoku_model_t *m)
{
    if (m == NULL) {
        return SUDOKU_EVENT_NONE;
    }
    if (m->state == SUDOKU_SOLVED) {
        return SUDOKU_EVENT_BLOCKED;   // 已解出,禁止一切编辑
    }

    int x = m->cursor_x;
    int y = m->cursor_y;

    if (m->mode == SUDOKU_MODE_NAVIGATE) {
        if (m->given[y][x] != 0U) {
            return SUDOKU_EVENT_BLOCKED;   // 题面给定格不可编辑
        }
        m->mode = SUDOKU_MODE_EDIT;
        m->edit_value = m->cell[y][x];     // 从当前值开始循环,方便修改已填的数
        return SUDOKU_EVENT_EDIT_BEGIN;
    }

    // EDIT 模式:提交候选值并回到 NAVIGATE。
    m->cell[y][x] = m->edit_value;
    m->edit_value = 0U;
    m->mode = SUDOKU_MODE_NAVIGATE;
    recount_empty(m);

    if (m->empty_count == 0U && board_matches_solution(m)) {
        m->state = SUDOKU_SOLVED;
        return SUDOKU_EVENT_SOLVED;
    }
    return SUDOKU_EVENT_COMMITTED;
}

sudoku_event_t sudoku_model_cycle(sudoku_model_t *m, int dir)
{
    if (m == NULL) {
        return SUDOKU_EVENT_NONE;
    }
    if (m->state == SUDOKU_SOLVED || m->mode != SUDOKU_MODE_EDIT) {
        return SUDOKU_EVENT_BLOCKED;   // 只有 EDIT 模式下才有候选值可循环
    }
    // 在 0(空)与 1..9 之间循环,共 10 个取值。dir 支持任意整数步长。
    int step = dir % (SUDOKU_N + 1);
    int next = (((int)m->edit_value + step) % (SUDOKU_N + 1) + (SUDOKU_N + 1)) % (SUDOKU_N + 1);
    if (next == (int)m->edit_value) {
        return SUDOKU_EVENT_NONE;      // dir 为 0 或 10 的整数倍,候选值没变
    }
    m->edit_value = (uint8_t)next;
    return SUDOKU_EVENT_EDIT_CHANGED;
}

sudoku_event_t sudoku_model_clear(sudoku_model_t *m)
{
    if (m == NULL) {
        return SUDOKU_EVENT_NONE;
    }
    if (m->state == SUDOKU_SOLVED) {
        return SUDOKU_EVENT_BLOCKED;
    }
    int x = m->cursor_x;
    int y = m->cursor_y;
    if (m->given[y][x] != 0U) {
        return SUDOKU_EVENT_BLOCKED;   // 给定格不能清
    }
    // 任何模式下都生效,并统一回到 NAVIGATE(EDIT 中按 clear 相当于"清空并退出编辑")。
    m->cell[y][x] = 0U;
    m->edit_value = 0U;
    m->mode = SUDOKU_MODE_NAVIGATE;
    recount_empty(m);
    return SUDOKU_EVENT_CLEARED;
}

sudoku_event_t sudoku_model_cancel(sudoku_model_t *m)
{
    if (m == NULL) {
        return SUDOKU_EVENT_NONE;
    }
    // SOLVED 之后不会再处于 EDIT 模式,这里显式挡一道,和其余编辑类操作保持一致。
    if (m->state == SUDOKU_SOLVED || m->mode != SUDOKU_MODE_EDIT) {
        return SUDOKU_EVENT_BLOCKED;
    }
    // 与 clear() 的区别:cancel 只丢弃候选值,**不动盘面**,因此 empty_count 也无需重算。
    // UI 长按跨行时先 cancel 再 move,就能在不误改当前格的前提下离开 EDIT。
    m->edit_value = 0U;
    m->mode = SUDOKU_MODE_NAVIGATE;
    return SUDOKU_EVENT_CANCELLED;
}

bool sudoku_model_is_given(const sudoku_model_t *m, int x, int y)
{
    if (m == NULL || x < 0 || x >= SUDOKU_N || y < 0 || y >= SUDOKU_N) {
        return false;
    }
    return m->given[y][x] != 0U;
}

bool sudoku_model_conflicts(const sudoku_model_t *m, int x, int y)
{
    if (m == NULL || x < 0 || x >= SUDOKU_N || y < 0 || y >= SUDOKU_N) {
        return false;
    }
    uint8_t v = m->cell[y][x];
    if (v == 0U) {
        return false;   // 空格不参与冲突判定
    }
    // 只与同行/同列/同宫的其他格比较,**不与 solution 比较**:
    // 玩家填了一个"合法但不是最终答案"的数时不应标红,那属于后续推理才能否定的猜测。
    for (int i = 0; i < SUDOKU_N; i++) {
        if (i != x && m->cell[y][i] == v) {
            return true;
        }
        if (i != y && m->cell[i][x] == v) {
            return true;
        }
    }
    int r0 = (y / 3) * 3;
    int c0 = (x / 3) * 3;
    for (int r = r0; r < r0 + 3; r++) {
        for (int c = c0; c < c0 + 3; c++) {
            if ((r != y || c != x) && m->cell[r][c] == v) {
                return true;
            }
        }
    }
    return false;
}

bool sudoku_model_cursor_at(const sudoku_model_t *m, int x, int y)
{
    if (m == NULL) {
        return false;
    }
    return (int)m->cursor_x == x && (int)m->cursor_y == y;
}
