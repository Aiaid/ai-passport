// main/main.c —— FoloToy AI Passport · ECHO:初始化 + 菜单 + 按键分发。
//
// 菜单两项感知设备:ECHO(被动 WiFi CSI 在场/运动)+ RADAR(被动设备发现)。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "demo_radio.h"    // demo_radio_nvs_prepare(供常驻 BLE 先备好 NVS)
#include "echo_state.h"
#include "csi_ble.h"
#include "ui_echo.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { "ECHO",  demo_csi_enter,       demo_csi_exit,       demo_csi_key       },
    { "RADAR", demo_discovery_enter, demo_discovery_exit, demo_discovery_key },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

// 每项的副标题(菜单卡片上的一行说明)。
static const char *DEMO_DESC[DEMO_COUNT] = {
    "WiFi CSI motion sense",
    "Passive device scan",
};

// 各项是否可进入(依赖按键初始化)。
static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];   // 标题
static lv_obj_t *s_desc[DEMO_COUNT];   // 副标题
static int  s_sel;                 // 当前选中项
static int  s_active = -1;         // 当前所在演示页;-1 = 在菜单

// 按键事件队列:按键回调只投递,真正的分发在 LVGL 任务里做(见 key_timer_cb)。
typedef struct {
    bsp_btn_t    btn;
    bsp_btn_ev_t ev;
} key_event_t;

#define KEY_QUEUE_DEPTH 8
#define KEY_DRAIN_MS    20

static QueueHandle_t s_key_queue;
static lv_timer_t   *s_key_timer;

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        bool sel = ((int)i == s_sel);
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name, s_ok[i] ? "" : " [X]");
        // 选中:琥珀底 + 深色字;失败:红字;常规:面板底 + 亮字。
        uint32_t bg = sel ? ECHO_AMBER : ECHO_PANEL;
        uint32_t bd = sel ? ECHO_AMBER : ECHO_STROKE;
        uint32_t tx = !s_ok[i] ? ECHO_RED : (sel ? ECHO_HDRTEXT : ECHO_TEXT);
        uint32_t dx = sel ? ECHO_HDRTEXT : ECHO_MUTED;
        lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(s_cards[i], lv_color_hex(bd), 0);
        lv_obj_set_style_border_width(s_cards[i], sel ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_rows[i], lv_color_hex(tx), 0);
        lv_obj_set_style_text_color(s_desc[i], lv_color_hex(dx), 0);
    }
}

static void menu_build(void) {
    s_menu_scr = ui_echo_screen("ECHO");

    lv_obj_t *sub = ui_echo_label(s_menu_scr, "PASSIVE WIFI ECHOLOCATION",
                                  &lv_font_montserrat_14, ECHO_MUTED);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, ECHO_SAFE + 2, ECHO_BODY_Y);

    // 两张大卡片竖排(安全区内)。
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int y = 72 + (int)i * 88;
        s_cards[i] = ui_echo_panel(s_menu_scr, ECHO_SAFE, y, ECHO_BODY_W, 76);
        s_rows[i] = ui_echo_label(s_cards[i], DEMOS[i].name,
                                  &lv_font_montserrat_20, ECHO_TEXT);
        lv_obj_align(s_rows[i], LV_ALIGN_TOP_LEFT, 4, 10);
        s_desc[i] = ui_echo_label(s_cards[i], DEMO_DESC[i],
                                  &lv_font_montserrat_14, ECHO_MUTED);
        lv_obj_align(s_desc[i], LV_ALIGN_BOTTOM_LEFT, 4, -10);
    }

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    menu_build();
}

// 真正的分发。只在 LVGL 任务里被调用(key_timer_cb),此时锁已由 LVGL 任务持有,
// 不得再调 bsp_lvgl_lock。
static void dispatch_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {     // 统一返回
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)   { s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % DEMO_COUNT;              menu_refresh(); }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            DEMOS[s_active].enter();
        }
    }
}

// 切到指定 demo(本地按键或 BLE 远程请求共用)。只在 LVGL 任务里调用(已持锁)。
static void switch_to(int idx) {
    if (idx < 0 || idx >= (int)DEMO_COUNT) return;
    if (s_active == idx) return;             // 已在目标模式
    if (s_active >= 0) {
        DEMOS[s_active].exit();
    } else if (s_menu_scr) {
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
    }
    s_active = idx;
    s_sel = idx;
    DEMOS[idx].enter();
}

// 处理 BLE 下发的远程切模式请求(在 LVGL 任务里安全地退出/进入 demo)。
static void handle_mode_request(void) {
    char req[8];
    if (!echo_state_take_mode_request(req, sizeof(req))) return;
    if (strcmp(req, "echo") == 0)       switch_to(0);
    else if (strcmp(req, "radar") == 0) switch_to(1);
}

// 跑在 LVGL 任务里(已持锁),每 20ms 把队列里攒下的按键一次性处理完。
static void key_timer_cb(lv_timer_t *timer) {
    (void)timer;
    key_event_t event;
    while (s_key_queue && xQueueReceive(s_key_queue, &event, 0) == pdTRUE) {
        // 菜单尚未建好(app_main 还卡在外设初始化上)时丢弃事件,
        // 否则会对还是 NULL 的 s_rows[] 调 LVGL API。
        if (s_active < 0 && !s_menu_scr) continue;
        dispatch_key(event.btn, event.ev);
    }
    handle_mode_request();  // BLE 远程切模式
}

// 按键回调运行在 esp_timer 任务里 —— iot_button 的扫描和 esp_lvgl_port 的
// lv_tick_inc 共用这一个任务,所以这里绝不能阻塞等 LVGL 锁。
// 这里只做一次非阻塞投递,分发交给 LVGL 任务里的 key_timer_cb。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_key_queue) return;
    key_event_t event = { .btn = btn, .ev = ev };
    if (xQueueSend(s_key_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "按键队列已满,丢弃 btn=%d ev=%d", btn, ev);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport · ECHO 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本固件的 UI 载体,失败就没有菜单可言 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,固件无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 队列必须早于 bsp_button_init 建好:回调一旦武装,按键随时可能进来。
    s_key_queue = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(key_event_t));
    if (!s_key_queue) {
        ESP_LOGE(TAG, "按键队列创建失败,按键将不可用");
    }

    bool button_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[0] = button_ok;      // ECHO:可进入,网络失败在屏上报
    s_ok[1] = button_ok;      // RADAR:可进入,失败在屏上报

    // 共享状态 + 常驻 BLE:开机即起,独立于具体 demo,面板随时可连/看状态/远程切模式。
    echo_state_init();
    demo_radio_nvs_prepare();   // BLE 控制器需要 NVS 就绪
    if (csi_ble_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败;本地功能仍可用");
    }

    if (bsp_lvgl_lock(1000)) {
        enter_menu();
        if (s_key_queue) xQueueReset(s_key_queue);
        s_key_timer = lv_timer_create(key_timer_cb, KEY_DRAIN_MS, NULL);
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪:Display=1 Button=%d", button_ok);
}
