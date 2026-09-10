// main/demo_csi.c —— 被动 WiFi CSI 采集模式(在场/运动检测)+ FTM 测距 + BLE 控制。
//
// 生命周期(反复进出必须不崩,严格遵循 AGENTS):
//   enter(): 建 LVGL 屏 + UI 刷新 lv_timer,起后台 task。后台 task:起 BLE
//            (广播 + 接收 CONTROL 命令)→ 自动开始采集(连 WiFi → 配置并开启
//            CSI → 定速 ping)→ 进命令循环(start/stop/ftm/ping_ms)。
//   CSI 回调(WiFi task,绝不碰 LVGL):算幅度/运动分,按 CSV 契约 printf 到串口;
//            更新 volatile 共享状态(运动分、包计数、rssi)。
//   FTM 报告(WiFi task):更新距离 cm 与有效位。
//   BLE notify/读(nimble task / esp_timer):读共享状态生成 STATUS JSON。
//   lv_timer(LVGL task、已持锁):刷新状态行、速率、运动条、距离行。
//   exit(): 请求后台收场 → 后台按序关 CSI/ping/FTM、停 WiFi、停 BLE 广播;
//            exit 先停 UI 定时器、等后台结束、最后删屏。
//
// 屏上文案用 ASCII:内置 montserrat 字体无中文字形。
#include "demo.h"
#include "demo_csi.h"
#include "demo_radio.h"
#include "csi_metric.h"
#include "csi_proto.h"
#include "csi_ble.h"
#include "ui_echo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

static const char *TAG = "demo_csi";

#define CSI_MOTION_ALPHA       0.2f
#define CSI_MOTION_SCALE       8.0f
#define CSI_CONNECT_TIMEOUT_MS 15000
#define CSI_UI_REFRESH_MS      300
#define CSI_CMD_QUEUE_DEPTH    8

// 采集内部状态(映射到协议 st:off/connecting/running/failed)。
enum { CAP_CONNECTING = 0, CAP_CONNECTED, CAP_FAILED };

#define CSI_BARS 16  // 屏上子载波热力条数量(由幅度降采样而来,仅展示用)

// --- LVGL 对象(仅 LVGL task 访问)---
static lv_obj_t   *s_scr;
static lv_obj_t   *s_status;     // 状态行:SSID / 连接态
static lv_obj_t   *s_rssi_lbl;   // 状态行右侧 RSSI
static lv_obj_t   *s_arc;        // 半圆运动分仪表
static lv_obj_t   *s_motion_num; // 仪表中央大数字
static lv_obj_t   *s_heat[CSI_BARS];  // 子载波热力条
static lv_obj_t   *s_rate;
static lv_obj_t   *s_dist;
static lv_timer_t *s_ui_timer;
static uint32_t    s_ui_last_ms;
static uint32_t    s_ui_last_count;

// --- 后台 task / 命令队列 ---
static TaskHandle_t  s_worker;
static volatile bool s_stop_req;
static volatile bool s_worker_alive;
static QueueHandle_t s_cmd_queue;

// --- 共享状态(int 用 volatile;ssid 用互斥量保护)---
static SemaphoreHandle_t s_lock;
static volatile bool     s_capturing;
static volatile int      s_cap_state;    // CAP_*
static volatile int      s_motion;       // 0..100
static volatile int      s_rssi;         // dBm
static volatile int      s_rate_pps;     // CSI pkt/s(由 ui_tick 计算)
static volatile uint32_t s_pkt_count;
static volatile int      s_ftm_cm;       // 距离 cm(0 = 无效)
static volatile int      s_ftm_valid;    // 0/1
static volatile int      s_bars[CSI_BARS]; // 子载波幅度 16-bin 归一化(0..100)
static char              s_ssid[33];

// --- 采集资源(仅后台 task / 其回调持有)---
static bool              s_wifi_up;
static bool              s_csi_on;
static esp_ping_handle_t s_ping;
static esp_event_handler_instance_t s_ftm_evt;
static bool              s_ftm_reg;
static esp_timer_handle_t s_ftm_auto_timer;
static ip_addr_t         s_ping_target;
static bool              s_ping_target_ok;
static int               s_ping_interval_ms;
static csi_motion_t      s_filt;

// ===========================================================================
// 共享状态辅助
// ===========================================================================
static void set_ssid(const char *ssid)
{
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        strncpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
        xSemaphoreGive(s_lock);
    }
}

static void get_ssid(char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        strncpy(out, s_ssid, cap - 1);
        out[cap - 1] = '\0';
        xSemaphoreGive(s_lock);
    }
}

// 协议 st:0 off / 1 connecting / 2 running / 3 failed。
static int protocol_st(void)
{
    if (!s_capturing) return 0;
    switch (s_cap_state) {
    case CAP_CONNECTING: return 1;
    case CAP_CONNECTED:  return 2;
    default:             return 3;
    }
}

// BLE 状态提供者(nimble task / esp_timer 调用)。
static void fill_status(csi_status_t *out)
{
    out->st = protocol_st();
    get_ssid(out->ssid, sizeof(out->ssid));
    out->rssi = s_rssi;
    out->mot = s_motion;
    out->rate = s_rate_pps;
    out->ftm = s_ftm_cm;
    out->fv = s_ftm_valid;
}

// ===========================================================================
// CSI 回调(WiFi task)
// ===========================================================================
static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (s_stop_req || !info || !info->buf || info->len <= 0) {
        return;
    }
    float amp[CSI_METRIC_MAX_SUBCARRIERS];
    int n = csi_metric_amplitudes(info->buf, info->len, amp,
                                  CSI_METRIC_MAX_SUBCARRIERS);
    int motion = csi_motion_update(&s_filt, amp, n);
    s_motion = motion;
    s_rssi = info->rx_ctrl.rssi;
    s_pkt_count++;

    // 把 n 个子载波幅度降采样成 16 个 bin 并按帧内最大值归一化,喂给屏上热力条。
    // 纯展示派生量,不改变采集/回调行为。
    if (n > 0) {
        float mx = 0.0f;
        for (int i = 0; i < n; i++) if (amp[i] > mx) mx = amp[i];
        for (int b = 0; b < CSI_BARS; b++) {
            int lo = b * n / CSI_BARS;
            int hi = (b + 1) * n / CSI_BARS;
            if (hi <= lo) hi = lo + 1;
            float sum = 0.0f;
            int cntb = 0;
            for (int i = lo; i < hi && i < n; i++) { sum += amp[i]; cntb++; }
            float avg = cntb ? sum / (float)cntb : 0.0f;
            s_bars[b] = (mx > 0.0f) ? (int)(avg / mx * 100.0f) : 0;
        }
    }

    const wifi_pkt_rx_ctrl_t *rx = &info->rx_ctrl;
    printf("CSI_DATA,%u,%u,%d,%u,%d,%u,%d,%u,[",
           (unsigned)info->rx_seq,
           (unsigned)rx->timestamp,
           (int)rx->rssi,
           (unsigned)rx->rate,
           (int)rx->noise_floor,
           (unsigned)rx->channel,
           motion,
           (unsigned)info->len);
    for (int i = 0; i < info->len; i++) {
        printf("%d", (int)info->buf[i]);
        if (i + 1 < info->len) putchar(' ');
    }
    printf("]\n");
}

// ===========================================================================
// FTM
// ===========================================================================
static void ftm_report_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    wifi_event_ftm_report_t *r = (wifi_event_ftm_report_t *)data;
    if (r && r->status == FTM_STATUS_SUCCESS) {
        s_ftm_cm = (int)r->dist_est;
        s_ftm_valid = 1;
        ESP_LOGI(TAG, "FTM 距离 %d cm", s_ftm_cm);
    } else {
        s_ftm_cm = 0;
        s_ftm_valid = 0;
        ESP_LOGW(TAG, "FTM 失败 status=%d", r ? (int)r->status : -1);
    }
    // ftm_report_data 由驱动分配,使用指针方式时需释放;我们只用 dist_est,
    // 释放以防内存泄漏。
    if (r && r->ftm_report_data) {
        free(r->ftm_report_data);
    }
}

static void trigger_ftm(void)
{
    if (!s_wifi_up || s_cap_state != CAP_CONNECTED) {
        s_ftm_valid = 0;
        s_ftm_cm = 0;
        return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        s_ftm_valid = 0;
        s_ftm_cm = 0;
        return;
    }
    if (!ap.ftm_responder) {
        // AP 不支持 FTM responder:优雅降级。
        s_ftm_valid = 0;
        s_ftm_cm = 0;
        ESP_LOGW(TAG, "AP 不支持 FTM responder,距离不可用");
        return;
    }
    wifi_ftm_initiator_cfg_t cfg = {
        .channel = ap.primary,
        .frm_count = 16,
        .burst_period = 2,
        .use_get_report_api = false,
    };
    memcpy(cfg.resp_mac, ap.bssid, 6);
    esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    if (err != ESP_OK) {
        s_ftm_valid = 0;
        s_ftm_cm = 0;
        ESP_LOGW(TAG, "发起 FTM 失败: %s", esp_err_to_name(err));
    }
}

#if CONFIG_CSI_FTM_AUTO_INTERVAL_S > 0
static void ftm_auto_cb(void *arg)
{
    (void)arg;
    // 自动测距:投递命令交后台串行处理,避免并发发起。
    if (s_cmd_queue) {
        csi_cmd_t cmd = { .kind = CSI_CMD_FTM, .v = 0 };
        xQueueSend(s_cmd_queue, &cmd, 0);
    }
}
#endif

// ===========================================================================
// ping
// ===========================================================================
static void start_ping(void)
{
    if (!s_ping_target_ok || s_ping) return;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = s_ping_target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = (uint32_t)s_ping_interval_ms;
    cfg.timeout_ms = 1000;
    esp_ping_callbacks_t cbs = { 0 };
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) == ESP_OK) {
        esp_ping_start(s_ping);
    } else {
        ESP_LOGW(TAG, "ping 会话创建失败");
        s_ping = NULL;
    }
}

static void stop_ping(void)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}

// ===========================================================================
// 采集开/关(均在后台 task 上下文)
// ===========================================================================
static void capture_start(void)
{
    if (s_capturing) return;
    s_capturing = true;
    s_cap_state = CAP_CONNECTING;
    s_motion = 0;
    s_pkt_count = 0;
    s_ftm_cm = 0;
    s_ftm_valid = 0;
    csi_motion_init(&s_filt, CSI_MOTION_ALPHA, CSI_MOTION_SCALE);

    const char *ssid = CONFIG_CSI_WIFI_SSID;
    const char *pw = CONFIG_CSI_WIFI_PASSWORD;
    set_ssid(ssid);
    if (ssid[0] == '\0') {
        s_cap_state = CAP_FAILED;  // 未配置:屏上提示,不崩
        return;
    }

    uint32_t gw = 0;
    esp_err_t err = demo_radio_wifi_sta_start(ssid, pw, CSI_CONNECT_TIMEOUT_MS,
                                              &s_stop_req, &gw);
    if (err != ESP_OK) {
        s_cap_state = (s_stop_req) ? CAP_CONNECTING : CAP_FAILED;
        return;
    }
    s_wifi_up = true;
    if (s_stop_req) return;

    // FTM 报告事件处理(WIFI_EVENT_FTM_REPORT)。
    if (esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT,
                                            ftm_report_handler, NULL,
                                            &s_ftm_evt) == ESP_OK) {
        s_ftm_reg = true;
    }

    // 开启 CSI(C3 经典非 HE 配置)。
    wifi_csi_config_t cfg = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };
    esp_wifi_set_csi_config(&cfg);
    esp_wifi_set_csi_rx_cb(csi_cb, NULL);
    esp_wifi_set_csi(true);
    s_csi_on = true;

    // ping 目标:优先 menuconfig 指定,否则网关。
    memset(&s_ping_target, 0, sizeof(s_ping_target));
    s_ping_target_ok = false;
    const char *target_str = CONFIG_CSI_PING_TARGET;
    if (target_str[0] != '\0' && ipaddr_aton(target_str, &s_ping_target)) {
        s_ping_target_ok = true;
    } else if (gw != 0) {
        // 经指针变量调用,避免宏内部 if(&obj) 触发 -Werror=address。
        ip_addr_t *tp = &s_ping_target;
        ip_addr_set_ip4_u32(tp, gw);
        s_ping_target_ok = true;
    }
    start_ping();

    // 可选自动 FTM。
#if CONFIG_CSI_FTM_AUTO_INTERVAL_S > 0
    const esp_timer_create_args_t ftm_args = {
        .callback = ftm_auto_cb,
        .name = "csi_ftm_auto",
    };
    if (!s_ftm_auto_timer) esp_timer_create(&ftm_args, &s_ftm_auto_timer);
    if (s_ftm_auto_timer) {
        esp_timer_start_periodic(s_ftm_auto_timer,
            (uint64_t)CONFIG_CSI_FTM_AUTO_INTERVAL_S * 1000000ULL);
    }
#endif

    s_cap_state = CAP_CONNECTED;
}

static void capture_stop(void)
{
    if (!s_capturing) return;

    if (s_ftm_auto_timer) {
        esp_timer_stop(s_ftm_auto_timer);
    }
    if (s_csi_on) {
        esp_wifi_set_csi(false);
        esp_wifi_set_csi_rx_cb(NULL, NULL);
        s_csi_on = false;
    }
    stop_ping();
    esp_wifi_ftm_end_session();  // 结束可能在途的 FTM(无进行中也安全)
    if (s_ftm_reg) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_FTM_REPORT,
                                              s_ftm_evt);
        s_ftm_reg = false;
        s_ftm_evt = NULL;
    }
    if (s_wifi_up) {
        demo_radio_wifi_sta_stop();
        s_wifi_up = false;
    }
    s_ping_target_ok = false;
    s_capturing = false;
    s_cap_state = CAP_CONNECTING;
    s_ftm_valid = 0;
    s_ftm_cm = 0;
}

// 改 ping 间隔:重建会话(ping 无运行时改间隔的 API)。
static void update_ping_interval(int ms)
{
    s_ping_interval_ms = ms;
    if (s_capturing && s_cap_state == CAP_CONNECTED) {
        stop_ping();
        start_ping();
    }
}

// ===========================================================================
// 命令处理
// ===========================================================================
static void handle_cmd(const csi_cmd_t *cmd)
{
    switch (cmd->kind) {
    case CSI_CMD_START:   capture_start(); break;
    case CSI_CMD_STOP:    capture_stop(); break;
    case CSI_CMD_FTM:     trigger_ftm(); break;
    case CSI_CMD_PING_MS: update_ping_interval(cmd->v); break;
    default: break;
    }
}

// BLE CONTROL 回调(nimble host task):只投递,不阻塞。
static void on_ble_cmd(const csi_cmd_t *cmd)
{
    if (s_cmd_queue) {
        xQueueSend(s_cmd_queue, cmd, 0);
    }
}

// ===========================================================================
// 后台 task
// ===========================================================================
static void csi_worker(void *arg)
{
    (void)arg;
    demo_radio_nvs_prepare();  // BLE 与 WiFi 都依赖 NVS

    csi_ble_set_status_provider(fill_status);
    csi_ble_set_cmd_handler(on_ble_cmd);
    csi_ble_start();

    capture_start();  // 进入即自动开始采集

    // 命令循环:100ms 超时醒来以便响应 stop_req。
    while (!s_stop_req) {
        csi_cmd_t cmd;
        if (s_cmd_queue &&
            xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_stop_req) break;
            handle_cmd(&cmd);
        }
    }

    // 收场:先停采集(关 CSI/ping/FTM、停 WiFi),再停 BLE 广播。
    capture_stop();
    csi_ble_stop();

    s_worker_alive = false;
    vTaskDelete(NULL);
}

// ===========================================================================
// UI 刷新(LVGL task,已持锁)
// ===========================================================================
static void ui_tick(lv_timer_t *t)
{
    (void)t;
    int st = protocol_st();
    char ssid[33];
    get_ssid(ssid, sizeof(ssid));

    // 状态行:SSID + 连接态配色。
    uint32_t scol;
    switch (st) {
    case 0: lv_label_set_text(s_status, "IDLE"); scol = ECHO_MUTED; break;
    case 1: lv_label_set_text_fmt(s_status, "%s",
                ssid[0] ? ssid : "connecting..."); scol = ECHO_AMBER; break;
    case 2: lv_label_set_text_fmt(s_status, "%s", ssid); scol = ECHO_GREEN; break;
    default:
        lv_label_set_text(s_status,
            ssid[0] ? "connect failed" : "set WiFi in menuconfig");
        scol = ECHO_RED;
        break;
    }
    lv_obj_set_style_text_color(s_status, lv_color_hex(scol), 0);

    if (st == 2) lv_label_set_text_fmt(s_rssi_lbl, "%ddB", s_rssi);
    else         lv_label_set_text(s_rssi_lbl, "--");

    uint32_t now = lv_tick_get();
    uint32_t cnt = s_pkt_count;
    if (st == 2) {
        uint32_t dms = now - s_ui_last_ms;
        int pps = 0;
        if (dms > 0) {
            pps = (int)(((uint64_t)(cnt - s_ui_last_count) * 1000U) / dms);
        }
        s_rate_pps = pps;
        lv_label_set_text_fmt(s_rate, "%d/s", pps);
    } else {
        s_rate_pps = 0;
        lv_label_set_text(s_rate, "--");
    }
    s_ui_last_ms = now;
    s_ui_last_count = cnt;

    if (s_ftm_valid) {
        lv_label_set_text_fmt(s_dist, "%dcm", s_ftm_cm);
        lv_obj_set_style_text_color(s_dist, lv_color_hex(ECHO_GREEN), 0);
    } else {
        lv_label_set_text(s_dist, "N/A");
        lv_obj_set_style_text_color(s_dist, lv_color_hex(ECHO_RED), 0);
    }

    // 运动分半圆仪表 + 中央数字。
    int motion = s_motion;
    lv_arc_set_value(s_arc, motion);
    lv_obj_set_style_arc_color(s_arc,
        lv_color_hex(ui_echo_motion_color(motion)), LV_PART_INDICATOR);
    lv_label_set_text_fmt(s_motion_num, "%d", motion);

    // 子载波热力条:高度按归一化幅度、颜色按热力色阶。
    for (int b = 0; b < CSI_BARS; b++) {
        int lvl = s_bars[b];
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        lv_obj_set_height(s_heat[b], 3 + lvl * 25 / 100);
        lv_obj_align(s_heat[b], LV_ALIGN_BOTTOM_LEFT, b * 13, 0);
        lv_obj_set_style_bg_color(s_heat[b],
            lv_color_hex(ui_echo_heat_color(lvl)), 0);
    }
}

// ===========================================================================
// demo 接口
// ===========================================================================
void demo_csi_enter(void)
{
    s_stop_req = false;
    s_worker_alive = false;
    s_worker = NULL;
    s_wifi_up = false;
    s_csi_on = false;
    s_ping = NULL;
    s_ftm_reg = false;
    s_ftm_evt = NULL;
    s_capturing = false;
    s_cap_state = CAP_CONNECTING;
    s_motion = 0;
    s_rssi = 0;
    s_rate_pps = 0;
    s_pkt_count = 0;
    s_ftm_cm = 0;
    s_ftm_valid = 0;
    s_ui_last_ms = 0;
    s_ui_last_count = 0;
    s_ping_interval_ms = CONFIG_CSI_PING_INTERVAL_MS;
    set_ssid("");

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_cmd_queue) s_cmd_queue = xQueueCreate(CSI_CMD_QUEUE_DEPTH, sizeof(csi_cmd_t));
    if (s_cmd_queue) xQueueReset(s_cmd_queue);

    // enter 已在 LVGL task 上下文且已持锁,直接建屏。ECHO HUD 风格。
    s_scr = ui_echo_screen("ECHO");
    ui_echo_header_right(s_scr, "REC");

    // 状态行:SSID(左)+ RSSI(右)。
    lv_obj_t *stp = ui_echo_panel(s_scr, 8, 34, 224, 22);
    s_status = ui_echo_label(stp, "Starting...", &lv_font_montserrat_14, ECHO_MUTED);
    lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 0, 0);
    s_rssi_lbl = ui_echo_label(stp, "--", &lv_font_montserrat_14, ECHO_MUTED);
    lv_obj_align(s_rssi_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // 半圆运动分仪表(lv_arc:180..360 为上半圆)。
    lv_obj_t *mp = ui_echo_panel(s_scr, 8, 60, 224, 104);
    lv_obj_t *ml = ui_echo_label(mp, "MOTION", &lv_font_montserrat_14, ECHO_AMBER2);
    lv_obj_align(ml, LV_ALIGN_TOP_LEFT, 0, 0);

    s_arc = lv_arc_create(mp);
    lv_obj_set_size(s_arc, 150, 150);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 2);
    lv_arc_set_bg_angles(s_arc, 180, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(ECHO_ARCTRK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(ECHO_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_arc, 0, 0);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);  // 隐藏拖柄

    s_motion_num = ui_echo_label(mp, "0", &lv_font_montserrat_20, ECHO_TEXT);
    lv_obj_align(s_motion_num, LV_ALIGN_TOP_MID, 0, 50);

    // 子载波热力条(16 条,高度/颜色按幅度)。
    lv_obj_t *ap = ui_echo_panel(s_scr, 8, 168, 224, 52);
    lv_obj_t *al = ui_echo_label(ap, "CSI AMP", &lv_font_montserrat_14, ECHO_T_PC);
    lv_obj_align(al, LV_ALIGN_TOP_LEFT, 0, 0);
    for (int b = 0; b < CSI_BARS; b++) {
        lv_obj_t *bar = lv_obj_create(ap);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bar, 11, 4);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x1F6FE0), 0);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, b * 13, 0);
        s_heat[b] = bar;
    }

    // DIST / RATE 两个小面板。
    lv_obj_t *dp = ui_echo_panel(s_scr, 8, 224, 108, 28);
    lv_obj_t *dl = ui_echo_label(dp, "DIST", &lv_font_montserrat_14, ECHO_MUTED);
    lv_obj_align(dl, LV_ALIGN_LEFT_MID, 0, 0);
    s_dist = ui_echo_label(dp, "N/A", &lv_font_montserrat_14, ECHO_RED);
    lv_obj_align(s_dist, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *rp = ui_echo_panel(s_scr, 124, 224, 108, 28);
    lv_obj_t *rl = ui_echo_label(rp, "RATE", &lv_font_montserrat_14, ECHO_MUTED);
    lv_obj_align(rl, LV_ALIGN_LEFT_MID, 0, 0);
    s_rate = ui_echo_label(rp, "--", &lv_font_montserrat_14, ECHO_GREEN);
    lv_obj_align(s_rate, LV_ALIGN_RIGHT_MID, 0, 0);

    ui_tick(NULL);
    s_ui_timer = lv_timer_create(ui_tick, CSI_UI_REFRESH_MS, NULL);
    lv_screen_load(s_scr);

    s_worker_alive = true;
    if (xTaskCreate(csi_worker, "csi_worker", 4608, NULL, 5, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "后台任务创建失败");
        s_worker_alive = false;
        s_worker = NULL;
        s_capturing = true;
        s_cap_state = CAP_FAILED;
    }
}

void demo_csi_exit(void)
{
    // 先停 UI 定时器:此后没有东西再读/写 LVGL 对象。
    if (s_ui_timer) {
        lv_timer_delete(s_ui_timer);
        s_ui_timer = NULL;
    }

    // 请求后台收场并等待其完成(它负责按序关 CSI/ping/FTM/WiFi + 停 BLE 广播)。
    s_stop_req = true;
    for (int guard = 0; s_worker_alive && guard < 500; guard++) {
        vTaskDelay(pdMS_TO_TICKS(10));  // 最多约 5s 保险;常规远快于此
    }
    s_worker = NULL;

    // 后台已收场,最后删屏。
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = s_rssi_lbl = s_arc = s_motion_num = s_rate = s_dist = NULL;
        for (int b = 0; b < CSI_BARS; b++) s_heat[b] = NULL;
    }
}

void demo_csi_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    (void)ev;  // OK 长按返回由 main 统一拦截;本页无其它按键语义。
}
