idf_component_register(
    SRCS "network_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_netif esp_event nvs_flash esp_hw_support bootloader_support freertos heater_control log heap espressif__esp_tinyusb
)
