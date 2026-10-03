idf_component_register(
    SRCS "main.c"
         "companion_store.c"
         "companion_store_format.c"
         "companion_store_files.c"
         "companion_nav.c"
         "companion_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi fatfs freertos nvs_flash wear_levelling
)
