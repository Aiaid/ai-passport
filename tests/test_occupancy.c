// tests/test_occupancy.c —— 占用去抖 + 中值抗尖峰的主机侧单元测试。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_occupancy.c main/occupancy.c -o /tmp/test_occ && /tmp/test_occ

#include <assert.h>
#include <stdio.h>

#include "occupancy.h"

#define DT 300  // 与设备端 ECHO 刷新周期一致(~3.3Hz)

static void feed(occupancy_t *o, int motion, int n)
{
    for (int i = 0; i < n; i++) occupancy_update(o, motion, DT);
}

// 持续"有动"会进入占用;持续"无动"会回到空置。
static void test_basic_flip(void)
{
    occupancy_t o;
    occupancy_init(&o, 25, 2, 6);  // th=25,T1=2s,T2=6s
    assert(o.occ == 0);

    feed(&o, 40, 40);              // 足够长的"有动"
    assert(o.occ == 1);

    feed(&o, 16, 60);              // 足够长的"无动"
    assert(o.occ == 0);
}

// 关键修正:空置过程中的偶发尖峰不得打断"变空"(旧逻辑会卡在 OCCUPIED)。
static void test_spikes_do_not_block_empty(void)
{
    occupancy_t o;
    occupancy_init(&o, 25, 2, 6);

    feed(&o, 40, 40);
    assert(o.occ == 1);

    // 模拟真机空场:多数样本低(~16),每 4 个插一个尖峰(~95)。
    // 25% 尖峰 < 50%,窗口中值仍 ~16 < 阈值 → 应能判为 EMPTY。
    for (int i = 0; i < 60; i++) {
        int m = (i % 4 == 3) ? 95 : 16;
        occupancy_update(&o, m, DT);
    }
    assert(o.occ == 0);            // 真机复测"放下走开应变 EMPTY"
    assert(occupancy_level(&o) < 25);  // 中值被尖峰拉不动
}

// 空置态下单个尖峰不应误判为占用(中值不过阈值)。
static void test_single_spike_no_false_occupied(void)
{
    occupancy_t o;
    occupancy_init(&o, 25, 2, 6);
    feed(&o, 16, 20);              // 稳定空场
    occupancy_update(&o, 100, DT); // 一个尖峰
    assert(o.occ == 0);
    feed(&o, 16, 5);
    assert(o.occ == 0);
}

// occ_s:进入状态后按时间累计。
static void test_seconds(void)
{
    occupancy_t o;
    occupancy_init(&o, 25, 2, 6);
    feed(&o, 40, 40);
    assert(o.occ == 1);
    int s0 = occupancy_seconds(&o);
    feed(&o, 40, 10);              // 再 +3s
    assert(occupancy_seconds(&o) >= s0 + 2);
}

// 运行中改阈值生效(sens/occ_th 命令)。
static void test_set_threshold(void)
{
    occupancy_t o;
    occupancy_init(&o, 25, 2, 6);
    occupancy_set_threshold(&o, 70);
    feed(&o, 40, 40);              // 40 < 新阈值 70 → 不占用
    assert(o.occ == 0);
    feed(&o, 90, 40);             // 90 > 70 → 占用
    assert(o.occ == 1);
}

// 自标定:稳健统计,尖峰不抬高阈值。
static void test_calibrate(void)
{
    // 恒定空场(全 17):MAD=0 → 阈值 = 17 + margin(6) = 23。
    int flat[20];
    for (int i = 0; i < 20; i++) flat[i] = 17;
    assert(occupancy_calibrate_threshold(flat, 20) == 23);

    // 空场 16-18 + 每 4 个一个尖峰 95:中值仍 ~17,MAD 小,阈值落在 ~22..30
    // 之间(明显低于"人在"中值 32),尖峰没把阈值顶上去。
    int noisy[40];
    for (int i = 0; i < 40; i++) noisy[i] = (i % 4 == 3) ? 95 : (16 + i % 3);
    int th = occupancy_calibrate_threshold(noisy, 40);
    assert(th >= 20 && th <= 31);

    // clamp:全高 → 上限 80;全 0 → 下限 10。
    int hi[10]; for (int i = 0; i < 10; i++) hi[i] = 200;
    assert(occupancy_calibrate_threshold(hi, 10) == 80);
    int zero[10] = { 0 };
    assert(occupancy_calibrate_threshold(zero, 10) == 10);

    // 退化输入。
    assert(occupancy_calibrate_threshold(NULL, 0) == 25);
}

int main(void)
{
    test_basic_flip();
    test_spikes_do_not_block_empty();
    test_single_spike_no_false_occupied();
    test_seconds();
    test_set_threshold();
    test_calibrate();
    printf("test_occupancy: all assertions passed\n");
    return 0;
}
