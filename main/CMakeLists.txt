idf_component_register(
    SRCS "main.c"
         "ui_echo.c"
         "demo_radio.c"
         "demo_csi.c"
         "csi_metric.c"
         "csi_proto.c"
         "csi_ble.c"
         "demo_discovery.c"
         "disco_class.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer
             esp_wifi esp_netif esp_event nvs_flash lwip
             bt esp_coex
)
