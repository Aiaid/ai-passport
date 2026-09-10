// main/occupancy.c —— 见 occupancy.h。纯 C,只用标准库。
#include "occupancy.h"

#include <stddef.h>

void occupancy_init(occupancy_t *o, int threshold, int t1_s, int t2_s)
{
    if (!o) return;
    o->occ = 0;
    o->th = threshold;
    o->t1_ms = t1_s * 1000;
    o->t2_ms = t2_s * 1000;
    o->above_ms = 0;
    o->below_ms = 0;
    o->occ_ms = 0;
    for (int i = 0; i < OCC_WIN; i++) o->win[i] = 0;
    o->win_n = 0;
    o->win_i = 0;
    o->med = 0;
}

void occupancy_set_threshold(occupancy_t *o, int threshold)
{
    if (o) o->th = threshold;
}

// 对已填窗口样本求中值(拷贝 + 插入排序,窗口很小)。
static int window_median(const occupancy_t *o)
{
    int n = o->win_n;
    if (n <= 0) return 0;
    int tmp[OCC_WIN];
    for (int i = 0; i < n; i++) tmp[i] = o->win[i];
    for (int i = 1; i < n; i++) {
        int key = tmp[i], j = i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    return tmp[n / 2];  // 偶数个时取偏上的中值,判定更保守(略偏"有动")
}

int occupancy_update(occupancy_t *o, int motion, int dt_ms)
{
    if (!o || dt_ms < 0) return o ? o->occ : 0;

    // 压入环形窗口。
    o->win[o->win_i] = motion;
    o->win_i = (o->win_i + 1) % OCC_WIN;
    if (o->win_n < OCC_WIN) o->win_n++;

    int med = window_median(o);
    o->med = med;
    o->occ_ms += dt_ms;

    if (o->occ) {
        // 占用态:中值持续低于阈值累计到 T2 才判空置。尖峰无法抬高中值,故不打断。
        if (med < o->th) {
            o->below_ms += dt_ms;
            if (o->below_ms >= o->t2_ms) {
                o->occ = 0;
                o->occ_ms = 0;
                o->below_ms = 0;
                o->above_ms = 0;
            }
        } else {
            o->below_ms = 0;
        }
    } else {
        // 空置态:中值持续高于阈值累计到 T1 才判占用(较灵敏)。
        if (med > o->th) {
            o->above_ms += dt_ms;
            if (o->above_ms >= o->t1_ms) {
                o->occ = 1;
                o->occ_ms = 0;
                o->above_ms = 0;
                o->below_ms = 0;
            }
        } else {
            o->above_ms = 0;
        }
    }
    return o->occ;
}

int occupancy_seconds(const occupancy_t *o)
{
    return o ? o->occ_ms / 1000 : 0;
}

int occupancy_level(const occupancy_t *o)
{
    return o ? o->med : 0;
}

// —— 自标定 ——
#define OCC_CALIB_MARGIN 6    // 阈值相对底噪中值的最小余量
#define OCC_CALIB_K      3    // MAD 的倍数(覆盖环境噪声)
#define OCC_TH_MIN       10
#define OCC_TH_MAX       80
#define OCC_CALIB_MAX    128  // 参与统计的最大样本数

static int median_of(int *a, int n)  // 原地排序取中值
{
    for (int i = 1; i < n; i++) {
        int key = a[i], j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    return a[n / 2];
}

int occupancy_calibrate_threshold(const int *samples, int n)
{
    if (!samples || n <= 0) return 25;  // 保守默认
    if (n > OCC_CALIB_MAX) n = OCC_CALIB_MAX;

    int buf[OCC_CALIB_MAX];
    for (int i = 0; i < n; i++) buf[i] = samples[i];
    int med = median_of(buf, n);

    // MAD:|x - med| 的中值(稳健离散度,尖峰不抬高)。
    int dev[OCC_CALIB_MAX];
    for (int i = 0; i < n; i++) {
        int d = samples[i] - med;
        dev[i] = d < 0 ? -d : d;
    }
    int mad = median_of(dev, n);

    int margin = OCC_CALIB_K * mad;
    if (margin < OCC_CALIB_MARGIN) margin = OCC_CALIB_MARGIN;
    int th = med + margin;
    if (th < OCC_TH_MIN) th = OCC_TH_MIN;
    if (th > OCC_TH_MAX) th = OCC_TH_MAX;
    return th;
}
