// main/demo.h —— 每个演示页实现的统一接口。
// 新增一个演示页 = 实现这三个函数 + 在 main.c 的 DEMOS[] 里加一行。
#pragma once

#include "bsp_button.h"

typedef struct {
    const char *name;
    void (*enter)(void);                          // 建自己的屏并载入
    void (*exit)(void);                           // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键(长按确定已被 main 拦截)
} demo_entry_t;

// 被动 WiFi CSI 采集(在场/运动检测)
void demo_csi_enter(void);      void demo_csi_exit(void);
void demo_csi_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 被动设备发现(RADAR:嗅探 + 粗分类)
void demo_discovery_enter(void); void demo_discovery_exit(void);
void demo_discovery_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 设置屏(语言/阈值/灵敏度/告警/重标定/ping)
void demo_settings_enter(void); void demo_settings_exit(void);
void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);
