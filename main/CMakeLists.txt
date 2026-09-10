idf_component_register(
    SRCS "main.c"
         "ui_echo.c"
         "ui_i18n.c"
         "lv_font_echo_cjk_16.c"
         "lv_font_echo_cjk_20.c"
         "echo_state.c"
         "app_settings.c"
         "demo_settings.c"
         "demo_radio.c"
         "demo_csi.c"
         "csi_metric.c"
         "csi_proto.c"
         "csi_ble.c"
         "occupancy.c"
         "demo_discovery.c"
         "disco_class.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer
             esp_wifi esp_netif esp_event nvs_flash lwip
             bt esp_coex
)
