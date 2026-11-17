idf_component_register(
    SRCS "host_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_driver_usb_serial_jtag heater_control heater_protocol freertos telemetry_store
)
