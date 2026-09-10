idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "puzzle_ui.c"
         "demo_sudoku.c"
         "sudoku_model.c"
         "demo_mines.c"
         "mines_model.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_csi.c"
         "csi_metric.c"
         "csi_proto.c"
         "csi_ble.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer
             esp_wifi esp_netif esp_event nvs_flash lwip
             bt esp_coex
)
