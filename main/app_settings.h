// main/app_settings.h —— 设置持久化:存取 NVS(用系统 nvs 分区,不动受保护的 cardid)。
// 字段为 language/occ_th/alpha/scale/alert_enabled/ping_ms。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    int   language;       // 0 = EN, 1 = ZH
    int   occ_th;
    float alpha;
    float scale;
    int   alert_mode;     // 0 off / 1 once / 2 cont
    int   volume;         // 0..100
    int   brightness;     // 0..100
    int   ping_ms;
} app_settings_t;

// 填入默认值(中文、occ_th=25、alpha=0.2、scale=8、告警开、ping=100)。
void app_settings_defaults(app_settings_t *s);

// 从 NVS 读取;无存档时填默认并返回 ESP_ERR_NVS_NOT_FOUND(仍可直接使用)。
esp_err_t app_settings_load(app_settings_t *s);

// 写入 NVS。
esp_err_t app_settings_save(const app_settings_t *s);

// 把一份设置应用到运行态(ui_i18n 语言 + echo_state 参数)。
void app_settings_apply(const app_settings_t *s);

// 从当前运行态(ui_i18n + echo_state)快照出一份设置(供保存)。
void app_settings_capture(app_settings_t *s);
