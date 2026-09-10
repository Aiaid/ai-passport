// tests/test_csi_proto.c —— csi_proto(BLE 线上协议纯逻辑)的主机侧单元测试。
//
// 只依赖 C 标准库。编译运行:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_csi_proto.c main/csi_proto.c -o /tmp/test_csi_proto
//   /tmp/test_csi_proto
// 全部断言通过时 main 返回 0。

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csi_proto.h"

// ---------------------------------------------------------------------------
// STATUS JSON 生成
// ---------------------------------------------------------------------------
static void test_build_basic(void)
{
    char buf[256];
    int n = csi_proto_build_status(buf, sizeof(buf), 2, "home", -47, 38, 9, 312, 1);
    assert(n > 0);
    assert((size_t)n == strlen(buf));
    assert(strcmp(buf,
        "{\"st\":2,\"ssid\":\"home\",\"rssi\":-47,\"mot\":38,"
        "\"rate\":9,\"ftm\":312,\"fv\":1}") == 0);
}

static void test_build_field_order_and_values(void)
{
    char buf[256];
    csi_proto_build_status(buf, sizeof(buf), 0, "", 0, 0, 0, 0, 0);
    // 空 SSID、全 0,字段顺序固定
    assert(strcmp(buf,
        "{\"st\":0,\"ssid\":\"\",\"rssi\":0,\"mot\":0,"
        "\"rate\":0,\"ftm\":0,\"fv\":0}") == 0);
}

static void test_build_escaping(void)
{
    char buf[256];
    // SSID 含双引号和反斜杠,需转义
    csi_proto_build_status(buf, sizeof(buf), 1, "a\"b\\c", -30, 0, 0, 0, 0);
    assert(strstr(buf, "\"ssid\":\"a\\\"b\\\\c\"") != NULL);

    // 控制字符转 \u00XX
    csi_proto_build_status(buf, sizeof(buf), 1, "x\ty", -30, 0, 0, 0, 0);
    assert(strstr(buf, "\"ssid\":\"x\\u0009y\"") != NULL);
}

static void test_build_truncation(void)
{
    char buf[8];
    int n = csi_proto_build_status(buf, sizeof(buf), 2, "home", -47, 38, 9, 312, 1);
    assert(n == -1);  // 容量不足
}

// ---------------------------------------------------------------------------
// CONTROL 命令解析
// ---------------------------------------------------------------------------
static void test_parse_simple_cmds(void)
{
    csi_cmd_t c;
    assert(csi_proto_parse_control("{\"cmd\":\"start\"}", &c) && c.kind == CSI_CMD_START);
    assert(csi_proto_parse_control("{\"cmd\":\"stop\"}", &c) && c.kind == CSI_CMD_STOP);
    assert(csi_proto_parse_control("{\"cmd\":\"ftm\"}", &c) && c.kind == CSI_CMD_FTM);
    // 空白容忍
    assert(csi_proto_parse_control("{ \"cmd\" : \"start\" }", &c) && c.kind == CSI_CMD_START);
}

static void test_parse_ping_ms(void)
{
    csi_cmd_t c;
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\",\"v\":100}", &c));
    assert(c.kind == CSI_CMD_PING_MS && c.v == 100);

    // 下界 clamp
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\",\"v\":5}", &c));
    assert(c.v == 20);

    // 上界 clamp
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\",\"v\":5000}", &c));
    assert(c.v == 2000);

    // 负数 clamp 到下界
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\",\"v\":-10}", &c));
    assert(c.v == 20);

    // 缺 v:按默认 100
    assert(csi_proto_parse_control("{\"cmd\":\"ping_ms\"}", &c));
    assert(c.kind == CSI_CMD_PING_MS && c.v == 100);
}

static void test_parse_invalid(void)
{
    csi_cmd_t c;
    // 未知命令
    assert(!csi_proto_parse_control("{\"cmd\":\"boom\"}", &c));
    assert(c.kind == CSI_CMD_NONE);
    // 无 cmd 字段
    assert(!csi_proto_parse_control("{\"v\":100}", &c));
    // 非法 JSON / 空
    assert(!csi_proto_parse_control("garbage", &c));
    assert(!csi_proto_parse_control("", &c));
    assert(!csi_proto_parse_control(NULL, &c));
    // cmd 值未闭合
    assert(!csi_proto_parse_control("{\"cmd\":\"start", &c));
}

int main(void)
{
    test_build_basic();
    test_build_field_order_and_values();
    test_build_escaping();
    test_build_truncation();
    test_parse_simple_cmds();
    test_parse_ping_ms();
    test_parse_invalid();
    printf("test_csi_proto: all assertions passed\n");
    return 0;
}
