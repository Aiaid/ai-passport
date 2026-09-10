// main/csi_ble.h —— 常驻 NimBLE GATT 外设:广播 AIPassport-CSI,暴露 STATUS(通知)
// 与 CONTROL(写)。与客户端的 UUID/JSON 契约(GATT_CONTRACT_V2)对齐。
//
// 常驻:开机 init 一次后持续广播,独立于具体 demo。STATUS 从 echo_state 读;
// CONTROL 解析后:mode→请求主循环切模式,sens/occ_th→更新参数,其余→投递命令队列。
// 线程:NimBLE 回调在 host task、STATUS 通知由 esp_timer 触发,二者都不碰 LVGL。
#pragma once

#include "esp_err.h"

// 开机初始化并开始广播 + STATUS 通知定时器。幂等(重复调用仅确保广播在跑)。
esp_err_t csi_ble_init(void);
