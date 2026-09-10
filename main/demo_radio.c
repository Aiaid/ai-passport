// main/demo_radio.c —— 见 demo_radio.h。
#include "demo_radio.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "demo_radio";

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;

esp_err_t demo_radio_nvs_prepare(void)
{
    if (s_nvs_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // 示例不能为了启动无线功能而擦除未来应用可能已经保存的数据。
        ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除分区", esp_err_to_name(err));
        return err;
    }
    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t demo_radio_network_prepare(void)
{
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) return err;
        s_event_loop_ready = true;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// STA 连接 helper
// ---------------------------------------------------------------------------

#define RADIO_MAX_RETRY 5  // 断连后重试连接次数,超过则判定失败

static esp_netif_t               *s_sta_netif;
static esp_event_handler_instance_t s_wifi_evt;
static esp_event_handler_instance_t s_ip_evt;
static SemaphoreHandle_t          s_conn_sem;   // 连上拿到 IP 或彻底失败时 give
static volatile bool              s_got_ip;
static volatile bool              s_conn_failed;
static volatile uint32_t          s_gw_addr;
static int                        s_retry;
static bool                       s_wifi_started;  // esp_wifi_start 是否已执行

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < RADIO_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "STA 断连,重试连接 %d/%d", s_retry, RADIO_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "STA 连接失败:重试已耗尽");
            s_conn_failed = true;
            if (s_conn_sem) xSemaphoreGive(s_conn_sem);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_gw_addr = event->ip_info.gw.addr;
        s_retry = 0;
        s_got_ip = true;
        ESP_LOGI(TAG, "STA 已拿到 IP,网关 " IPSTR, IP2STR(&event->ip_info.gw));
        if (s_conn_sem) xSemaphoreGive(s_conn_sem);
    }
}

// 回滚 demo_radio_wifi_sta_start 已经做过的各步(按逆序),用于失败或 stop。
static void radio_teardown(void)
{
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_evt) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt);
        s_wifi_evt = NULL;
    }
    if (s_ip_evt) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt);
        s_ip_evt = NULL;
    }
    esp_wifi_deinit();
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_conn_sem) {
        vSemaphoreDelete(s_conn_sem);
        s_conn_sem = NULL;
    }
}

esp_err_t demo_radio_wifi_sta_start(const char *ssid, const char *password,
                                    int timeout_ms, volatile bool *abort_flag,
                                    uint32_t *gw_addr_out)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;

    s_got_ip = false;
    s_conn_failed = false;
    s_gw_addr = 0;
    s_retry = 0;
    s_wifi_started = false;

    s_conn_sem = xSemaphoreCreateBinary();
    if (!s_conn_sem) return ESP_ERR_NO_MEM;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) { radio_teardown(); return ESP_FAIL; }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) { radio_teardown(); return err; }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, &s_wifi_evt);
    if (err != ESP_OK) { radio_teardown(); return err; }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, &s_ip_evt);
    if (err != ESP_OK) { radio_teardown(); return err; }

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) { radio_teardown(); return err; }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) { radio_teardown(); return err; }
    err = esp_wifi_start();
    if (err != ESP_OK) { radio_teardown(); return err; }
    s_wifi_started = true;

    // 分片等待拿到 IP:每 100ms 醒一次检查中止标志,避免退出时长时间阻塞。
    const TickType_t step = pdMS_TO_TICKS(100);
    int waited = 0;
    while (waited < timeout_ms) {
        if (xSemaphoreTake(s_conn_sem, step) == pdTRUE) {
            break;  // 要么拿到 IP,要么彻底失败
        }
        if (abort_flag && *abort_flag) {
            ESP_LOGW(TAG, "连接被中止");
            radio_teardown();
            return ESP_ERR_TIMEOUT;
        }
        waited += 100;
    }

    if (!s_got_ip) {
        ESP_LOGE(TAG, "STA 连接超时/失败");
        radio_teardown();
        return ESP_ERR_TIMEOUT;
    }

    if (gw_addr_out) *gw_addr_out = s_gw_addr;
    // 成功:保留 netif/事件/Wi-Fi,等调用方 demo_radio_wifi_sta_stop 清理。
    // 连接信号量已用完,删掉以免泄漏(后续断连重试不再依赖它)。
    if (s_conn_sem) { vSemaphoreDelete(s_conn_sem); s_conn_sem = NULL; }
    return ESP_OK;
}

void demo_radio_wifi_sta_stop(void)
{
    radio_teardown();
}
