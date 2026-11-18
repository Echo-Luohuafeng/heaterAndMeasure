idf_component_register(
    SRCS "host_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES espressif__esp_tinyusb network_manager heater_control heater_protocol freertos telemetry_store
)
