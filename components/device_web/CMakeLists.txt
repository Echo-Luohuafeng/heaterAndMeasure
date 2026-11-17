idf_component_register(
    SRCS "device_web.c" "ota_update.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html"
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_server app_update bootloader_support esp_app_format esp_system
                  esp_timer esp_hw_support heap freertos heater_control heater_protocol
                  telemetry_store network_manager nvs_flash log
)
