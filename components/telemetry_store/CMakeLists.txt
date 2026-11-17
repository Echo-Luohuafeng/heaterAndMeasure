idf_component_register(
    SRCS "telemetry_ring.c" "telemetry_store.c" "telemetry_json.c"
    INCLUDE_DIRS "include"
    REQUIRES heater_protocol freertos esp_common
    PRIV_REQUIRES heap esp_psram log
)
