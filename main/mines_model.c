// main/mines_model.c —— 扫雷纯逻辑模型实现。
//
// 设计要点(与目标硬件相关):
//  - 只依赖 C11 标准库(stdint/stdbool/string),不引用 ESP-IDF / LVGL,可在主机上直接编译测试。
//  - 连锁展开(flood fill)一律使用固定大小的显式队列迭代实现,**不使用递归**:
//    ESP32-C3 上 UI/游戏任务栈通常只有几 KB,9×9 棋盘最坏情况递归深度可达 81 层,
//    每层栈帧叠加后有溢出风险;显式队列最多 81 字节,栈占用可静态估算。
//  - 棋盘内部用一维线性索引 idx = y * MINES_W + x(行优先)流转,便于队列存储与光标回绕计算。

#include "mines_model.h"

#include <string.h>

// 棋盘总格数(9×9 = 81)。同时是线性索引的模数与展开队列的容量上限。
#define MINES_CELL_TOTAL (MINES_W * MINES_H)
// 需要翻开的非雷格总数;revealed_count 达到该值即判定胜利。
#define MINES_SAFE_TOTAL (MINES_CELL_TOTAL - MINES_COUNT)
// rng 为 0 时的替代种子:xorshift32 的状态一旦为 0 会永远输出 0,必须避免。
// 取值为常见的非零魔数(来源:Mulberry32 常量;任意非零值均可)。
#define MINES_RNG_FALLBACK 0x6D2B79F5U

// 8 邻域偏移表(dx, dy),按行优先的扫描顺序排列(左上 → 右下)。
// 用于计算 adjacent 数、统计旗数、和弦翻开与连锁展开,四处共用避免重复写循环。
static const int8_t NEIGHBOR_DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
static const int8_t NEIGHBOR_DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

/**
 * 判断坐标是否落在棋盘内。
 *
 * @param x 列坐标,可为任意整数(允许越界值,由本函数过滤)。
 * @param y 行坐标,同上。
 * @return  落在 [0, MINES_W) × [0, MINES_H) 内返回 true。
 * @note    纯函数,无副作用。
 */
static bool in_board(int x, int y) {
    return x >= 0 && x < MINES_W && y >= 0 && y < MINES_H;
}

/**
 * 统计每格 8 邻域内的雷数,写入该格低 4 位(MINES_CELL_ADJ_MASK)。
 * 仅在布雷完成后调用一次,遍历全盘。
 *
 * @param m 模型指针,非空;要求雷位(MINES_CELL_MINE)已全部就位。
 * @note   雷格自身的低 4 位也会被赋值(其邻域雷数),UI 不应显示它,保留仅为便于调试。
 */
static void compute_adjacent(mines_model_t *m) {
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            uint8_t count = 0;
            for (int i = 0; i < 8; i++) {
                int nx = x + NEIGHBOR_DX[i];
                int ny = y + NEIGHBOR_DY[i];
                if (!in_board(nx, ny)) continue;
                if (m->cell[ny][nx] & MINES_CELL_MINE) count++;
            }
            // 低 4 位存 0..8:先清空再写入,避免与已有状态位相互干扰。
            m->cell[y][x] = (uint8_t)((m->cell[y][x] & ~(uint8_t)MINES_CELL_ADJ_MASK) | count);
        }
    }
}

/**
 * 首次翻开时布雷:排除 (safe_x, safe_y) 及其 8 邻格(3×3 安全区)后,
 * 随机选取 MINES_COUNT 个格子放雷,并计算全盘 adjacent 数。
 *
 * 算法:先把所有"可放雷"的线性索引收集成候选数组,再做部分 Fisher-Yates 洗牌取前
 * MINES_COUNT 个。相比"随机取点 + 冲突重试",该做法执行步数有确定上界(不会偶发地
 * 长时间循环),更适合放在固件的 UI 任务里执行。
 *
 * @param m      模型指针,非空;调用后 m->mines_placed 置 true。
 * @param safe_x 首次翻开的列坐标,必须在盘内。
 * @param safe_y 首次翻开的行坐标,必须在盘内。
 * @note 候选数量最少为 81 - 9 = 72(光标在盘内部时),恒大于 MINES_COUNT,故必定布雷成功。
 * @note 会推进 rng 状态;布雷结果由调用前的 m->rng 唯一确定(同种子同操作序列可复现)。
 * @note 栈占用:候选数组 81 字节。
 */
static void place_mines(mines_model_t *m, int safe_x, int safe_y) {
    uint8_t candidate[MINES_CELL_TOTAL];  // 可放雷的线性索引集合,最多 81 项。
    uint8_t candidate_count = 0;

    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            // 3×3 安全区:切比雪夫距离 ≤ 1 的格子都不放雷,保证首翻必是 0 格并触发连锁展开。
            int dx = x - safe_x;
            int dy = y - safe_y;
            if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) continue;
            candidate[candidate_count++] = (uint8_t)(y * MINES_W + x);
        }
    }

    // 部分 Fisher-Yates:第 i 轮从 [i, candidate_count) 中随机挑一个换到位置 i,再放雷。
    for (uint8_t i = 0; i < MINES_COUNT && i < candidate_count; i++) {
        uint32_t span = (uint32_t)(candidate_count - i);
        uint8_t pick = (uint8_t)(i + mines_model_random(m, span));
        uint8_t tmp = candidate[i];
        candidate[i] = candidate[pick];
        candidate[pick] = tmp;

        uint8_t idx = candidate[i];
        m->cell[idx / MINES_W][idx % MINES_W] |= MINES_CELL_MINE;
    }

    compute_adjacent(m);
    m->mines_placed = true;
}

/**
 * 统计某格 8 邻域内已插旗的格子数,用于和弦(chord)翻开的判定。
 *
 * @param m 模型指针,非空。
 * @param x 中心格列坐标,必须在盘内。
 * @param y 中心格行坐标,必须在盘内。
 * @return  0..8。
 */
static uint8_t count_flags_around(const mines_model_t *m, int x, int y) {
    uint8_t count = 0;
    for (int i = 0; i < 8; i++) {
        int nx = x + NEIGHBOR_DX[i];
        int ny = y + NEIGHBOR_DY[i];
        if (!in_board(nx, ny)) continue;
        if (m->cell[ny][nx] & MINES_CELL_FLAG) count++;
    }
    return count;
}

/**
 * 从 (start_x, start_y) 开始的迭代式连锁展开(flood fill)。
 *
 * 关键实现细节:格子在**入队时**就立刻置 REVEALED 并计数,而不是等到出队才处理。
 * 这样每个格子最多入队一次,队列容量 81 即为硬上界,既不会溢出,也不需要额外的 visited 位图。
 *
 * @param m       模型指针,非空。
 * @param start_x 起点列坐标,必须在盘内,且要求该格未插旗、未翻开、不是雷。
 * @param start_y 起点行坐标,同上。
 * @return        本次新翻开的格子数(≥ 1)。
 * @note 只展开非雷格:起点是 0 格时向 8 邻域扩散,数字格则只翻开自身(数字格是展开边界)。
 *       插旗格视为屏障,不会被翻开(与常见扫雷实现一致)。
 * @note 栈占用:队列 81 字节 + 少量局部变量;函数不递归、不调用自身。
 */
static int flood_reveal(mines_model_t *m, int start_x, int start_y) {
    uint8_t queue[MINES_CELL_TOTAL];  // 待扩散的线性索引队列(FIFO),容量即全盘格数。
    uint8_t head = 0;                 // 队首下标(下一个出队位置)。
    uint8_t tail = 0;                 // 队尾下标(下一个入队位置),同时等于已入队总数。
    int revealed = 0;

    m->cell[start_y][start_x] |= MINES_CELL_REVEALED;
    m->revealed_count++;
    revealed++;
    queue[tail++] = (uint8_t)(start_y * MINES_W + start_x);

    while (head < tail) {
        uint8_t idx = queue[head++];
        int x = idx % MINES_W;
        int y = idx / MINES_W;
        // 只有"周围 0 雷"的格子才继续向外扩散;数字格到此为止。
        if ((m->cell[y][x] & MINES_CELL_ADJ_MASK) != 0) continue;

        for (int i = 0; i < 8; i++) {
            int nx = x + NEIGHBOR_DX[i];
            int ny = y + NEIGHBOR_DY[i];
            if (!in_board(nx, ny)) continue;
            uint8_t neighbor = m->cell[ny][nx];
            if (neighbor & (MINES_CELL_REVEALED | MINES_CELL_FLAG)) continue;
            // 0 格的邻居不可能是雷(否则它的 adjacent 就不为 0),因此这里无需判雷。
            m->cell[ny][nx] = neighbor | MINES_CELL_REVEALED;
            m->revealed_count++;
            revealed++;
            queue[tail++] = (uint8_t)(ny * MINES_W + nx);
        }
    }
    return revealed;
}

/**
 * 踩雷收尾:标记踩中的那颗雷、翻开全部雷、状态转 LOST。
 *
 * @param m 模型指针,非空。
 * @param x 踩中的雷所在列坐标,必须在盘内且该格确为雷。
 * @param y 踩中的雷所在行坐标,同上。
 * @return  恒为 MINES_EVENT_LOST,方便调用处直接 return。
 * @note 已插旗的雷保留 FLAG 位并同时置 REVEALED,UI 可据此区分"标对的雷"与"漏掉的雷"。
 * @note 雷格不计入 revealed_count(该字段语义是"已翻开的非雷格数")。
 */
static mines_event_t trigger_boom(mines_model_t *m, int x, int y) {
    m->cell[y][x] |= (uint8_t)(MINES_CELL_BOOM | MINES_CELL_REVEALED);
    for (int by = 0; by < MINES_H; by++) {
        for (int bx = 0; bx < MINES_W; bx++) {
            if (m->cell[by][bx] & MINES_CELL_MINE) m->cell[by][bx] |= MINES_CELL_REVEALED;
        }
    }
    m->state = MINES_LOST;
    return MINES_EVENT_LOST;
}

/**
 * 胜负判定:所有非雷格都已翻开即胜利。
 *
 * @param m 模型指针,非空。
 * @return  胜利时返回 true,并且已把 state 置为 MINES_WON。
 * @note 胜利时把尚未插旗的雷自动插旗(flags_used 同步累加),UI 无需自己补这一步。
 *       此时 flags_used 必定等于 MINES_COUNT:插旗格无法被翻开,而胜利要求全部非雷格已翻开,
 *       故局面中不可能存在插错位置的旗,remaining 归零。
 */
static bool check_win(mines_model_t *m) {
    if (m->revealed_count < MINES_SAFE_TOTAL) return false;
    for (int y = 0; y < MINES_H; y++) {
        for (int x = 0; x < MINES_W; x++) {
            if (!(m->cell[y][x] & MINES_CELL_MINE)) continue;
            if (m->cell[y][x] & MINES_CELL_FLAG) continue;
            m->cell[y][x] |= MINES_CELL_FLAG;
            m->flags_used++;
        }
    }
    m->state = MINES_WON;
    return true;
}

void mines_model_init(mines_model_t *m, uint32_t seed) {
    memset(m, 0, sizeof(*m));
    // seed 为 0 会让 xorshift32 永远卡在 0,替换成固定非零常量。
    m->rng = seed ? seed : MINES_RNG_FALLBACK;
    mines_model_reset(m);
}

void mines_model_reset(mines_model_t *m) {
    // 保留 rng 序列(不重置回种子),连续开局才不会反复出现同一盘。
    uint32_t seed = m->rng ? m->rng : MINES_RNG_FALLBACK;
    memset(m, 0, sizeof(*m));
    m->rng = seed;
    m->cursor_x = MINES_W / 2;  // 光标归中 (4, 4),即 9×9 的正中心。
    m->cursor_y = MINES_H / 2;
    m->mines_placed = false;    // 首次翻开时才布雷(以下三项 memset 已清零,显式写出以明确语义)。
    m->flags_used = 0;
    m->revealed_count = 0;
    m->state = MINES_PLAYING;
}

mines_event_t mines_model_move(mines_model_t *m, int delta) {
    // 先对 delta 取模再相加,保证任意 int 输入(含 INT_MAX / INT_MIN)都不会溢出:
    // index ∈ [0, 80],step ∈ (-81, 81),两者之和落在 int 的安全范围内。
    int index = m->cursor_y * MINES_W + m->cursor_x;
    int step = delta % MINES_CELL_TOTAL;
    int next = (index + step) % MINES_CELL_TOTAL;
    if (next < 0) next += MINES_CELL_TOTAL;  // C 对负数取模结果为负,手工回绕到 [0, 81)。
    m->cursor_x = (uint8_t)(next % MINES_W);
    m->cursor_y = (uint8_t)(next / MINES_W);
    return MINES_EVENT_MOVED;  // 任何状态(含 WON/LOST)都允许移动,便于结束后浏览棋盘。
}

mines_event_t mines_model_reveal(mines_model_t *m) {
    if (m->state != MINES_PLAYING) return MINES_EVENT_BLOCKED;  // 已结束:只剩移动光标可用。

    int x = m->cursor_x;
    int y = m->cursor_y;
    uint8_t cell = m->cell[y][x];

    if (cell & MINES_CELL_FLAG) return MINES_EVENT_BLOCKED;  // 插旗格受保护,须先拔旗才能翻开。

    if (cell & MINES_CELL_REVEALED) {
        // 已翻开的格子:仅当它是数字格、且周围旗数恰好等于该数字时,执行"和弦"批量翻开。
        uint8_t adj = cell & MINES_CELL_ADJ_MASK;
        if (adj == 0) return MINES_EVENT_BLOCKED;  // 0 格的邻居早已展开,无事可做。
        if (count_flags_around(m, x, y) != adj) {
            return MINES_EVENT_BLOCKED;            // 旗数与数字不符,拒绝执行(防误触炸盘)。
        }

        int revealed = 0;
        for (int i = 0; i < 8; i++) {
            int nx = x + NEIGHBOR_DX[i];
            int ny = y + NEIGHBOR_DY[i];
            if (!in_board(nx, ny)) continue;
            uint8_t neighbor = m->cell[ny][nx];
            if (neighbor & (MINES_CELL_REVEALED | MINES_CELL_FLAG)) continue;
            // 旗插错位置时,和弦会翻到真雷 —— 这是玩家自己的判断失误,按踩雷处理。
            if (neighbor & MINES_CELL_MINE) return trigger_boom(m, nx, ny);
            revealed += flood_reveal(m, nx, ny);
        }
        if (revealed == 0) return MINES_EVENT_BLOCKED;  // 邻格全已翻开/插旗,视为无效操作。
        if (check_win(m)) return MINES_EVENT_WON;
        return MINES_EVENT_REVEALED;
    }

    // 未翻开且未插旗:首次翻开时才布雷,保证第一手既不踩雷、周围也无雷。
    if (!m->mines_placed) place_mines(m, x, y);

    if (m->cell[y][x] & MINES_CELL_MINE) return trigger_boom(m, x, y);

    flood_reveal(m, x, y);
    if (check_win(m)) return MINES_EVENT_WON;
    return MINES_EVENT_REVEALED;
}

mines_event_t mines_model_toggle_flag(mines_model_t *m) {
    if (m->state != MINES_PLAYING) return MINES_EVENT_BLOCKED;

    uint8_t *cell = &m->cell[m->cursor_y][m->cursor_x];
    if (*cell & MINES_CELL_REVEALED) return MINES_EVENT_BLOCKED;  // 已翻开的格子不能插旗。

    if (*cell & MINES_CELL_FLAG) {
        *cell &= (uint8_t)~MINES_CELL_FLAG;
        if (m->flags_used > 0) m->flags_used--;  // 防御性下溢保护:计数与旗位理论上恒一致。
    } else {
        // 不限制旗数上限:插旗数可以超过 MINES_COUNT,此时 remaining 为负,交由 UI 呈现。
        *cell |= MINES_CELL_FLAG;
        m->flags_used++;
    }
    return MINES_EVENT_FLAGGED;
}

int mines_model_remaining(const mines_model_t *m) {
    return MINES_COUNT - (int)m->flags_used;
}

bool mines_model_cursor_at(const mines_model_t *m, int x, int y) {
    return x == (int)m->cursor_x && y == (int)m->cursor_y;
}

uint32_t mines_model_random(mines_model_t *m, uint32_t limit) {
    // xorshift32(Marsaglia 2003,移位参数 13/17/5):周期 2^32-1,状态恒不为 0,
    // 无乘除、无查表,适合 MCU;不用于任何安全用途。
    uint32_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x ? x : MINES_RNG_FALLBACK;
    if (limit == 0) return 0;
    // 直接取模存在极轻微的模偏差,对 9×9 扫雷的手感影响可忽略,换取实现简单与固定耗时。
    return m->rng % limit;
}
