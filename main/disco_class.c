// main/disco_class.c —— 见 disco_class.h。纯 C,只用标准库。
#include "disco_class.h"

#include <string.h>

// 内置小型 OUI → (类型, 厂商) 表。仅十余条常见前缀,用于对"全局 MAC"的粗猜;
// 随机 MAC 不会命中真实 OUI,自然落到 UNKNOWN / ""。
typedef struct {
    uint8_t      oui[3];
    disco_type_t type;
    const char  *vendor;
} oui_entry_t;

static const oui_entry_t OUI_TABLE[] = {
    // Apple → 手机/平板
    { { 0x00, 0x03, 0x93 }, DISCO_PHONE, "Apple" },
    { { 0x00, 0x1B, 0x63 }, DISCO_PHONE, "Apple" },
    { { 0x3C, 0x15, 0xC2 }, DISCO_PHONE, "Apple" },
    { { 0xAC, 0xBC, 0x32 }, DISCO_PHONE, "Apple" },
    { { 0xF0, 0x18, 0x98 }, DISCO_PHONE, "Apple" },
    // Samsung → 手机/平板
    { { 0x00, 0x12, 0xFB }, DISCO_PHONE, "Samsung" },
    { { 0x5C, 0x0A, 0x5B }, DISCO_PHONE, "Samsung" },
    { { 0x88, 0x32, 0x9B }, DISCO_PHONE, "Samsung" },
    // Intel → PC
    { { 0x00, 0x1B, 0x21 }, DISCO_PC, "Intel" },
    { { 0x3C, 0xA9, 0xF4 }, DISCO_PC, "Intel" },
    { { 0x7C, 0x7A, 0x91 }, DISCO_PC, "Intel" },
    { { 0x34, 0x13, 0xE8 }, DISCO_PC, "Intel" },
    // Dell → PC
    { { 0x00, 0x14, 0x22 }, DISCO_PC, "Dell" },
    { { 0xB8, 0xAC, 0x6F }, DISCO_PC, "Dell" },
    // Espressif → IoT/开发板
    { { 0x24, 0x0A, 0xC4 }, DISCO_IOT, "Espressif" },
    { { 0x24, 0x6F, 0x28 }, DISCO_IOT, "Espressif" },
    { { 0x30, 0xAE, 0xA4 }, DISCO_IOT, "Espressif" },
    { { 0x7C, 0x9E, 0xBD }, DISCO_IOT, "Espressif" },
    // Raspberry Pi → IoT
    { { 0xB8, 0x27, 0xEB }, DISCO_IOT, "Raspberry Pi" },
    { { 0xDC, 0xA6, 0x32 }, DISCO_IOT, "Raspberry Pi" },
    { { 0xE4, 0x5F, 0x01 }, DISCO_IOT, "Raspberry Pi" },
    // Google/Nest → IoT
    { { 0x3C, 0x5A, 0xB4 }, DISCO_IOT, "Google" },
    { { 0xF4, 0xF5, 0xD8 }, DISCO_IOT, "Google" },
};

#define OUI_TABLE_LEN (sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]))

static const oui_entry_t *lookup_oui(const uint8_t mac[6])
{
    if (mac == NULL) return NULL;
    for (size_t i = 0; i < OUI_TABLE_LEN; i++) {
        if (mac[0] == OUI_TABLE[i].oui[0] &&
            mac[1] == OUI_TABLE[i].oui[1] &&
            mac[2] == OUI_TABLE[i].oui[2]) {
            return &OUI_TABLE[i];
        }
    }
    return NULL;
}

bool disco_is_randomized(const uint8_t mac[6])
{
    if (mac == NULL) return false;
    return (mac[0] & 0x02) != 0;
}

const char *disco_vendor(const uint8_t mac[6])
{
    const oui_entry_t *e = lookup_oui(mac);
    return e ? e->vendor : "";
}

disco_type_t disco_classify(const uint8_t mac[6], bool is_ap)
{
    if (is_ap) return DISCO_AP;
    if (disco_is_randomized(mac)) return DISCO_UNKNOWN;
    const oui_entry_t *e = lookup_oui(mac);
    return e ? e->type : DISCO_UNKNOWN;
}

const char *disco_type_name(disco_type_t t)
{
    switch (t) {
    case DISCO_AP:    return "AP";
    case DISCO_PHONE: return "PHONE";
    case DISCO_PC:    return "PC";
    case DISCO_IOT:   return "IOT";
    default:          return "?";
    }
}
