# ESP-IDF 组件声明；公共类型依赖与实现依赖分开管理。
idf_component_register(
    SRCS "heater_board.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_common heater_domain
    PRIV_REQUIRES esp_driver_gpio esp_driver_mcpwm
)
