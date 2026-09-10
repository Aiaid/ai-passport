// main/csi_proto.h —— BLE 线上协议的纯逻辑:STATUS JSON 生成 + CONTROL 命令解析。
//
// 纯 C,仅依赖标准库,不 include 任何 esp_/nimble 头,便于主机单元测试
// (见 tests/test_csi_proto.c)。契约见 GATT_CONTRACT_V2。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// CONTROL 命令种类。
typedef enum {
    CSI_CMD_NONE = 0,   // 未识别/无命令
    CSI_CMD_START,      // 开始采集
    CSI_CMD_STOP,       // 停止采集
    CSI_CMD_FTM,        // 触发一次 FTM 测距
    CSI_CMD_PING_MS,    // 修改 ping 间隔 ms(clamp 20..2000)
    CSI_CMD_MODE,       // 远程切模式(mode 字段 "echo"/"radar")
    CSI_CMD_SENS,       // 调灵敏度(alpha/scale)
    CSI_CMD_OCC_TH,     // 占用阈值(v)
} csi_cmd_kind_t;

typedef struct {
    csi_cmd_kind_t kind;
    int   v;            // PING_MS 的 ms / OCC_TH 的阈值
    float alpha;        // SENS:EWMA 系数,未给为 -1
    float scale;        // SENS:归一化分母,未给为 -1
    char  mode[8];      // MODE:目标模式字符串
} csi_cmd_t;

// STATUS 通知的运行状态快照。mode 决定 JSON 形态(见 build_status)。
typedef struct {
    int  st;            // 0 off / 1 connecting / 2 running / 3 failed
    char mode[8];       // "echo" / "radar"
    char ssid[33];      // ECHO:连接的 SSID
    int  rssi;          // ECHO:dBm
    int  mot;           // ECHO:运动分 0..100
    int  rate;          // 包速率 pkt/s
    int  ftm;           // ECHO:FTM 距离 cm(0 = 无效)
    int  fv;            // ECHO:FTM 有效位 0/1
    int  occ;           // ECHO:占用 0/1
    int  occ_s;         // ECHO:占用态持续秒数
    int  dev_ph;        // RADAR:各类型计数
    int  dev_pc;
    int  dev_io;
    int  dev_ap;
    int  dev_unk;
    int  dev_tot;
} csi_status_t;

// 生成 STATUS JSON 到 out(NUL 结尾)。按 s->mode 决定字段:
//   radar: {"st":..,"mode":"radar","dev":{"ph":..,"pc":..,"io":..,"ap":..,"unk":..,"tot":..},"rate":..}
//   其它(echo):{"st":..,"mode":"echo","ssid":"..","rssi":..,"mot":..,"occ":..,"occ_s":..,"rate":..,"ftm":..,"fv":..}
// ssid 做 JSON 转义。返回写入字符数(不含 NUL);cap 不足返回 -1。
int csi_proto_build_status(char *out, size_t cap, const csi_status_t *s);

// 解析 CONTROL JSON 命令。识别 start/stop/ftm/ping_ms/mode/sens/occ_th。
//   ping_ms.v 缺省 100,clamp 20..2000;occ_th.v clamp 0..100;
//   sens.alpha clamp 0..1(缺省 -1),sens.scale 要求 >0(缺省 -1)。
// 识别成功返回 true 并填 out;未知/非法返回 false 且 out->kind = CSI_CMD_NONE。
bool csi_proto_parse_control(const char *in, csi_cmd_t *out);
