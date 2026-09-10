// main/echo_state.c —— 见 echo_state.h。
#include "echo_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define ECHO_CMD_QUEUE_DEPTH 8

// 默认灵敏度/阈值。occ_th=25 取自真机:空场运动分中值 ~16-18、人在 ~32,
// 中值滤波后二者干净分离,25 居中(两侧各约 7 余量)。scale/alpha 维持原值,
// 因为尖峰问题由占用模块的中值滤波解决,无需改变显示动态。
#define DEF_ALPHA  0.2f
#define DEF_SCALE  8.0f
#define DEF_OCC_TH 25
#define DEF_PING_MS 100

static SemaphoreHandle_t s_mtx;
static QueueHandle_t     s_cmd_q;

static csi_status_t s_state;
static float s_alpha = DEF_ALPHA;
static float s_scale = DEF_SCALE;
static int   s_occ_th = DEF_OCC_TH;
static int   s_ping_ms = DEF_PING_MS;

static char s_mode_req[8];
static bool s_mode_req_pending;

static int s_alert_mode = 1;    // 0 off / 1 once / 2 continuous(默认 once)
static int s_volume = 70;       // 蜂鸣音量
static int s_brightness = 100;  // 屏幕亮度

static void lock(void)   { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void) { if (s_mtx) xSemaphoreGive(s_mtx); }

void echo_state_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_cmd_q) s_cmd_q = xQueueCreate(ECHO_CMD_QUEUE_DEPTH, sizeof(csi_cmd_t));
    lock();
    memset(&s_state, 0, sizeof(s_state));
    strcpy(s_state.mode, "echo");  // 默认模式
    s_alpha = DEF_ALPHA;
    s_scale = DEF_SCALE;
    s_occ_th = DEF_OCC_TH;
    s_ping_ms = DEF_PING_MS;
    s_mode_req_pending = false;
    s_alert_mode = 1;
    s_volume = 70;
    s_brightness = 100;
    unlock();
}

void echo_state_get(csi_status_t *out)
{
    if (!out) return;
    lock();
    *out = s_state;
    unlock();
}

void echo_state_set_mode(const char *mode)
{
    if (!mode) return;
    lock();
    strncpy(s_state.mode, mode, sizeof(s_state.mode) - 1);
    s_state.mode[sizeof(s_state.mode) - 1] = '\0';
    unlock();
}

void echo_state_set_echo(int st, const char *ssid, int rssi, int mot,
                         int rate, int ftm, int fv, int occ, int occ_s)
{
    lock();
    strcpy(s_state.mode, "echo");
    s_state.st = st;
    strncpy(s_state.ssid, ssid ? ssid : "", sizeof(s_state.ssid) - 1);
    s_state.ssid[sizeof(s_state.ssid) - 1] = '\0';
    s_state.rssi = rssi;
    s_state.mot = mot;
    s_state.rate = rate;
    s_state.ftm = ftm;
    s_state.fv = fv;
    s_state.occ = occ;
    s_state.occ_s = occ_s;
    unlock();
}

void echo_state_set_radar(int st, int rate,
                          int ph, int pc, int io, int ap, int unk, int tot)
{
    lock();
    strcpy(s_state.mode, "radar");
    s_state.st = st;
    s_state.rate = rate;
    s_state.dev_ph = ph;
    s_state.dev_pc = pc;
    s_state.dev_io = io;
    s_state.dev_ap = ap;
    s_state.dev_unk = unk;
    s_state.dev_tot = tot;
    unlock();
}

void echo_state_set_idle(void)
{
    lock();
    s_state.st = 0;
    unlock();
}

void echo_state_set_sens(float alpha, float scale)
{
    lock();
    if (alpha >= 0.0f) s_alpha = alpha;
    if (scale > 0.0f)  s_scale = scale;
    unlock();
}

void echo_state_get_sens(float *alpha, float *scale)
{
    lock();
    if (alpha) *alpha = s_alpha;
    if (scale) *scale = s_scale;
    unlock();
}

void echo_state_set_occ_th(int th)
{
    lock();
    s_occ_th = th;
    unlock();
}

int echo_state_get_occ_th(void)
{
    lock();
    int v = s_occ_th;
    unlock();
    return v;
}

void echo_state_set_ping_ms(int ms)
{
    lock();
    s_ping_ms = ms;
    unlock();
}

int echo_state_get_ping_ms(void)
{
    lock();
    int v = s_ping_ms;
    unlock();
    return v;
}

void echo_state_request_mode(const char *mode)
{
    if (!mode) return;
    lock();
    strncpy(s_mode_req, mode, sizeof(s_mode_req) - 1);
    s_mode_req[sizeof(s_mode_req) - 1] = '\0';
    s_mode_req_pending = true;
    unlock();
}

bool echo_state_take_mode_request(char *out, int cap)
{
    bool pending;
    lock();
    pending = s_mode_req_pending;
    if (pending && out && cap > 0) {
        strncpy(out, s_mode_req, cap - 1);
        out[cap - 1] = '\0';
    }
    s_mode_req_pending = false;
    unlock();
    return pending;
}

void echo_state_mark_alert(void)
{
    lock();
    s_state.alert++;
    unlock();
}

void echo_state_set_alert_mode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    lock();
    s_alert_mode = mode;
    unlock();
}

int echo_state_get_alert_mode(void)
{
    lock();
    int v = s_alert_mode;
    unlock();
    return v;
}

void echo_state_set_alert_enabled(bool en)
{
    lock();
    s_alert_mode = en ? 1 : 0;   // 兼容旧开关:开=once,关=off
    unlock();
}

bool echo_state_alert_enabled(void)
{
    lock();
    bool v = (s_alert_mode != 0);
    unlock();
    return v;
}

void echo_state_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    lock();
    s_volume = vol;
    unlock();
}

int echo_state_get_volume(void)
{
    lock();
    int v = s_volume;
    unlock();
    return v;
}

void echo_state_set_brightness(int b)
{
    lock();
    s_brightness = b;
    unlock();
}

int echo_state_get_brightness(void)
{
    lock();
    int v = s_brightness;
    unlock();
    return v;
}

void echo_state_post_cmd(const csi_cmd_t *cmd)
{
    if (s_cmd_q && cmd) xQueueSend(s_cmd_q, cmd, 0);
}

bool echo_state_take_cmd(csi_cmd_t *out)
{
    if (!s_cmd_q || !out) return false;
    return xQueueReceive(s_cmd_q, out, 0) == pdTRUE;
}
