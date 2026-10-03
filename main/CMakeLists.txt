idf_component_register(
    SRCS "main.c"
         "companion_store.c"
         "companion_store_format.c"
         "companion_store_files.c"
         "companion_nav.c"
         "companion_ui.c"
         "companion_text.c"
         "companion_secret.c"
         "companion_blufi_security.c"
         "companion_net.c"
         "companion_web.c"
         "companion_wav.c"
         "companion_ai_parse.c"
         "companion_ai.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_http_client esp_http_server esp_hw_support esp_netif esp-tls esp_timer esp_wifi fatfs freertos lwip mbedtls nvs_flash wear_levelling
)
