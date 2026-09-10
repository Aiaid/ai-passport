// main/disco_class.h —— 设备发现(RADAR)的分类纯逻辑:随机 MAC 判定 + 粗粒度
// 类型分类 + 厂商查表。纯 C,仅依赖标准库,不 include 任何 esp_ 头,便于主机
// 单元测试(见 tests/test_disco_class.c)。设备侧由 demo_discovery.c 调用。
//
// 说明:现代设备普遍 MAC 随机化,随机 MAC 的 OUI 无意义,分类只能粗略猜测。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISCO_UNKNOWN = 0,
    DISCO_AP,      // 接入点(由 beacon / probe-response 管理帧判定)
    DISCO_PHONE,   // 手机/平板(按全局 OUI 猜测)
    DISCO_PC,      // 笔记本/PC
    DISCO_IOT,     // IoT / 开发板
} disco_type_t;

// locally-administered 位(mac[0] 的 bit1)置位即为随机/本地管理 MAC。
bool disco_is_randomized(const uint8_t mac[6]);

// 厂商名:在内置小型 OUI 表命中返回厂商字符串,否则返回 ""(空串,非 NULL)。
const char *disco_vendor(const uint8_t mac[6]);

// 粗分类:
//   is_ap 为真(见过该 MAC 发 beacon/probe-resp)→ DISCO_AP;
//   否则随机 MAC → DISCO_UNKNOWN;
//   否则查 OUI 表 → PHONE/PC/IOT;未命中 → DISCO_UNKNOWN。
disco_type_t disco_classify(const uint8_t mac[6], bool is_ap);

// 类型的短名:"AP"/"PHONE"/"PC"/"IOT"/"?"。
const char *disco_type_name(disco_type_t t);
