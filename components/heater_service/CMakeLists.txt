# 应用服务向上提供用例，向下依赖执行、记录与维护。
idf_component_register(SRCS "heater_service.c" INCLUDE_DIRS "include"
  REQUIRES esp_common heater_domain
  PRIV_REQUIRES heater_runtime heater_controller device_maintenance telemetry_store)
