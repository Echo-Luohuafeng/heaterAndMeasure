# 维护拥有 NVS/OTA 写入及启动确认，禁止依赖网页或应用服务。
idf_component_register(SRCS "device_maintenance.c" "device_credentials.c" "ota_update.c" "boot_verify.c"
  INCLUDE_DIRS "include" REQUIRES esp_common freertos
  PRIV_REQUIRES heater_runtime nvs_flash app_update bootloader_support esp_app_format
    esp_system esp_hw_support heap log)
