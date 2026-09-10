// main/demo_discovery.c —— 设备发现(RADAR)模式。
//
// 被动嗅探(不连接、不发射):WiFi 进混杂模式,esp_timer 每 ~250ms 在信道
// 1..13 间轮询;混杂 rx 回调(WiFi task,绝不碰 LVGL)解析 802.11 头取发射端
// MAC / 帧类型 / RSSI,写入一张有上限、带 LRU 老化的设备表(临界区保护);
// LVGL 定时器从表里渲染列表与计数。
//
// 生命周期(反复进出不崩,遵循 AGENTS):
//   enter(): 清表 → 建屏 + 渲染 lv_timer → 起后台 task 做 WiFi 混杂 bring-up。
//   exit():  先删渲染 timer → 置停止位等后台结束 → 停信道 timer、关混杂、
//            esp_wifi_stop/deinit → 最后删屏。
//
// 安全/诚实声明:现代设备 MAC 随机化普遍,随机 MAC 以 "~rnd" 标注;分类为粗猜。
// 屏上文案用 ASCII:内置 montserrat 字体无中文字形。
#include "demo.h"
#include "demo_discovery.h"
#include "demo_radio.h"
#include "disco_class.h"
#include "echo_state.h"
#include "ui_echo.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "demo_disco";

#define DEV_MAX         32      // 设备表上限
#define ROW_MAX         5       // 列表显示行数
#define CHAN_HOP_MS     250     // 信道轮询间隔
#define CHAN_MAX        13      // 2.4GHz 信道 1..13
#define DEV_STALE_MS    30000   // 超过此时长未见视为过期,不显示/可被 LRU 顶替
#define UI_REFRESH_MS   400

// 类型配色(对齐 ECHO 设计稿):PHONE 绿 / PC 蓝 / IOT 琥珀 / AP 紫 / ? 灰。
static uint32_t type_color(disco_type_t t)
{
    switch (t) {
    case DISCO_PHONE: return ECHO_T_PHONE;
    case DISCO_PC:    return ECHO_T_PC;
    case DISCO_IOT:   return ECHO_T_IOT;
    case DISCO_AP:    return ECHO_T_AP;
    default:          return 0x9E9E9E;
    }
}

// 类型的 2 字母徽标码。
static const char *type_code(disco_type_t t)
{
    switch (t) {
    case DISCO_PHONE: return "PH";
    case DISCO_PC:    return "PC";
    case DISCO_IOT:   return "IO";
    case DISCO_AP:    return "AP";
    default:          return "??";
    }
}

// ---------------------------------------------------------------------------
// 设备表(rx 回调写、渲染读;临界区保护)
// ---------------------------------------------------------------------------
typedef struct {
    bool         used;
    uint8_t      mac[6];
    int8_t       last_rssi;
    int8_t       best_rssi;
    uint32_t     count;
    uint32_t     last_ms;
    bool         is_ap;
    bool         randomized;
    disco_type_t type;
} dev_entry_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static dev_entry_t  s_table[DEV_MAX];

// --- LVGL 对象(仅 LVGL task)---
static lv_obj_t   *s_scr;
static lv_obj_t   *s_total;              // header 右侧设备总数
static lv_obj_t   *s_cnt[5];             // PHONE/PC/IOT/AP/? 计数标签
static lv_obj_t   *s_rows[ROW_MAX];
static lv_obj_t   *s_badges[ROW_MAX];    // 类型色块徽标
static lv_obj_t   *s_labels[ROW_MAX];    // 厂商/MAC + ~rnd
static lv_obj_t   *s_rbar[ROW_MAX][3];   // 每行 RSSI 迷你条
static lv_timer_t *s_ui_timer;

// --- 后台 / 资源 ---
static TaskHandle_t      s_worker;
static volatile bool     s_stop;
static volatile bool     s_worker_alive;
static volatile bool     s_sniffing;
static volatile bool     s_failed;
static bool              s_wifi_inited;
static bool              s_promisc_on;
static esp_timer_handle_t s_chan_timer;
static volatile int      s_channel = 1;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
// 混杂 rx 回调(WiFi task)—— 绝不碰 LVGL
// ---------------------------------------------------------------------------
static void table_update(const uint8_t *mac, int8_t rssi, bool is_ap)
{
    uint32_t t = now_ms();
    portENTER_CRITICAL(&s_mux);

    int slot = -1;
    for (int i = 0; i < DEV_MAX; i++) {
        if (s_table[i].used && memcmp(s_table[i].mac, mac, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // 找空位;没有则 LRU(最久未见)顶替。
        uint32_t oldest = 0xFFFFFFFFu;
        int lru = 0;
        for (int i = 0; i < DEV_MAX; i++) {
            if (!s_table[i].used) { slot = i; break; }
            if (s_table[i].last_ms < oldest) { oldest = s_table[i].last_ms; lru = i; }
        }
        if (slot < 0) slot = lru;
        memset(&s_table[slot], 0, sizeof(s_table[slot]));
        memcpy(s_table[slot].mac, mac, 6);
        s_table[slot].used = true;
        s_table[slot].best_rssi = rssi;
        s_table[slot].randomized = disco_is_randomized(mac);
    }

    dev_entry_t *e = &s_table[slot];
    e->last_rssi = rssi;
    if (rssi > e->best_rssi) e->best_rssi = rssi;
    e->count++;
    e->last_ms = t;
    if (is_ap) e->is_ap = true;
    e->type = disco_classify(e->mac, e->is_ap);

    portEXIT_CRITICAL(&s_mux);
}

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (s_stop || !s_sniffing) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (!pkt || pkt->rx_ctrl.sig_len < 16) return;  // 需至少到 addr2

    const uint8_t *p = pkt->payload;
    uint8_t ft = (uint8_t)((p[0] >> 2) & 0x3);   // type:0=mgmt,2=data
    uint8_t st = (uint8_t)((p[0] >> 4) & 0xF);   // subtype
    // beacon(mgmt/8)或 probe-response(mgmt/5)→ 可靠判定为 AP。
    bool is_ap = (ft == 0 && (st == 8 || st == 5));
    const uint8_t *addr2 = p + 10;               // 发射端 MAC(TA)
    table_update(addr2, pkt->rx_ctrl.rssi, is_ap);
}

// ---------------------------------------------------------------------------
// 信道轮询(esp_timer task)
// ---------------------------------------------------------------------------
static void chan_hop_cb(void *arg)
{
    (void)arg;
    if (s_stop) return;
    s_channel = (s_channel % CHAN_MAX) + 1;
    esp_wifi_set_channel((uint8_t)s_channel, WIFI_SECOND_CHAN_NONE);  // 失败忽略
}

// ---------------------------------------------------------------------------
// 后台 bring-up
// ---------------------------------------------------------------------------
static void disco_worker(void *arg)
{
    (void)arg;
    bool ok = true;
    if (demo_radio_nvs_prepare() != ESP_OK) ok = false;
    if (ok && demo_radio_network_prepare() != ESP_OK) ok = false;

    if (ok) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) ok = false;
        else s_wifi_inited = true;
    }
    if (ok && esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) ok = false;
    if (ok && esp_wifi_start() != ESP_OK) ok = false;

    if (ok) {
        wifi_promiscuous_filter_t filter = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
        };
        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(sniffer_cb);
        if (esp_wifi_set_promiscuous(true) != ESP_OK) ok = false;
        else s_promisc_on = true;
    }

    if (ok) {
        s_channel = 1;
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        const esp_timer_create_args_t targs = {
            .callback = chan_hop_cb,
            .name = "disco_chan",
        };
        if (!s_chan_timer) esp_timer_create(&targs, &s_chan_timer);
        if (s_chan_timer) esp_timer_start_periodic(s_chan_timer, CHAN_HOP_MS * 1000);
        s_sniffing = true;
        ESP_LOGI(TAG, "混杂嗅探已启动");
    } else {
        s_failed = true;
        ESP_LOGE(TAG, "嗅探 bring-up 失败");
    }

    s_worker_alive = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 渲染(LVGL task,已持锁)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t      mac[6];
    int8_t       best;
    bool         rnd;
    disco_type_t type;
    const char  *vendor;
} snap_t;

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    snap_t snap[DEV_MAX];
    int n = 0;
    int cnt[5] = { 0, 0, 0, 0, 0 };  // 按 disco_type_t 下标计数
    uint32_t now = now_ms();

    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < DEV_MAX; i++) {
        if (!s_table[i].used) continue;
        if (now - s_table[i].last_ms > DEV_STALE_MS) continue;  // 过期不计
        snap[n].best = s_table[i].best_rssi;
        snap[n].rnd = s_table[i].randomized;
        snap[n].type = s_table[i].type;
        memcpy(snap[n].mac, s_table[i].mac, 6);
        n++;
    }
    portEXIT_CRITICAL(&s_mux);

    // 临界区外再查厂商(纯函数,避免拉长临界区),并统计类型。
    for (int i = 0; i < n; i++) {
        snap[i].vendor = disco_vendor(snap[i].mac);
        cnt[snap[i].type]++;
    }

    // 按最强 RSSI 降序(插入排序,n<=32)。
    for (int i = 1; i < n; i++) {
        snap_t key = snap[i];
        int j = i - 1;
        while (j >= 0 && snap[j].best < key.best) {
            snap[j + 1] = snap[j];
            j--;
        }
        snap[j + 1] = key;
    }

    if (s_failed) {
        lv_label_set_text(s_total, "ERR");
    } else {
        lv_label_set_text_fmt(s_total, "x%d ch%d", n, s_channel);
    }
    lv_label_set_text_fmt(s_cnt[0], "%s  %d", ui_i18n_t(I18N_PHONE), cnt[DISCO_PHONE]);
    lv_label_set_text_fmt(s_cnt[1], "%s  %d", ui_i18n_t(I18N_PC), cnt[DISCO_PC]);
    lv_label_set_text_fmt(s_cnt[2], "%s  %d", ui_i18n_t(I18N_IOT), cnt[DISCO_IOT]);
    lv_label_set_text_fmt(s_cnt[3], "%s  %d", ui_i18n_t(I18N_AP), cnt[DISCO_AP]);
    // 随机 MAC 的 STA 多被归为 UNKNOWN,单列一项以免"消失"。
    lv_label_set_text_fmt(s_cnt[4], "%s  %d", ui_i18n_t(I18N_UNKNOWN), cnt[DISCO_UNKNOWN]);

    // 写入共享状态,供常驻 BLE 的 STATUS 通知(radar 形态)。
    int st = s_failed ? 3 : (s_sniffing ? 2 : 1);
    echo_state_set_radar(st, 0, cnt[DISCO_PHONE], cnt[DISCO_PC],
                         cnt[DISCO_IOT], cnt[DISCO_AP], cnt[DISCO_UNKNOWN], n);

    for (int r = 0; r < ROW_MAX; r++) {
        if (r < n) {
            uint32_t col = type_color(snap[r].type);
            lv_obj_remove_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
            // 徽标:类型色块 + 2 字母码。
            lv_obj_set_style_bg_color(s_badges[r], lv_color_hex(col), 0);
            lv_label_set_text(s_badges[r], type_code(snap[r].type));
            // 名称:厂商或短 MAC + 随机标记。
            const uint8_t *m = snap[r].mac;
            char name[24];
            if (snap[r].vendor[0]) {
                snprintf(name, sizeof(name), "%s", snap[r].vendor);
            } else {
                snprintf(name, sizeof(name), "%02X:%02X:%02X", m[3], m[4], m[5]);
            }
            lv_label_set_text_fmt(s_labels[r], "%s%s",
                                  name, snap[r].rnd ? " ~rnd" : "");
            // RSSI 迷你条:按强度填充 1..3 段。
            int filled = (snap[r].best > -55) ? 3 : (snap[r].best > -70) ? 2 : 1;
            for (int k = 0; k < 3; k++) {
                lv_obj_set_style_bg_color(s_rbar[r][k],
                    lv_color_hex(k < filled ? col : ECHO_ARCTRK), 0);
            }
        } else {
            lv_obj_add_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// demo 接口
// ---------------------------------------------------------------------------
void demo_discovery_enter(void)
{
    s_stop = false;
    s_worker_alive = false;
    s_worker = NULL;
    s_sniffing = false;
    s_failed = false;
    s_wifi_inited = false;
    s_promisc_on = false;
    s_channel = 1;

    // 清表(此时尚无并发访问)。
    portENTER_CRITICAL(&s_mux);
    memset(s_table, 0, sizeof(s_table));
    portEXIT_CRITICAL(&s_mux);

    // enter 已在 LVGL task 上下文且已持锁,直接建屏。ECHO HUD 风格。
    echo_state_set_mode("radar");
    s_scr = ui_echo_screen("RADAR");
    s_total = ui_echo_header_right(s_scr, "x0");

    // 左上角装饰小雷达(同心圈 + blip;扫描线 LVGL 做不动,省略)。
    lv_obj_t *radar = ui_echo_panel(s_scr, ECHO_SAFE, ECHO_BODY_Y, 84, 84);
    lv_obj_set_style_pad_all(radar, 0, 0);
    const int rings[3] = { 78, 50, 24 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(radar);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(ring, rings[i], rings[i]);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(ECHO_STROKE), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
    }
    // 几个静态 blip(装饰,非实时数据)。
    const int blip[4][3] = {  // x, y 偏移, 颜色下标
        { 16, -20 }, { -18, 12 }, { 8, 22 }, { -24, -10 } };
    const uint32_t blipc[4] = { ECHO_T_PHONE, ECHO_T_PC, ECHO_T_IOT, ECHO_T_AP };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(radar);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(b, 6, 6);
        lv_obj_align(b, LV_ALIGN_CENTER, blip[i][0], blip[i][1]);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(blipc[i]), 0);
    }

    // 右侧类型计数(颜色即类型)+ UNKNOWN。
    lv_obj_t *cp = ui_echo_panel(s_scr, 100, ECHO_BODY_Y, 130, 84);
    const uint32_t cc[5] = { ECHO_T_PHONE, ECHO_T_PC, ECHO_T_IOT, ECHO_T_AP, 0x9E9E9E };
    for (int i = 0; i < 5; i++) {
        s_cnt[i] = ui_echo_label(cp, "", ui_echo_font(false), cc[i]);
        lv_obj_align(s_cnt[i], LV_ALIGN_TOP_LEFT, 0, i * 15);
    }

    // 设备列表行(各自为 HUD 面板)。
    for (int r = 0; r < ROW_MAX; r++) {
        lv_obj_t *row = ui_echo_panel(s_scr, ECHO_SAFE, 130 + r * 34, ECHO_BODY_W, 30);
        lv_obj_set_style_pad_all(row, 3, 0);

        // 徽标 = 带底色的 label(2 字母)。
        lv_obj_t *badge = lv_label_create(row);
        lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(badge, lv_color_hex(ECHO_HDRTEXT), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x9E9E9E), 0);
        lv_obj_set_style_radius(badge, 2, 0);
        lv_obj_set_style_pad_left(badge, 3, 0);
        lv_obj_set_style_pad_right(badge, 3, 0);
        lv_obj_set_style_pad_top(badge, 1, 0);
        lv_obj_set_style_pad_bottom(badge, 1, 0);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_text(badge, "??");

        lv_obj_t *label = ui_echo_label(row, "", &lv_font_montserrat_14, ECHO_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 34, 0);

        // RSSI 三段迷你条(高度 4/7/10,共底对齐)。
        const int bh[3] = { 4, 7, 10 };
        const int bx[3] = { -14, -9, -4 };
        for (int k = 0; k < 3; k++) {
            lv_obj_t *b = lv_obj_create(row);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(b, 3, bh[k]);
            lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, bx[k], -2);
            lv_obj_set_style_radius(b, 0, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(ECHO_ARCTRK), 0);
            s_rbar[r][k] = b;
        }

        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        s_rows[r] = row;
        s_badges[r] = badge;
        s_labels[r] = label;
    }

    ui_tick(NULL);
    s_ui_timer = lv_timer_create(ui_tick, UI_REFRESH_MS, NULL);
    lv_screen_load(s_scr);

    s_worker_alive = true;
    if (xTaskCreate(disco_worker, "disco_worker", 4096, NULL, 5, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "后台任务创建失败");
        s_worker_alive = false;
        s_worker = NULL;
        s_failed = true;
    }
}

void demo_discovery_exit(void)
{
    // 先停渲染定时器:此后无人再读/写 LVGL 对象。
    if (s_ui_timer) {
        lv_timer_delete(s_ui_timer);
        s_ui_timer = NULL;
    }

    // 停止嗅探路径,等后台 bring-up 结束(它很快自删)。
    s_stop = true;
    s_sniffing = false;
    for (int guard = 0; s_worker_alive && guard < 300; guard++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_worker = NULL;

    // 按 AGENTS 顺序拆除:停信道 timer → 关混杂/注销回调 → 停/deinit WiFi。
    if (s_chan_timer) {
        esp_timer_stop(s_chan_timer);
        esp_timer_delete(s_chan_timer);
        s_chan_timer = NULL;
    }
    if (s_promisc_on) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        s_promisc_on = false;
    }
    if (s_wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_inited = false;
    }

    echo_state_set_idle();  // STATUS 的 st 归 0,保留 mode

    // 最后删屏。
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_total = NULL;
        for (int i = 0; i < 5; i++) s_cnt[i] = NULL;
        for (int r = 0; r < ROW_MAX; r++) {
            s_rows[r] = s_badges[r] = s_labels[r] = NULL;
            for (int k = 0; k < 3; k++) s_rbar[r][k] = NULL;
        }
    }
}

void demo_discovery_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    (void)ev;  // OK 长按返回由 main 统一拦截;本页无其它按键语义。
}
