// main/demo_settings.c —— 见 demo_settings.h。
//
// 三键导航:上/下移动选项;确定进入数值编辑(再用上/下加减、确定确认),或对
// 语言/告警直接切换、对重新标定触发动作。改动实时应用(echo_state / ui_i18n)
// 并存 NVS。文案走 i18n + 语言字体。
#include "demo.h"
#include "demo_settings.h"
#include "app_settings.h"
#include "echo_state.h"
#include "ui_echo.h"
#include "ui_i18n.h"

#include <stdio.h>

#include "bsp_display.h"   // 亮度实时调背光
#include "lvgl.h"

typedef enum {
    IT_LANG = 0,
    IT_OCC,
    IT_ALPHA,
    IT_SCALE,
    IT_ALERT,      // 告警模式 off/once/cont
    IT_VOLUME,
    IT_BRIGHT,
    IT_PING,
    IT_RECALIB,
    IT_COUNT,
} set_item_t;

static const ui_i18n_key_t ITEM_KEY[IT_COUNT] = {
    I18N_LANGUAGE, I18N_OCC_TH, I18N_SMOOTHING, I18N_SCALE,
    I18N_ALERT, I18N_VOLUME, I18N_BRIGHT, I18N_PING, I18N_RECALIB,
};

// 亮度下限(别调到全黑)。
#define SET_BRIGHT_MIN 10

static lv_obj_t *s_scr;
static lv_obj_t *s_row[IT_COUNT];
static lv_obj_t *s_name[IT_COUNT];
static lv_obj_t *s_val[IT_COUNT];
static lv_obj_t *s_hint;
static int       s_sel;
static bool      s_editing;

// 值 label 的字体:语言自称/告警模式含中文用语言字体,其余数值用 montserrat。
static const lv_font_t *val_font(int i)
{
    return (i == IT_LANG || i == IT_ALERT) ? ui_echo_font(false)
                                           : &lv_font_montserrat_14;
}

static void save_now(void)
{
    app_settings_t s;
    app_settings_capture(&s);
    app_settings_save(&s);
}

// 取某项的显示值到 buf。
static void fmt_value(set_item_t it, char *buf, size_t cap)
{
    float a, sc;
    switch (it) {
    case IT_LANG:
        snprintf(buf, cap, "%s", ui_i18n_t(I18N_LANG_SELF));
        break;
    case IT_OCC:
        snprintf(buf, cap, "%d", echo_state_get_occ_th());
        break;
    case IT_ALPHA:
        echo_state_get_sens(&a, &sc);
        snprintf(buf, cap, "0.%02d", (int)(a * 100.0f + 0.5f));
        break;
    case IT_SCALE:
        echo_state_get_sens(&a, &sc);
        snprintf(buf, cap, "%d", (int)(sc + 0.5f));
        break;
    case IT_ALERT: {
        int m = echo_state_get_alert_mode();
        snprintf(buf, cap, "%s",
                 m == 0 ? ui_i18n_t(I18N_OFF)
                 : m == 1 ? ui_i18n_t(I18N_ONCE) : ui_i18n_t(I18N_CONT));
        break;
    }
    case IT_VOLUME:
        snprintf(buf, cap, "%d", echo_state_get_volume());
        break;
    case IT_BRIGHT:
        snprintf(buf, cap, "%d", echo_state_get_brightness());
        break;
    case IT_PING:
        snprintf(buf, cap, "%d", echo_state_get_ping_ms());
        break;
    case IT_RECALIB:
        snprintf(buf, cap, ">");
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void refresh(void)
{
    char v[24];
    for (int i = 0; i < IT_COUNT; i++) {
        bool sel = (i == s_sel);
        lv_label_set_text(s_name[i], ui_i18n_t(ITEM_KEY[i]));
        fmt_value((set_item_t)i, v, sizeof(v));
        lv_label_set_text(s_val[i], v);

        uint32_t bg = sel ? ECHO_AMBER : ECHO_PANEL;
        uint32_t nm = sel ? ECHO_HDRTEXT : ECHO_TEXT;
        // 编辑态:数值用红色突出。
        uint32_t vl = (sel && s_editing) ? ECHO_RED
                      : (sel ? ECHO_HDRTEXT : ECHO_MUTED);
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(s_row[i],
            lv_color_hex(sel ? ECHO_AMBER : ECHO_STROKE), 0);
        lv_obj_set_style_border_width(s_row[i], sel ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_name[i], lv_color_hex(nm), 0);
        lv_obj_set_style_text_color(s_val[i], lv_color_hex(vl), 0);
    }
}

static void settings_build(void)
{
    s_scr = ui_echo_screen(ui_i18n_t(I18N_SETTINGS_TITLE));

    for (int i = 0; i < IT_COUNT; i++) {
        int y = ECHO_BODY_Y + i * 26;   // 9 项紧排
        s_row[i] = ui_echo_panel(s_scr, ECHO_SAFE, y, ECHO_BODY_W, 24);
        lv_obj_set_style_pad_all(s_row[i], 3, 0);

        s_name[i] = ui_echo_label(s_row[i], ui_i18n_t(ITEM_KEY[i]),
                                  ui_echo_font(false), ECHO_TEXT);
        lv_obj_align(s_name[i], LV_ALIGN_LEFT_MID, 0, 0);

        s_val[i] = ui_echo_label(s_row[i], "", val_font(i), ECHO_MUTED);
        lv_obj_align(s_val[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // 底部操作提示。
    s_hint = ui_echo_label(s_scr, ui_i18n_t(I18N_NAV_HINT),
                           ui_echo_font(false), ECHO_MUTED);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    refresh();
    lv_screen_load(s_scr);
}

// 切语言:就地更新现有屏的文字+字体,不删屏重建(LVGL9 下 load 新屏后同步删旧
// 活动屏会踩悬空引用导致卡死)。全程在 LVGL task、已持锁。
static void settings_relocalize(void)
{
    lv_obj_t *title = ui_echo_screen_title(s_scr);
    if (title) {
        lv_obj_set_style_text_font(title, ui_echo_font(true), 0);
        lv_label_set_text(title, ui_i18n_t(I18N_SETTINGS_TITLE));
    }
    for (int i = 0; i < IT_COUNT; i++) {
        lv_obj_set_style_text_font(s_name[i], ui_echo_font(false), 0);
        lv_obj_set_style_text_font(s_val[i], val_font(i), 0);
    }
    if (s_hint) {
        lv_obj_set_style_text_font(s_hint, ui_echo_font(false), 0);
        lv_label_set_text(s_hint, ui_i18n_t(I18N_NAV_HINT));
    }
    refresh();  // 重设各项文字(t(key))/值/选中态
}

// 编辑态下加减当前项(实时应用到 echo_state)。
static void adjust(int dir)
{
    float a, sc;
    switch (s_sel) {
    case IT_OCC: {
        int v = echo_state_get_occ_th() + dir;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        echo_state_set_occ_th(v);
        break;
    }
    case IT_ALPHA: {
        echo_state_get_sens(&a, &sc);
        a += dir * 0.05f;
        if (a < 0.05f) a = 0.05f;
        if (a > 0.95f) a = 0.95f;
        echo_state_set_sens(a, -1.0f);
        break;
    }
    case IT_SCALE: {
        echo_state_get_sens(&a, &sc);
        int v = (int)(sc + 0.5f) + dir;
        if (v < 1) v = 1;
        if (v > 50) v = 50;
        echo_state_set_sens(-1.0f, (float)v);
        break;
    }
    case IT_VOLUME: {
        int v = echo_state_get_volume() + dir * 10;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        echo_state_set_volume(v);
        break;
    }
    case IT_BRIGHT: {
        int v = echo_state_get_brightness() + dir * 10;
        if (v < SET_BRIGHT_MIN) v = SET_BRIGHT_MIN;
        if (v > 100) v = 100;
        echo_state_set_brightness(v);
        bsp_display_backlight((uint8_t)v);  // 实时调背光
        break;
    }
    case IT_PING: {
        int v = echo_state_get_ping_ms() + dir * 10;
        if (v < 20) v = 20;
        if (v > 2000) v = 2000;
        echo_state_set_ping_ms(v);
        break;
    }
    default: break;
    }
    refresh();
}

void demo_settings_enter(void)
{
    s_sel = 0;
    s_editing = false;
    settings_build();
}

void demo_settings_exit(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_hint = NULL;
        for (int i = 0; i < IT_COUNT; i++) {
            s_row[i] = s_name[i] = s_val[i] = NULL;
        }
    }
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;  // 仅用单击;OK 长按返回由 main 拦截

    if (s_editing) {
        if (btn == BSP_BTN_UP)        adjust(+1);
        else if (btn == BSP_BTN_DOWN) adjust(-1);
        else if (btn == BSP_BTN_OK) { s_editing = false; save_now(); refresh(); }
        return;
    }

    if (btn == BSP_BTN_UP)   { s_sel = (s_sel + IT_COUNT - 1) % IT_COUNT; refresh(); return; }
    if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % IT_COUNT;            refresh(); return; }
    if (btn != BSP_BTN_OK) return;

    switch (s_sel) {
    case IT_LANG:
        ui_i18n_set_lang(ui_i18n_get_lang() == UI_LANG_ZH ? UI_LANG_EN : UI_LANG_ZH);
        save_now();
        settings_relocalize();    // 就地换文字+字体,不删屏重建(防 UAF 卡死)
        break;
    case IT_ALERT:  // 循环 off -> once -> cont
        echo_state_set_alert_mode((echo_state_get_alert_mode() + 1) % 3);
        save_now();
        refresh();
        break;
    case IT_RECALIB:
        echo_state_request_mode("echo");  // 切到 ECHO,进去会自动校准(请离开)
        break;
    default:  // 数值项:进入编辑
        s_editing = true;
        refresh();
        break;
    }
}
