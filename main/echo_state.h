// main/echo_state.h —— 跨任务共享的设备状态 + 可调参数 + 模式切换请求 + ECHO
// 命令队列。常驻 BLE(csi_ble)从这里读 STATUS;ECHO / RADAR 两个 demo 往这里写;
// 主循环从这里取模式切换请求;demo_csi 从这里取 start/stop/ftm/ping_ms 命令。
//
// 所有访问加锁(互斥量/队列),可在任意任务调用。
#pragma once

#include <stdbool.h>
#include "csi_proto.h"

// 开机一次性初始化(建互斥量与命令队列)。幂等。
void echo_state_init(void);

// 读取当前状态快照(csi_ble 的 notify / STATUS 读回调用)。
void echo_state_get(csi_status_t *out);

// 设置当前模式字符串("echo"/"radar")。
void echo_state_set_mode(const char *mode);

// ECHO 模式每周期写入运行状态。
void echo_state_set_echo(int st, const char *ssid, int rssi, int mot,
                         int rate, int ftm, int fv, int occ, int occ_s);

// RADAR 模式每周期写入发现摘要。
void echo_state_set_radar(int st, int rate,
                          int ph, int pc, int io, int ap, int unk, int tot);

// demo 退出时置空闲(st=0,保留 mode)。
void echo_state_set_idle(void);

// 可调参数:灵敏度(alpha/scale)与占用阈值(occ_th)。
// 传入 <0(alpha/scale)表示不改该项;occ_th 直接设置。
void echo_state_set_sens(float alpha, float scale);
void echo_state_get_sens(float *alpha, float *scale);
void echo_state_set_occ_th(int th);
int  echo_state_get_occ_th(void);
void echo_state_set_ping_ms(int ms);
int  echo_state_get_ping_ms(void);

// 远程模式切换请求:BLE 回调 set,主循环 take(取走后清除)。
void echo_state_request_mode(const char *mode);
bool echo_state_take_mode_request(char *out, int cap);  // 有待处理返回 true

// ECHO 命令队列(start/stop/ftm/ping_ms):BLE post,demo_csi 非阻塞 take。
void echo_state_post_cmd(const csi_cmd_t *cmd);
bool echo_state_take_cmd(csi_cmd_t *out);

// 绊线告警:占用跳变时自增序号(随 STATUS 带出)。
void echo_state_mark_alert(void);
// 告警模式:0 off / 1 once / 2 continuous。alert_enabled 为兼容旧开关(mode!=0)。
void echo_state_set_alert_mode(int mode);
int  echo_state_get_alert_mode(void);
void echo_state_set_alert_enabled(bool en);   // en -> mode once / off
bool echo_state_alert_enabled(void);
// 蜂鸣音量 0..100。
void echo_state_set_volume(int vol);
int  echo_state_get_volume(void);
// 屏幕亮度 0..100(下限由调用方 clamp)。
void echo_state_set_brightness(int b);
int  echo_state_get_brightness(void);
