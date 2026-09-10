// main/csi_metric.h —— CSI 幅度换算与运动检测:纯 C,仅依赖标准库。
//
// 本模块刻意不 include 任何 esp_ 头,使其能在主机上独立编译并做单元测试
// (见 tests/test_csi_metric.c)。设备侧由 demo_csi.c 在 WiFi 回调里调用。
#pragma once

#include <stdint.h>

// 单条 CSI 记录最多跟踪的子载波数。HT40 最坏约 128 个复数对,够用。
#define CSI_METRIC_MAX_SUBCARRIERS 128

// CSI buf 是 len 个 int8,按 (imag, real) 对交织,故子载波数 = len/2。
// len 为负数时按 0 处理。
int csi_metric_subcarriers(int len);

// 把交织的 (imag, real) int8 对换算成每个子载波的幅度 sqrt(real^2 + imag^2)。
// 最多写 max_out 个到 amp_out,返回实际写入的子载波数(= min(len/2, max_out))。
// buf 为 NULL、len<2 或 max_out<=0 时返回 0。
int csi_metric_amplitudes(const int8_t *buf, int len, float *amp_out, int max_out);

// 运动检测状态:对每个子载波幅度做 EWMA(指数加权滑动平均),
// 运动分 = 归一化的平均绝对偏差(幅度偏离其 EWMA 基线的均值),clamp 到 0..100。
typedef struct {
    float ewma[CSI_METRIC_MAX_SUBCARRIERS];  // 每个子载波的幅度基线
    int   n;        // 当前跟踪的子载波数
    float alpha;    // EWMA 平滑系数,0..1;越大越跟随最新值
    float scale;    // 归一化分母:平均绝对偏差达到 scale 时运动分为 100
    int   primed;   // ewma 是否已用首帧播种
} csi_motion_t;

// 初始化。alpha 建议 0.1~0.5,scale 为"满量程对应的平均绝对偏差"(正数)。
void csi_motion_init(csi_motion_t *m, float alpha, float scale);

// 输入一帧 n 个子载波的幅度,更新 EWMA 并返回运动分(0..100 整数)。
// 首帧只播种基线、返回 0。n 会被 clamp 到 CSI_METRIC_MAX_SUBCARRIERS;
// n<=0 返回 0。
int csi_motion_update(csi_motion_t *m, const float *amp, int n);
