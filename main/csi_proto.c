// main/csi_proto.c —— 见 csi_proto.h。纯 C,只用标准库。
#include "csi_proto.h"

#include <stdio.h>
#include <string.h>

// 把 src 做 JSON 字符串转义写入 dst(不含外层引号),保证 NUL 结尾且不越界。
// 非可打印 ASCII(含 >=0x80 字节)按 \u00XX 输出:结果始终是合法 JSON。
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
            dst[w++] = '\\'; dst[w++] = 'u'; dst[w++] = '0'; dst[w++] = '0';
            dst[w++] = hex[(c >> 4) & 0xF];
            dst[w++] = hex[c & 0xF];
        }
    }
    dst[w] = '\0';
}

int csi_proto_build_status(char *out, size_t cap, const csi_status_t *s)
{
    if (out == NULL || cap == 0 || s == NULL) return -1;

    int n;
    if (strcmp(s->mode, "radar") == 0) {
        n = snprintf(out, cap,
            "{\"st\":%d,\"mode\":\"radar\",\"dev\":{\"ph\":%d,\"pc\":%d,"
            "\"io\":%d,\"ap\":%d,\"unk\":%d,\"tot\":%d},\"rate\":%d}",
            s->st, s->dev_ph, s->dev_pc, s->dev_io, s->dev_ap,
            s->dev_unk, s->dev_tot, s->rate);
    } else {
        char esc[200];
        escape_json(s->ssid, esc, sizeof(esc));
        n = snprintf(out, cap,
            "{\"st\":%d,\"mode\":\"echo\",\"ssid\":\"%s\",\"rssi\":%d,"
            "\"mot\":%d,\"occ\":%d,\"occ_s\":%d,\"rate\":%d,\"ftm\":%d,"
            "\"fv\":%d,\"alert\":%d}",
            s->st, esc, s->rssi, s->mot, s->occ, s->occ_s,
            s->rate, s->ftm, s->fv, s->alert);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

static const char *skip_ws_colon(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    return p;
}

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
    while (*p && *p != '"' && i + 1 < tok_cap) tok[i++] = *p++;
    tok[i] = '\0';
    return (*p == '"');
}

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
        if (acc > 100000000L) break;
        p++;
    }
    *out = (int)(sign * acc);
    return true;
}

// 解析形如 0.25 / -1 / 8 的浮点。成功返回 true。
static bool find_float_value(const char *in, const char *key, float *out)
{
    const char *p = strstr(in, key);
    if (!p) return false;
    p += strlen(key);
    p = skip_ws_colon(p);
    float sign = 1.0f;
    if (*p == '-') { sign = -1.0f; p++; }
    if ((*p < '0' || *p > '9') && *p != '.') return false;
    float val = 0.0f;
    while (*p >= '0' && *p <= '9') { val = val * 10.0f + (float)(*p - '0'); p++; }
    if (*p == '.') {
        p++;
        float frac = 0.1f;
        while (*p >= '0' && *p <= '9') { val += (float)(*p - '0') * frac; frac *= 0.1f; p++; }
    }
    *out = sign * val;
    return true;
}

bool csi_proto_parse_control(const char *in, csi_cmd_t *out)
{
    if (out) {
        out->kind = CSI_CMD_NONE;
        out->v = 0;
        out->alpha = -1.0f;
        out->scale = -1.0f;
        out->mode[0] = '\0';
    }
    if (!in || !out) return false;

    char cmd[16];
    if (!find_string_value(in, "\"cmd\"", cmd, sizeof(cmd))) return false;

    if (strcmp(cmd, "start") == 0) { out->kind = CSI_CMD_START; return true; }
    if (strcmp(cmd, "stop") == 0)  { out->kind = CSI_CMD_STOP;  return true; }
    if (strcmp(cmd, "ftm") == 0)   { out->kind = CSI_CMD_FTM;   return true; }
    if (strcmp(cmd, "calib") == 0) { out->kind = CSI_CMD_CALIB; return true; }

    if (strcmp(cmd, "alert") == 0) {
        int v = 1;  // 缺省开
        find_int_value(in, "\"v\"", &v);
        out->kind = CSI_CMD_ALERT;
        out->v = (v != 0) ? 1 : 0;
        return true;
    }

    if (strcmp(cmd, "ping_ms") == 0) {
        int v = 100;
        find_int_value(in, "\"v\"", &v);
        if (v < 20)   v = 20;
        if (v > 2000) v = 2000;
        out->kind = CSI_CMD_PING_MS;
        out->v = v;
        return true;
    }

    if (strcmp(cmd, "mode") == 0) {
        char m[8];
        if (!find_string_value(in, "\"v\"", m, sizeof(m))) return false;
        if (strcmp(m, "echo") != 0 && strcmp(m, "radar") != 0) return false;
        out->kind = CSI_CMD_MODE;
        strncpy(out->mode, m, sizeof(out->mode) - 1);
        out->mode[sizeof(out->mode) - 1] = '\0';
        return true;
    }

    if (strcmp(cmd, "sens") == 0) {
        float a, s;
        bool any = false;
        if (find_float_value(in, "\"alpha\"", &a)) {
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            out->alpha = a;
            any = true;
        }
        if (find_float_value(in, "\"scale\"", &s)) {
            if (s > 0.0f) { out->scale = s; any = true; }
        }
        if (!any) return false;  // 两者都没有视为非法
        out->kind = CSI_CMD_SENS;
        return true;
    }

    if (strcmp(cmd, "occ_th") == 0) {
        int v = 30;
        find_int_value(in, "\"v\"", &v);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        out->kind = CSI_CMD_OCC_TH;
        out->v = v;
        return true;
    }

    return false;  // 未知命令
}
