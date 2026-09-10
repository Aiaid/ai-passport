// main/app_settings.c —— 见 app_settings.h。
#include "app_settings.h"
#include "echo_state.h"
#include "ui_i18n.h"
#include "bsp_display.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_settings";
#define NS   "echo"     // NVS 命名空间(系统 nvs 分区)
#define KEY  "cfg"      // 单个 blob 存整个结构

void app_settings_defaults(app_settings_t *s)
{
    if (!s) return;
    s->language = UI_LANG_ZH;   // 默认中文
    s->occ_th = 25;
    s->alpha = 0.2f;
    s->scale = 8.0f;
    s->alert_mode = 1;          // once
    s->volume = 70;
    s->brightness = 100;
    s->ping_ms = 100;
}

esp_err_t app_settings_load(app_settings_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    app_settings_defaults(s);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;

    app_settings_t tmp;
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, KEY, &tmp, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(tmp)) {
        *s = tmp;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;  // 保持默认
}

esp_err_t app_settings_save(const app_settings_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, KEY, s, sizeof(*s));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void app_settings_apply(const app_settings_t *s)
{
    if (!s) return;
    ui_i18n_set_lang(s->language == UI_LANG_EN ? UI_LANG_EN : UI_LANG_ZH);
    echo_state_set_occ_th(s->occ_th);
    echo_state_set_sens(s->alpha, s->scale);
    echo_state_set_alert_mode(s->alert_mode);
    echo_state_set_volume(s->volume);
    int b = s->brightness < 10 ? 10 : s->brightness;  // 下限,别全黑
    echo_state_set_brightness(b);
    bsp_display_backlight((uint8_t)b);
    echo_state_set_ping_ms(s->ping_ms);
}

void app_settings_capture(app_settings_t *s)
{
    if (!s) return;
    s->language = (int)ui_i18n_get_lang();
    s->occ_th = echo_state_get_occ_th();
    echo_state_get_sens(&s->alpha, &s->scale);
    s->alert_mode = echo_state_get_alert_mode();
    s->volume = echo_state_get_volume();
    s->brightness = echo_state_get_brightness();
    s->ping_ms = echo_state_get_ping_ms();
}
