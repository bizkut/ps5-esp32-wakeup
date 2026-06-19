idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "pswake_bt.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        esp_lcd
        esp_netif
        esp_wifi
        lwip
        nvs_flash
)
