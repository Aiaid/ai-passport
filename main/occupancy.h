// main/occupancy.h —— 占用/在场判定:对运动分做滑动窗口中值滤波 + 去抖/滞回。
// 纯 C,零 esp_ 依赖,可主机单测(见 tests/test_occupancy.c)。
//
// 抗尖峰:判定不看逐样本瞬时值,而看窗口中值(等价于"窗口内多数样本高/低于阈值")。
// 空场偶发的运动尖峰只要不占窗口多数,就不会改变中值、不会打断"变空"计时——这是
// 真机上"放下走开也回不去 EMPTY"的根因修正。T1(进占用)灵敏,T2(进空置)抗尖峰。
#pragma once

#include <stdbool.h>

#define OCC_WIN 12   // 中值窗口(样本)。ECHO 以 ~3.3Hz 刷新喂入,约覆盖 3.6s。

typedef struct {
    int occ;        // 去抖后状态:0 空置 / 1 占用
    int th;         // 阈值(窗口中值 > th 视为"有动")
    int t1_ms;      // 进入占用所需的持续(中值高于阈值)时间
    int t2_ms;      // 进入空置所需的持续(中值低于阈值)时间
    int above_ms;   // 空置态下中值持续高于阈值的累计时间
    int below_ms;   // 占用态下中值持续低于阈值的累计时间
    int occ_ms;     // 当前状态已持续时间
    int win[OCC_WIN];
    int win_n;      // 窗口已填样本数
    int win_i;      // 环形写指针
    int med;        // 最近一次计算的窗口中值
} occupancy_t;

void occupancy_init(occupancy_t *o, int threshold, int t1_s, int t2_s);
void occupancy_set_threshold(occupancy_t *o, int threshold);

// 输入自上次以来经过的 dt_ms 与当前运动分:压入窗口、取中值、推进去抖。返回 occ。
int occupancy_update(occupancy_t *o, int motion, int dt_ms);

int occupancy_seconds(const occupancy_t *o);  // 当前状态已持续秒数
int occupancy_level(const occupancy_t *o);    // 最近窗口中值(供显示/调试)

// 自标定:从一段"空场"运动分样本算出占用阈值。
// 用稳健统计(中值 + 中值绝对偏差 MAD)吸收空场尖峰:
//   threshold = median + max(OCC_CALIB_MARGIN, OCC_CALIB_K * MAD),clamp 到 [10,80]。
// n<=0 时返回一个保守默认值。纯函数,可 host 测试。
int occupancy_calibrate_threshold(const int *samples, int n);
