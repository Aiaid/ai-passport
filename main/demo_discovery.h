// main/demo_discovery.h —— 设备发现(RADAR)模式:被动嗅探所在空口,粗粒度
// 发现并分类周边设备(诊断/环境扫描,只收不发)。实现 demo.h 的三件套。
#pragma once

#include "bsp_button.h"

void demo_discovery_enter(void);
void demo_discovery_exit(void);
void demo_discovery_key(bsp_btn_t btn, bsp_btn_ev_t ev);
