// main/csi_proto.h —— BLE 线上协议的纯逻辑:STATUS JSON 生成 + CONTROL 命令解析。
//
// 纯 C,仅依赖标准库,不 include 任何 esp_/nimble 头,便于主机单元测试
// (见 tests/test_csi_proto.c)。设备侧由 csi_ble.c / demo_csi.c 调用。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// CONTROL 命令种类。
typedef enum {
    CSI_CMD_NONE = 0,   // 未识别/无命令
    CSI_CMD_START,      // 开始采集
    CSI_CMD_STOP,       // 停止采集
    CSI_CMD_FTM,        // 触发一次 FTM 测距
    CSI_CMD_PING_MS,    // 修改 ping 间隔,单位 ms(已 clamp 到 20..2000)
} csi_cmd_kind_t;

typedef struct {
    csi_cmd_kind_t kind;
    int            v;   // 仅 CSI_CMD_PING_MS 使用:clamp 后的间隔 ms
} csi_cmd_t;

// STATUS 通知的运行状态快照(供 csi_proto_build_status 取值)。
typedef struct {
    int  st;            // 0 off / 1 connecting / 2 running / 3 failed
    char ssid[33];      // 连接的 SSID(UTF-8,可为空)
    int  rssi;          // dBm
    int  mot;           // 运动分 0..100
    int  rate;          // CSI 包速率 pkt/s
    int  ftm;           // FTM 距离 cm(0 = 无效)
    int  fv;            // FTM 有效位 0/1
} csi_status_t;

// 生成 STATUS JSON 到 out(以 NUL 结尾)。字段顺序固定:
//   {"st":..,"ssid":"..","rssi":..,"mot":..,"rate":..,"ftm":..,"fv":..}
// ssid 做 JSON 转义(双引号、反斜杠、控制字符/非 ASCII 转 \u00XX)。
// 返回写入的字符数(不含 NUL);cap 不足以容纳完整结果时返回 -1。
int csi_proto_build_status(char *out, size_t cap, int st, const char *ssid,
                           int rssi, int mot, int rate, int ftm, int fv);

// 解析 CONTROL JSON 命令。识别 start/stop/ftm/ping_ms;ping_ms 的 v 缺省
// 按 100 处理并 clamp 到 20..2000。识别成功返回 true 并填 out;未知命令或
// 格式非法返回 false 且 out->kind = CSI_CMD_NONE。
bool csi_proto_parse_control(const char *in, csi_cmd_t *out);
