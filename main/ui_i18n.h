// main/ui_i18n.h —— 设备端中英双语字符串。纯 C(不含 lvgl),可主机单测。
// 动态数据(SSID/数值/MAC/厂商)不走这里,只翻译静态可见文案。
#pragma once

typedef enum {
    UI_LANG_EN = 0,
    UI_LANG_ZH = 1,
} ui_lang_t;

typedef enum {
    // 菜单
    I18N_SUBTITLE = 0,
    I18N_DESC_ECHO,
    I18N_DESC_RADAR,
    I18N_CARD_SETTINGS,
    // ECHO 屏
    I18N_IDLE,
    I18N_CONNECTING,
    I18N_FAILED,
    I18N_NOWIFI,
    I18N_MOTION,
    I18N_CSIAMP,
    I18N_DIST,
    I18N_RATE,
    I18N_OCCUPIED,
    I18N_EMPTY,
    I18N_CALIBRATING,
    I18N_LEAVE,
    I18N_LINK,
    I18N_NA,
    // RADAR 屏(类型计数名)
    I18N_PHONE,
    I18N_PC,
    I18N_IOT,
    I18N_AP,
    I18N_UNKNOWN,
    // 设置屏
    I18N_SETTINGS_TITLE,
    I18N_LANGUAGE,
    I18N_OCC_TH,
    I18N_SMOOTHING,
    I18N_SCALE,
    I18N_ALERT,
    I18N_RECALIB,
    I18N_PING,
    I18N_ON,
    I18N_OFF,
    I18N_ONCE,
    I18N_CONT,
    I18N_VOLUME,
    I18N_BRIGHT,
    I18N_NAV_HINT,
    I18N_LANG_SELF,   // 当前语言的自称:"EN" / "中文"
    I18N_KEY_COUNT,
} ui_i18n_key_t;

void       ui_i18n_set_lang(ui_lang_t lang);
ui_lang_t  ui_i18n_get_lang(void);

// 按当前语言返回 key 对应字符串(永不返回 NULL)。
const char *ui_i18n_t(ui_i18n_key_t key);
