// tests/test_disco_class.c —— 设备发现分类纯逻辑的主机侧单元测试。
//
// 只依赖 C 标准库。编译运行:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_disco_class.c main/disco_class.c -o /tmp/test_disco
//   /tmp/test_disco
// 全部断言通过时 main 返回 0。

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "disco_class.h"

static void mk(uint8_t out[6], uint8_t a, uint8_t b, uint8_t c)
{
    out[0] = a; out[1] = b; out[2] = c; out[3] = 0x11; out[4] = 0x22; out[5] = 0x33;
}

// ---------------------------------------------------------------------------
// 随机 MAC 判定:locally-administered 位 = mac[0] & 0x02
// ---------------------------------------------------------------------------
static void test_randomized(void)
{
    uint8_t m[6];
    mk(m, 0x02, 0x00, 0x00); assert(disco_is_randomized(m));   // bit 置位
    mk(m, 0x06, 0x11, 0x22); assert(disco_is_randomized(m));   // 0x06 含 0x02
    mk(m, 0x00, 0x00, 0x00); assert(!disco_is_randomized(m));  // 全局
    mk(m, 0x3C, 0x15, 0xC2); assert(!disco_is_randomized(m));  // Apple 全局
    mk(m, 0xAC, 0xBC, 0x32); assert(!disco_is_randomized(m));  // 0xAC 不含 0x02
}

// ---------------------------------------------------------------------------
// AP 判定:is_ap 为真即 AP,无视 OUI/随机位
// ---------------------------------------------------------------------------
static void test_ap(void)
{
    uint8_t m[6];
    mk(m, 0x3C, 0x15, 0xC2); assert(disco_classify(m, true) == DISCO_AP);  // Apple 但发 beacon
    mk(m, 0x02, 0x00, 0x00); assert(disco_classify(m, true) == DISCO_AP);  // 随机 MAC 的 AP
}

// ---------------------------------------------------------------------------
// OUI → 类型
// ---------------------------------------------------------------------------
static void test_oui_types(void)
{
    uint8_t m[6];
    mk(m, 0x3C, 0x15, 0xC2); assert(disco_classify(m, false) == DISCO_PHONE); // Apple
    mk(m, 0x88, 0x32, 0x9B); assert(disco_classify(m, false) == DISCO_PHONE); // Samsung
    mk(m, 0x3C, 0xA9, 0xF4); assert(disco_classify(m, false) == DISCO_PC);    // Intel
    mk(m, 0x00, 0x14, 0x22); assert(disco_classify(m, false) == DISCO_PC);    // Dell
    mk(m, 0x24, 0x0A, 0xC4); assert(disco_classify(m, false) == DISCO_IOT);   // Espressif
    mk(m, 0xB8, 0x27, 0xEB); assert(disco_classify(m, false) == DISCO_IOT);   // Raspberry Pi
}

// ---------------------------------------------------------------------------
// 随机 MAC(非 AP)→ UNKNOWN;未知全局 OUI → UNKNOWN
// ---------------------------------------------------------------------------
static void test_unknown(void)
{
    uint8_t m[6];
    mk(m, 0x02, 0x0A, 0xC4); assert(disco_classify(m, false) == DISCO_UNKNOWN); // 随机位,忽略 OUI
    mk(m, 0xAA, 0xBB, 0xCC); assert(disco_classify(m, false) == DISCO_UNKNOWN); // 未知全局 OUI
}

// ---------------------------------------------------------------------------
// 厂商查表
// ---------------------------------------------------------------------------
static void test_vendor(void)
{
    uint8_t m[6];
    mk(m, 0x3C, 0x15, 0xC2); assert(strcmp(disco_vendor(m), "Apple") == 0);
    mk(m, 0x24, 0x0A, 0xC4); assert(strcmp(disco_vendor(m), "Espressif") == 0);
    mk(m, 0xB8, 0x27, 0xEB); assert(strcmp(disco_vendor(m), "Raspberry Pi") == 0);
    mk(m, 0xAA, 0xBB, 0xCC); assert(strcmp(disco_vendor(m), "") == 0);  // 未命中
    mk(m, 0x02, 0x0A, 0xC4); assert(strcmp(disco_vendor(m), "") == 0);  // 随机 MAC 不命中
}

static void test_type_name(void)
{
    assert(strcmp(disco_type_name(DISCO_AP), "AP") == 0);
    assert(strcmp(disco_type_name(DISCO_PHONE), "PHONE") == 0);
    assert(strcmp(disco_type_name(DISCO_PC), "PC") == 0);
    assert(strcmp(disco_type_name(DISCO_IOT), "IOT") == 0);
    assert(strcmp(disco_type_name(DISCO_UNKNOWN), "?") == 0);
}

int main(void)
{
    test_randomized();
    test_ap();
    test_oui_types();
    test_unknown();
    test_vendor();
    test_type_name();
    printf("test_disco_class: all assertions passed\n");
    return 0;
}
