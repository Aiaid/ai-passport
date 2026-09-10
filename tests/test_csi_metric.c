// tests/test_csi_metric.c —— csi_metric 模块的主机侧单元测试。
//
// 只依赖 C 标准库,不需要 ESP-IDF。编译运行:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_csi_metric.c main/csi_metric.c -lm -o /tmp/test_csi
//   /tmp/test_csi
// 全部断言通过时 main 返回 0。

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "csi_metric.h"

// 浮点近似相等(绝对容差)。
static int approx(float a, float b) { return fabsf(a - b) < 1e-3f; }

// ---------------------------------------------------------------------------
// 子载波数 = len/2
// ---------------------------------------------------------------------------
static void test_subcarriers(void)
{
    assert(csi_metric_subcarriers(0) == 0);
    assert(csi_metric_subcarriers(1) == 0);   // 不足一对
    assert(csi_metric_subcarriers(2) == 1);
    assert(csi_metric_subcarriers(128) == 64);
    assert(csi_metric_subcarriers(384) == 192);
    assert(csi_metric_subcarriers(-4) == 0);  // 负数按 0
}

// ---------------------------------------------------------------------------
// 幅度换算:sqrt(real^2 + imag^2),对已知 (imag, real) 对验证
// ---------------------------------------------------------------------------
static void test_amplitudes_known(void)
{
    // 四对:(3,4)->5、(0,0)->0、(-3,-4)->5、(6,8)->10
    const int8_t buf[] = { 3, 4, 0, 0, -3, -4, 6, 8 };
    float amp[4] = { -1, -1, -1, -1 };
    int n = csi_metric_amplitudes(buf, (int)sizeof(buf), amp, 4);
    assert(n == 4);
    assert(approx(amp[0], 5.0f));
    assert(approx(amp[1], 0.0f));
    assert(approx(amp[2], 5.0f));
    assert(approx(amp[3], 10.0f));
}

static void test_amplitudes_edges(void)
{
    const int8_t buf[] = { 3, 4, 6, 8, 5, 12, 9, 12 };  // 4 对
    float amp[4];

    // max_out 限制写入数:给 4 对但 max_out=2 只写 2 个
    int n = csi_metric_amplitudes(buf, (int)sizeof(buf), amp, 2);
    assert(n == 2);
    assert(approx(amp[0], 5.0f));
    assert(approx(amp[1], 10.0f));

    // 退化输入:NULL / 长度不足一对 / max_out<=0
    assert(csi_metric_amplitudes(NULL, 8, amp, 4) == 0);
    assert(csi_metric_amplitudes(buf, 1, amp, 4) == 0);
    assert(csi_metric_amplitudes(buf, 8, amp, 0) == 0);

    // 奇数长度:最后一个孤立字节被丢弃(len/2 向下取整)
    assert(csi_metric_amplitudes(buf, 7, amp, 4) == 3);
}

// ---------------------------------------------------------------------------
// 运动检测
// ---------------------------------------------------------------------------

// 静止:首帧播种返回 0,之后相同幅度运动分保持 0
static void test_motion_static(void)
{
    csi_motion_t m;
    csi_motion_init(&m, 0.5f, 10.0f);
    float amp[4] = { 10, 20, 30, 40 };

    assert(csi_motion_update(&m, amp, 4) == 0);   // 首帧播种
    assert(csi_motion_update(&m, amp, 4) == 0);   // 无变化
    assert(csi_motion_update(&m, amp, 4) == 0);
}

// 运动:注入逐步变化的幅度,运动分明显升高且非零
static void test_motion_rising(void)
{
    csi_motion_t m;
    csi_motion_init(&m, 0.5f, 10.0f);
    float base[4] = { 10, 10, 10, 10 };
    assert(csi_motion_update(&m, base, 4) == 0);  // 播种

    // 每个子载波偏离基线 1 → 平均绝对偏差 1 → 分数约 10
    float step1[4] = { 11, 11, 11, 11 };
    int s1 = csi_motion_update(&m, step1, 4);
    assert(s1 > 0);

    // 继续拉大偏差,运动分应进一步升高
    float step2[4] = { 14, 14, 14, 14 };
    int s2 = csi_motion_update(&m, step2, 4);
    assert(s2 > s1);
}

// clamp:剧烈变化时运动分封顶 100,不溢出
static void test_motion_clamp(void)
{
    csi_motion_t m;
    csi_motion_init(&m, 0.5f, 10.0f);
    float base[4] = { 0, 0, 0, 0 };
    assert(csi_motion_update(&m, base, 4) == 0);

    // 平均绝对偏差 = 100,远超 scale=10 → 封顶 100
    float huge[4] = { 100, 100, 100, 100 };
    int s = csi_motion_update(&m, huge, 4);
    assert(s == 100);
}

// 越界 n 被 clamp 到 CSI_METRIC_MAX_SUBCARRIERS,不越界写
static void test_motion_bounds(void)
{
    csi_motion_t m;
    csi_motion_init(&m, 0.3f, 5.0f);

    static float big[CSI_METRIC_MAX_SUBCARRIERS + 32];
    for (int i = 0; i < CSI_METRIC_MAX_SUBCARRIERS + 32; i++) big[i] = 1.0f;

    // 传入超量 n:首帧播种返回 0,且内部只跟踪 MAX 个
    assert(csi_motion_update(&m, big, CSI_METRIC_MAX_SUBCARRIERS + 32) == 0);
    assert(m.n == CSI_METRIC_MAX_SUBCARRIERS);

    // n<=0 返回 0
    assert(csi_motion_update(&m, big, 0) == 0);
    assert(csi_motion_update(&m, big, -5) == 0);
}

int main(void)
{
    test_subcarriers();
    test_amplitudes_known();
    test_amplitudes_edges();
    test_motion_static();
    test_motion_rising();
    test_motion_clamp();
    test_motion_bounds();
    printf("test_csi_metric: all assertions passed\n");
    return 0;
}
