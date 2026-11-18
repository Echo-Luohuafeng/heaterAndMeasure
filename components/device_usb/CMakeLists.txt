# 传输适配通过服务请求停止，不直接访问实时状态或 NVS。
idf_component_register(SRCS "usb_network.c" "rescue_console.c" INCLUDE_DIRS "include"
  REQUIRES esp_common
  PRIV_REQUIRES esp_netif esp_event esp_hw_support freertos heater_service
    device_maintenance log heap espressif__esp_tinyusb heater_protocol)
