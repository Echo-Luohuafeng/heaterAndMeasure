# main 只负责装配与启动，不承担业务循环。
idf_component_register(SRCS "app_main.c" INCLUDE_DIRS "."
  PRIV_REQUIRES freertos log heater_runtime heater_protocol heater_board
    telemetry_store device_usb device_maintenance device_web)
