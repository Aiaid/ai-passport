// main/demo_csi.h —— 被动 WiFi CSI 采集模式(在场/运动检测)。
// 实现 demo.h 的 enter/exit/key 三件套,在 main.c 的 DEMOS[] 注册。
#pragma once

#include "bsp_button.h"

void demo_csi_enter(void);
void demo_csi_exit(void);
void demo_csi_key(bsp_btn_t btn, bsp_btn_ev_t ev);
