// main/occupancy.h —— 占用/在场判定:运动分经阈值 + 去抖/滞回。纯 C,零 esp_ 依赖,
// 可主机单测(见 tests/test_occupancy.c)。设备侧由 demo_csi 周期调用。
//
// 滞回:空置态下运动分持续 > 阈值达 T1 秒 → 判为占用;占用态下持续 < 阈值达 T2 秒
// → 判为空置。T2 通常 > T1,避免短暂静止就误判离场。
#pragma once

#include <stdbool.h>

typedef struct {
    int occ;        // 当前去抖后状态:0 空置 / 1 占用
    int th;         // 运动分阈值(> th 视为"有动")
    int t1_ms;      // 进入占用所需的持续"有动"时间
    int t2_ms;      // 进入空置所需的持续"无动"时间
    int above_ms;   // 空置态下已持续"有动"的累计时间
    int below_ms;   // 占用态下已持续"无动"的累计时间
    int occ_ms;     // 当前状态已持续的时间
} occupancy_t;

// threshold 运动分阈值;t1_s / t2_s 为进入占用 / 空置所需秒数。
void occupancy_init(occupancy_t *o, int threshold, int t1_s, int t2_s);

// 运行中更新阈值(sens/occ_th 命令),不影响当前状态与计时。
void occupancy_set_threshold(occupancy_t *o, int threshold);

// 输入自上次以来经过的 dt_ms 与当前运动分,推进去抖状态机,返回 occ(0/1)。
int occupancy_update(occupancy_t *o, int motion, int dt_ms);

// 当前占用/空置状态已持续的秒数。
int occupancy_seconds(const occupancy_t *o);
