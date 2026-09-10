// tests/test_ui_i18n.c —— i18n 表的主机侧单元测试:每个 key 中英都非空。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_ui_i18n.c main/ui_i18n.c -o /tmp/test_i18n && /tmp/test_i18n

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui_i18n.h"

int main(void)
{
    for (int k = 0; k < I18N_KEY_COUNT; k++) {
        ui_i18n_set_lang(UI_LANG_EN);
        const char *e = ui_i18n_t((ui_i18n_key_t)k);
        assert(e && e[0] != '\0');

        ui_i18n_set_lang(UI_LANG_ZH);
        const char *z = ui_i18n_t((ui_i18n_key_t)k);
        assert(z && z[0] != '\0');
    }

    ui_i18n_set_lang(UI_LANG_EN);
    assert(ui_i18n_get_lang() == UI_LANG_EN);
    ui_i18n_set_lang(UI_LANG_ZH);
    assert(ui_i18n_get_lang() == UI_LANG_ZH);

    // 越界 key 安全返回空串。
    assert(ui_i18n_t((ui_i18n_key_t)I18N_KEY_COUNT)[0] == '\0');
    assert(ui_i18n_t((ui_i18n_key_t)-1)[0] == '\0');

    printf("test_ui_i18n: all assertions passed\n");
    return 0;
}
