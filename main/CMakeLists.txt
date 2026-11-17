idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES freertos log heater_control heater_protocol host_link heater_board telemetry_store network_manager device_web
)
