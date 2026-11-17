idf_component_register(
    SRCS "heater_board.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_common heater_protocol
    PRIV_REQUIRES esp_driver_gpio esp_driver_mcpwm
)
