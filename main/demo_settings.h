// main/demo_settings.h —— 设置屏:语言/占用阈值/灵敏度/告警/重新标定/Ping 间隔。
// 实现 demo.h 接口,改动实时应用并存 NVS。
#pragma once

#include "bsp_button.h"

void demo_settings_enter(void);
void demo_settings_exit(void);
void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);
