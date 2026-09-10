// main/csi_proto.c —— 见 csi_proto.h。纯 C,只用标准库。
#include "csi_proto.h"

#include <stdio.h>
#include <string.h>

// 把 src 做 JSON 字符串转义写入 dst(不含外层引号),保证以 NUL 结尾且不越界。
// 非可打印 ASCII(含 >=0x80 的 UTF-8 字节)按 \u00XX 输出:结果始终是合法 JSON。
static void escape_json(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    if (cap == 0) return;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (w + 2 >= cap) break;
            dst[w++] = '\\';
            dst[w++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            if (w + 1 >= cap) break;
            dst[w++] = (char)c;
        } else {
            if (w + 6 >= cap) break;
            dst[w++] = '\\';
            dst[w++] = 'u';
            dst[w++] = '0';
            dst[w++] = '0';
            dst[w++] = hex[(c >> 4) & 0xF];
            dst[w++] = hex[c & 0xF];
        }
    }
    dst[w] = '\0';
}

int csi_proto_build_status(char *out, size_t cap, int st, const char *ssid,
                           int rssi, int mot, int rate, int ftm, int fv)
{
    if (out == NULL || cap == 0) return -1;

    char esc[200];
    escape_json(ssid ? ssid : "", esc, sizeof(esc));

    int n = snprintf(out, cap,
                     "{\"st\":%d,\"ssid\":\"%s\",\"rssi\":%d,\"mot\":%d,"
                     "\"rate\":%d,\"ftm\":%d,\"fv\":%d}",
                     st, esc, rssi, mot, rate, ftm, fv);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

// 跳过空白、冒号。
static const char *skip_ws_colon(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    return p;
}

// 从 in 中找 key(形如 "cmd")后面的字符串值 token,写入 tok(带 NUL)。
// 成功返回 true。
static bool find_string_value(const char *in, const char *key,
                              char *tok, size_t tok_cap)
{
    const char *p = strstr(in, key);
    if (!p) return false;
    p += strlen(key);
    p = skip_ws_colon(p);
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < tok_cap) {
        tok[i++] = *p++;
    }
    tok[i] = '\0';
    return (*p == '"');  // 必须正常闭合
}

// 从 in 中找 key 后面的整数值(支持负号)。成功返回 true 并写 *out。
static bool find_int_value(const char *in, const char *key, int *out)
{
    const char *p = strstr(in, key);
    if (!p) return false;
    p += strlen(key);
    p = skip_ws_colon(p);
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (*p < '0' || *p > '9') return false;
    long acc = 0;
    while (*p >= '0' && *p <= '9') {
        acc = acc * 10 + (*p - '0');
        if (acc > 100000000L) break;  // 防溢出,后续会 clamp
        p++;
    }
    *out = (int)(sign * acc);
    return true;
}

bool csi_proto_parse_control(const char *in, csi_cmd_t *out)
{
    if (out) { out->kind = CSI_CMD_NONE; out->v = 0; }
    if (!in || !out) return false;

    char cmd[16];
    if (!find_string_value(in, "\"cmd\"", cmd, sizeof(cmd))) {
        return false;
    }

    if (strcmp(cmd, "start") == 0) { out->kind = CSI_CMD_START; return true; }
    if (strcmp(cmd, "stop") == 0)  { out->kind = CSI_CMD_STOP;  return true; }
    if (strcmp(cmd, "ftm") == 0)   { out->kind = CSI_CMD_FTM;   return true; }
    if (strcmp(cmd, "ping_ms") == 0) {
        int v = 100;  // 未给 v 时用默认 100(落在合法区间内)
        find_int_value(in, "\"v\"", &v);
        if (v < 20)   v = 20;
        if (v > 2000) v = 2000;
        out->kind = CSI_CMD_PING_MS;
        out->v = v;
        return true;
    }
    return false;  // 未知命令
}
