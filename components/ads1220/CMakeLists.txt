# ESP-IDF 组件声明；公共类型依赖与实现依赖分开管理。
idf_component_register(
    SRCS "ads1220.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_rom freertos heater_board heater_domain
)
