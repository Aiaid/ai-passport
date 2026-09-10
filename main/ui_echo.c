// main/ui_echo.c —— 见 ui_echo.h。
#include "ui_echo.h"

lv_obj_t *ui_echo_screen(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(ECHO_BG), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(ECHO_BG2), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 顶部琥珀 header(设计稿的斜切角 LVGL 不原生,简化为直角条)。
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, 240, 30);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(ECHO_AMBER), 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_color_hex(ECHO_AMBER2), 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_HOR, 0);

    lv_obj_t *t = lv_label_create(hdr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(ECHO_HDRTEXT), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);
    return scr;
}

lv_obj_t *ui_echo_header_right(lv_obj_t *scr, const char *text)
{
    // header 是 scr 的第 0 个子对象。
    lv_obj_t *hdr = lv_obj_get_child(scr, 0);
    lv_obj_t *r = lv_label_create(hdr);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(ECHO_HDRTEXT), 0);
    lv_label_set_text(r, text ? text : "");
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -10, 0);
    return r;
}

lv_obj_t *ui_echo_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(ECHO_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(ECHO_STROKE), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    return p;
}

lv_obj_t *ui_echo_label(lv_obj_t *parent, const char *text,
                        const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

uint32_t ui_echo_motion_color(int score)
{
    if (score < 40) return ECHO_GREEN;
    if (score < 70) return ECHO_AMBER;
    return ECHO_RED;
}

uint32_t ui_echo_heat_color(int level)
{
    if (level < 20) return 0x1F6FE0;  // 蓝
    if (level < 40) return 0x2F9FE0;  // 青
    if (level < 60) return 0x5CCF8A;  // 绿
    if (level < 80) return ECHO_YELLOW;
    return ECHO_AMBER2;               // 橙
}
