// tests/test_occupancy.c —— 占用去抖/滞回的主机侧单元测试。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_occupancy.c main/occupancy.c -o /tmp/test_occ && /tmp/test_occ

#include <assert.h>
#include <stdio.h>

#include "occupancy.h"

// 喂 n 次(每次 dt_ms、运动分 motion)。
static void feed(occupancy_t *o, int motion, int dt_ms, int n)
{
    for (int i = 0; i < n; i++) occupancy_update(o, motion, dt_ms);
}

// 进入占用需持续"有动"达 T1;进入空置需持续"无动"达 T2。
static void test_debounce_flip(void)
{
    occupancy_t o;
    occupancy_init(&o, 30, 3, 10);  // th=30, T1=3s, T2=10s
    assert(o.occ == 0);

    // 高于阈值但不足 T1:还不占用。
    assert(occupancy_update(&o, 50, 1000) == 0);
    assert(occupancy_update(&o, 50, 1000) == 0);
    // 第 3 秒达到 T1 → 翻转为占用。
    assert(occupancy_update(&o, 50, 1000) == 1);

    // 占用态下短暂静止(5s < T2)不应离场。
    feed(&o, 0, 1000, 5);
    assert(o.occ == 1);
    // 又有动 → below 计时清零(滞回)。
    assert(occupancy_update(&o, 60, 1000) == 1);

    // 持续静止满 T2(10s)→ 翻转为空置。
    feed(&o, 0, 1000, 9);
    assert(o.occ == 1);            // 第 9 秒还不够
    assert(occupancy_update(&o, 0, 1000) == 0);  // 第 10 秒离场
}

// occ_s:当前状态已持续秒数,翻转时归零。
static void test_seconds(void)
{
    occupancy_t o;
    occupancy_init(&o, 30, 2, 5);
    // 翻转到占用(2 次 1s)。
    occupancy_update(&o, 80, 1000);
    occupancy_update(&o, 80, 1000);  // 此次翻转,occ_ms 归零
    assert(o.occ == 1);
    assert(occupancy_seconds(&o) == 0);
    feed(&o, 80, 1000, 4);
    assert(occupancy_seconds(&o) == 4);
}

// 阈值边界:motion == th 不算"有动"(契约为 > th)。
static void test_threshold_boundary(void)
{
    occupancy_t o;
    occupancy_init(&o, 30, 1, 1);
    // 正好等于阈值,持续也不占用。
    feed(&o, 30, 1000, 5);
    assert(o.occ == 0);
    // 超过阈值 1s → 占用。
    assert(occupancy_update(&o, 31, 1000) == 1);
}

// 运行中改阈值生效。
static void test_set_threshold(void)
{
    occupancy_t o;
    occupancy_init(&o, 30, 1, 1);
    occupancy_set_threshold(&o, 70);
    // 50 低于新阈值 70 → 不占用。
    feed(&o, 50, 1000, 5);
    assert(o.occ == 0);
    // 80 超过 → 占用。
    assert(occupancy_update(&o, 80, 1000) == 1);
}

int main(void)
{
    test_debounce_flip();
    test_seconds();
    test_threshold_boundary();
    test_set_threshold();
    printf("test_occupancy: all assertions passed\n");
    return 0;
}
