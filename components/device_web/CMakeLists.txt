# HTTP/WS 只适配服务用例与缓存读取，设备生命周期位于维护组件。
idf_component_register(SRCS "device_web.c" "telemetry_json.c" INCLUDE_DIRS "include"
  EMBED_TXTFILES "web/index.html" "web/dashboard.js" "web/api.js" "web/plot.js" "web/stream.js"
    "web/control.js" "web/export.js" "web/dashboard.css"
  REQUIRES esp_common
  PRIV_REQUIRES esp_http_server app_update esp_app_format esp_system esp_timer
    esp_hw_support heap freertos heater_service heater_controller heater_protocol telemetry_store
    device_usb device_maintenance log espressif__cjson)
