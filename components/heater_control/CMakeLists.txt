idf_component_register(
    SRCS "heater_control.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos heater_protocol
    PRIV_REQUIRES ads1220 heater_board esp_driver_gpio esp_driver_gptimer esp_hw_support
)
# Avoid flash-resident switch tables in the IRAM timer callback.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-jump-tables -fno-tree-switch-conversion)
