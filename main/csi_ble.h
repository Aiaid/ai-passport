// main/csi_ble.h —— NimBLE GATT 外设:广播 AIPassport-CSI,暴露 STATUS(通知)
// 与 CONTROL(写)两个特征。与 web/iOS 客户端的 UUID/JSON 契约对齐。
//
// 线程模型:NimBLE 回调跑在 nimble host task;STATUS 通知由内部 esp_timer 周期
// 触发。二者都不碰 LVGL。demo_csi 通过下面的回调提供状态、接收命令。
#pragma once

#include "esp_err.h"
#include "csi_proto.h"

// 由 notify 定时器与 STATUS 读回调调用,填充当前状态快照。
typedef void (*csi_ble_status_fn)(csi_status_t *out);
// CONTROL 写入并解析成功后调用(运行在 nimble host task,实现里应尽快返回)。
typedef void (*csi_ble_cmd_fn)(const csi_cmd_t *cmd);

void csi_ble_set_status_provider(csi_ble_status_fn fn);
void csi_ble_set_cmd_handler(csi_ble_cmd_fn fn);

// 启动:首次初始化 NimBLE host + 注册 GATT(host 初始化态此后常驻),开始广播;
// 再次调用仅重新开始广播。并启动 STATUS 通知定时器(约 3Hz)。
esp_err_t csi_ble_start(void);

// 停止广播与通知定时器、断开现有连接;保留 host 初始化态供下次复用。
void csi_ble_stop(void);
