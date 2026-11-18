# ESP-IDF 组件声明；公共类型依赖与实现依赖分开管理。
idf_component_register(
    SRCS "telemetry_ring.c" "telemetry_store.c"
    INCLUDE_DIRS "include"
    REQUIRES heater_domain freertos esp_common
    PRIV_REQUIRES heap esp_psram log
)
