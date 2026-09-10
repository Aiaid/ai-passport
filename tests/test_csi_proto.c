// tests/test_csi_proto.c —— csi_proto(BLE 线上协议纯逻辑 v2)的主机侧单元测试。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_csi_proto.c main/csi_proto.c -o /tmp/test_csi_proto && /tmp/test_csi_proto

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "csi_proto.h"

// ---------------------------------------------------------------------------
// STATUS JSON(按 mode 形态)
// ---------------------------------------------------------------------------
static void test_build_echo(void)
{
    char buf[256];
    csi_status_t s = { 0 };
    s.st = 2; strcpy(s.mode, "echo"); strcpy(s.ssid, "anend-iot");
    s.rssi = -47; s.mot = 38; s.occ = 1; s.occ_s = 12; s.rate = 10; s.alert = 3;
    int n = csi_proto_build_status(buf, sizeof(buf), &s);
    assert(n > 0 && (size_t)n == strlen(buf));
    assert(strcmp(buf,
        "{\"st\":2,\"mode\":\"echo\",\"ssid\":\"anend-iot\",\"rssi\":-47,"
        "\"mot\":38,\"occ\":1,\"occ_s\":12,\"rate\":10,\"ftm\":0,"
        "\"fv\":0,\"alert\":3}") == 0);
}

static void test_build_radar(void)
{
    char buf[256];
    csi_status_t s = { 0 };
    s.st = 2; strcpy(s.mode, "radar");
    s.dev_ap = 14; s.dev_unk = 23; s.dev_tot = 37; s.rate = 0;
    int n = csi_proto_build_status(buf, sizeof(buf), &s);
    assert(n > 0);
    assert(strcmp(buf,
        "{\"st\":2,\"mode\":\"radar\",\"dev\":{\"ph\":0,\"pc\":0,\"io\":0,"
        "\"ap\":14,\"unk\":23,\"tot\":37},\"rate\":0}") == 0);
}

static void test_build_escaping_and_trunc(void)
{
    char buf[256];
    csi_status_t s = { 0 };
    strcpy(s.mode, "echo"); strcpy(s.ssid, "a\"b\\c");
    csi_proto_build_status(buf, sizeof(buf), &s);
    assert(strstr(buf, "\"ssid\":\"a\\\"b\\\\c\"") != NULL);

    char small[8];
    assert(csi_proto_build_status(small, sizeof(small), &s) == -1);
}

// ---------------------------------------------------------------------------
// CONTROL 解析(v1 + v2)
// ---------------------------------------------------------------------------
static void test_parse_v1(void)
{
    csi_cmd_t c;
    assert(csi_proto_parse_control("{\"cmd\":\"start\"}", &c) && c.kind == CSI_CMD_START);
    assert(csi_proto_parse_control("{\"cmd\":\"stop\"}", &c) && c.kind == CSI_CMD_STOP);
    assert(csi_proto_parse_control("{\"cmd\":\"ftm\"}", &c) && c.kind == CSI_CMD_FTM);
    assert(csi_proto_parse_control("{\"cmd\":\"calib\"}", &c) && c.kind == CSI_CMD_CALIB);
    assert(csi_proto_parse_control("{\"cmd\":\"alert\",\"v\":0}", &c) && c.kind == CSI_CMD_ALERT && c.v == 0);
    assert(csi_proto_parse_control("{\"cmd\":\"alert\",\"v\":1}", &c) && c.v == 1);
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\",\"v\":5}", &c) && c.v == 20);
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\",\"v\":5000}", &c) && c.v == 2000);
}

static void test_parse_mode(void)
{
    csi_cmd_t c;
    assert(csi_proto_parse_control("{\"cmd\":\"mode\",\"v\":\"radar\"}", &c));
    assert(c.kind == CSI_CMD_MODE && strcmp(c.mode, "radar") == 0);
    assert(csi_proto_parse_control("{\"cmd\":\"mode\",\"v\":\"echo\"}", &c));
    assert(strcmp(c.mode, "echo") == 0);
    // 非法模式值 → false
    assert(!csi_proto_parse_control("{\"cmd\":\"mode\",\"v\":\"bogus\"}", &c));
}

static void test_parse_sens(void)
{
    csi_cmd_t c;
    assert(csi_proto_parse_control("{\"cmd\":\"sens\",\"alpha\":0.25,\"scale\":8}", &c));
    assert(c.kind == CSI_CMD_SENS);
    assert(fabsf(c.alpha - 0.25f) < 1e-4f);
    assert(fabsf(c.scale - 8.0f) < 1e-4f);

    // alpha clamp 到 0..1;scale<=0 被忽略但 alpha 有效 → 仍成立
    assert(csi_proto_parse_control("{\"cmd\":\"sens\",\"alpha\":2.0,\"scale\":-3}", &c));
    assert(fabsf(c.alpha - 1.0f) < 1e-4f);
    assert(c.scale < 0.0f);  // 未采纳非法 scale

    // 只给 scale
    assert(csi_proto_parse_control("{\"cmd\":\"sens\",\"scale\":4.5}", &c));
    assert(c.alpha < 0.0f && fabsf(c.scale - 4.5f) < 1e-4f);

    // 都没有 → 非法
    assert(!csi_proto_parse_control("{\"cmd\":\"sens\"}", &c));
}

static void test_parse_occ_th(void)
{
    csi_cmd_t c;
    assert(csi_proto_parse_control("{\"cmd\":\"occ_th\",\"v\":45}", &c));
    assert(c.kind == CSI_CMD_OCC_TH && c.v == 45);
    assert(csi_proto_parse_control("{\"cmd\":\"occ_th\",\"v\":500}", &c) && c.v == 100);
    assert(csi_proto_parse_control("{\"cmd\":\"occ_th\",\"v\":-5}", &c) && c.v == 0);
    assert(csi_proto_parse_control("{\"cmd\":\"occ_th\"}", &c) && c.v == 30);  // 缺省
}

static void test_parse_invalid(void)
{
    csi_cmd_t c;
    assert(!csi_proto_parse_control("{\"cmd\":\"boom\"}", &c));
    assert(c.kind == CSI_CMD_NONE);
    assert(!csi_proto_parse_control("garbage", &c));
    assert(!csi_proto_parse_control("", &c));
    assert(!csi_proto_parse_control(NULL, &c));
}

int main(void)
{
    test_build_echo();
    test_build_radar();
    test_build_escaping_and_trunc();
    test_parse_v1();
    test_parse_mode();
    test_parse_sens();
    test_parse_occ_th();
    test_parse_invalid();
    printf("test_csi_proto: all assertions passed\n");
    return 0;
}
