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
#include "ui_pixel.h"

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
#define ROW_MAX         6       // 列表显示行数
#define CHAN_HOP_MS     250     // 信道轮询间隔
#define CHAN_MAX        13      // 2.4GHz 信道 1..13
#define DEV_STALE_MS    30000   // 超过此时长未见视为过期,不显示/可被 LRU 顶替
#define UI_REFRESH_MS   400

// 类型配色(与 ECHO 设计稿一致):PHONE 绿 / PC 蓝 / IOT 琥珀 / AP 紫 / ? 灰。
static uint32_t type_color(disco_type_t t)
{
    switch (t) {
    case DISCO_PHONE: return 0x4CAF50;
    case DISCO_PC:    return 0x2196F3;
    case DISCO_IOT:   return 0xFFB300;
    case DISCO_AP:    return 0x9C27B0;
    default:          return 0x9E9E9E;
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
static lv_obj_t   *s_counts;
static lv_obj_t   *s_rows[ROW_MAX];
static lv_obj_t   *s_badges[ROW_MAX];
static lv_obj_t   *s_labels[ROW_MAX];
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
        lv_label_set_text(s_counts, "Sniffer unavailable");
    } else {
        lv_label_set_text_fmt(s_counts, "P:%d C:%d I:%d A:%d  ch%d",
                              cnt[DISCO_PHONE], cnt[DISCO_PC],
                              cnt[DISCO_IOT], cnt[DISCO_AP], s_channel);
    }

    for (int r = 0; r < ROW_MAX; r++) {
        if (r < n) {
            lv_obj_remove_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_badges[r],
                lv_color_hex(type_color(snap[r].type)), 0);
            const uint8_t *m = snap[r].mac;
            char name[20];
            if (snap[r].vendor[0]) {
                snprintf(name, sizeof(name), "%s", snap[r].vendor);
            } else {
                snprintf(name, sizeof(name), "%02X:%02X:%02X", m[3], m[4], m[5]);
            }
            lv_label_set_text_fmt(s_labels[r], "%-12s %ddBm%s",
                                  name, snap[r].best, snap[r].rnd ? " ~rnd" : "");
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

    // enter 已在 LVGL task 上下文且已持锁,直接建屏。
    s_scr = ui_pixel_screen_create("RADAR");

    lv_obj_t *count_panel = ui_pixel_panel_create(s_scr, 16, 50, 208, 26, UI_YELLOW);
    s_counts = lv_label_create(count_panel);
    lv_obj_set_style_text_font(s_counts, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_counts, lv_color_hex(UI_INK), 0);
    lv_obj_center(s_counts);
    lv_label_set_text(s_counts, "scanning...");

    lv_obj_t *list = ui_pixel_panel_create(s_scr, 16, 84, 208, 192, UI_PAPER);
    for (int r = 0; r < ROW_MAX; r++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, 192, 28);
        lv_obj_set_pos(row, 0, r * 30);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(badge, 14, 14);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(badge, 3, 0);
        lv_obj_set_style_border_width(badge, 2, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x9E9E9E), 0);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_INK), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 22, 0);
        lv_label_set_text(label, "");

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

    // 最后删屏。
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_counts = NULL;
        for (int r = 0; r < ROW_MAX; r++) {
            s_rows[r] = s_badges[r] = s_labels[r] = NULL;
        }
    }
}

void demo_discovery_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    (void)ev;  // OK 长按返回由 main 统一拦截;本页无其它按键语义。
}
