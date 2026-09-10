// main/ui_i18n.c —— 见 ui_i18n.h。纯 C。
#include "ui_i18n.h"

#include <stddef.h>

// 品牌/技术记号(ECHO/RADAR/AP/CSI/WiFi/Ping/ms/dBm/MAC/Alpha)即便中文界面也保留
// 英文,既是惯例也缩小汉字子集。下面 zh 列用到的汉字集见 tools 里的字体子集脚本。
static const char *const STR[I18N_KEY_COUNT][2] = {
    // [EN]                          [ZH]
    { "PASSIVE WIFI ECHOLOCATION",   "被动 WiFi 感知" },           // SUBTITLE
    { "WiFi CSI motion sense",       "WiFi CSI 运动感知" },        // DESC_ECHO
    { "Passive device scan",         "被动设备扫描" },             // DESC_RADAR
    { "Settings",                    "设置" },                     // CARD_SETTINGS
    { "IDLE",                        "空闲" },                     // IDLE
    { "connecting",                  "连接中" },                   // CONNECTING
    { "connect failed",              "连接失败" },                 // FAILED
    { "set WiFi first",              "未配置 WiFi" },              // NOWIFI
    { "MOTION",                      "运动" },                     // MOTION
    { "CSI AMP",                     "CSI 幅度" },                 // CSIAMP
    { "DIST",                        "距离" },                     // DIST
    { "RATE",                        "速率" },                     // RATE
    { "OCCUPIED",                    "有人" },                     // OCCUPIED
    { "EMPTY",                       "无人" },                     // EMPTY
    { "CALIBRATING",                 "标定中" },                   // CALIBRATING
    { "please leave area",           "请离开感应区" },             // LEAVE
    { "link: device-AP",             "设备与热点链路" },           // LINK
    { "N/A",                         "无" },                       // NA
    { "PHONE",                       "手机" },                     // PHONE
    { "PC",                          "电脑" },                     // PC
    { "IOT",                         "物联" },                     // IOT
    { "AP",                          "热点" },                     // AP
    { "?",                           "未知" },                     // UNKNOWN
    { "SETTINGS",                    "设置" },                     // SETTINGS_TITLE
    { "Language",                    "语言" },                     // LANGUAGE
    { "Occ threshold",              "占用阈值" },                 // OCC_TH
    { "Smoothing",                   "平滑系数" },                 // SMOOTHING
    { "Scale",                       "量程" },                     // SCALE
    { "Alert",                       "告警" },                     // ALERT
    { "Recalibrate",                 "重新标定" },                 // RECALIB
    { "Ping ms",                     "Ping 间隔" },                // PING
    { "ON",                          "开" },                       // ON
    { "OFF",                         "关" },                       // OFF
    { "once",                        "单次" },                     // ONCE
    { "cont",                        "连续" },                     // CONT
    { "Volume",                      "音量" },                     // VOLUME
    { "Brightness",                  "亮度" },                     // BRIGHT
    { "UP/DN move  OK edit",         "上下选择 确定编辑" },        // NAV_HINT
    { "EN",                          "中文" },                     // LANG_SELF
};

static ui_lang_t s_lang = UI_LANG_ZH;  // 默认中文(用户要求)

void ui_i18n_set_lang(ui_lang_t lang)
{
    s_lang = (lang == UI_LANG_EN) ? UI_LANG_EN : UI_LANG_ZH;
}

ui_lang_t ui_i18n_get_lang(void)
{
    return s_lang;
}

const char *ui_i18n_t(ui_i18n_key_t key)
{
    if (key < 0 || key >= I18N_KEY_COUNT) return "";
    const char *s = STR[key][s_lang];
    return s ? s : "";
}
