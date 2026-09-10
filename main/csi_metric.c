// main/csi_metric.c —— 见 csi_metric.h。纯 C,只用 <math.h>/<stddef.h>。
#include "csi_metric.h"

#include <math.h>
#include <stddef.h>

int csi_metric_subcarriers(int len)
{
    if (len < 0) return 0;
    return len / 2;
}

int csi_metric_amplitudes(const int8_t *buf, int len, float *amp_out, int max_out)
{
    if (buf == NULL || amp_out == NULL || len < 2 || max_out <= 0) {
        return 0;
    }
    int pairs = len / 2;
    int count = pairs < max_out ? pairs : max_out;
    for (int i = 0; i < count; i++) {
        // 交织顺序为 (imag, real);幅度与先后无关,取平方和开方。
        float imag = (float)buf[2 * i];
        float real = (float)buf[2 * i + 1];
        amp_out[i] = sqrtf(real * real + imag * imag);
    }
    return count;
}

void csi_motion_init(csi_motion_t *m, float alpha, float scale)
{
    if (m == NULL) return;
    for (int i = 0; i < CSI_METRIC_MAX_SUBCARRIERS; i++) {
        m->ewma[i] = 0.0f;
    }
    m->n = 0;
    m->alpha = alpha;
    m->scale = scale;
    m->primed = 0;
}

void csi_motion_set_params(csi_motion_t *m, float alpha, float scale)
{
    if (m == NULL) return;
    if (alpha >= 0.0f) m->alpha = alpha;
    if (scale > 0.0f)  m->scale = scale;
}

int csi_motion_update(csi_motion_t *m, const float *amp, int n)
{
    if (m == NULL || amp == NULL || n <= 0) {
        return 0;
    }
    if (n > CSI_METRIC_MAX_SUBCARRIERS) {
        n = CSI_METRIC_MAX_SUBCARRIERS;
    }

    // 首帧(或子载波数变化)时用当前帧播种基线,本帧不计运动。
    if (!m->primed || n != m->n) {
        for (int i = 0; i < n; i++) {
            m->ewma[i] = amp[i];
        }
        m->n = n;
        m->primed = 1;
        return 0;
    }

    float sum_dev = 0.0f;
    for (int i = 0; i < n; i++) {
        float dev = fabsf(amp[i] - m->ewma[i]);
        sum_dev += dev;
        m->ewma[i] = m->alpha * amp[i] + (1.0f - m->alpha) * m->ewma[i];
    }

    float mad = sum_dev / (float)n;  // 平均绝对偏差
    float score = 0.0f;
    if (m->scale > 0.0f) {
        score = mad / m->scale * 100.0f;
    }
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    return (int)(score + 0.5f);
}
