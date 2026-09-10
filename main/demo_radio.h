// main/demo_radio.h —— 无线栈的共享准备与一个被动 STA 连接 helper。
//
// CSI 采集(demo_csi.c)需要作为 STA 连到用户自己的路由器,只接收、不发射干扰。
// 这里封装 NVS / netif / event-loop 的一次性准备,以及"连到指定 SSID 并等到
// 拿到 IP"的阻塞式连接流程,便于 demo 复用且生命周期清晰。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Wi-Fi 默认 STA netif 依赖 NVS;只初始化,不在失败时擦除用户数据。
esp_err_t demo_radio_nvs_prepare(void);

// 默认 netif 与全局事件循环。按应用生命周期保留(准备过即复用)。
esp_err_t demo_radio_network_prepare(void);

// 以 STA 连接指定 SSID/密码,阻塞到拿到 IP 或超时/被中止。
//
// 契约:
//   - 返回 ESP_OK 时 Wi-Fi 已完整启动并连上,调用方随后必须调用
//     demo_radio_wifi_sta_stop() 做配对清理;gw_addr_out 回填网关 IPv4
//     (esp_ip4_addr_t.addr,网络字节序)。
//   - 返回任何错误时,本函数已把它启动的 Wi-Fi/netif 全部回滚干净,
//     调用方【不要】再调 demo_radio_wifi_sta_stop()。
//   - abort_flag 可为 NULL;非 NULL 时在等待 IP 的过程中被置 true 会尽快
//     中止连接(用于 demo 退出时快速收场,不阻塞 UI 任务过久)。
//   - ssid 为空串时返回 ESP_ERR_INVALID_ARG(由调用方负责"未配置"提示)。
esp_err_t demo_radio_wifi_sta_start(const char *ssid, const char *password,
                                    int timeout_ms, volatile bool *abort_flag,
                                    uint32_t *gw_addr_out);

// 停止并释放一次成功的 demo_radio_wifi_sta_start():注销事件处理、停 Wi-Fi、
// deinit、销毁 STA netif。可安全重复调用(幂等)。
void demo_radio_wifi_sta_stop(void);
