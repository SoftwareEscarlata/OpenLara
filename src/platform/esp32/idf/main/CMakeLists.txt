idf_component_register(
    SRCS
        "main.c"
        "display_esplcd.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_timer
        esp_psram
        esp_partition
        driver
)
