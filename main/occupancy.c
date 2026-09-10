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
}

void occupancy_set_threshold(occupancy_t *o, int threshold)
{
    if (o) o->th = threshold;
}

int occupancy_update(occupancy_t *o, int motion, int dt_ms)
{
    if (!o || dt_ms < 0) return o ? o->occ : 0;

    o->occ_ms += dt_ms;

    if (o->occ) {
        // 占用态:持续"无动"累计到 T2 才翻转为空置。
        if (motion < o->th) {
            o->below_ms += dt_ms;
            if (o->below_ms >= o->t2_ms) {
                o->occ = 0;
                o->occ_ms = 0;
                o->below_ms = 0;
                o->above_ms = 0;
            }
        } else {
            o->below_ms = 0;  // 一旦又有动,重新计时(滞回)
        }
    } else {
        // 空置态:持续"有动"累计到 T1 才翻转为占用。
        if (motion > o->th) {
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
