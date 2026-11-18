# ISR 调用链保持 IRAM 安全；私有状态不向业务组件公开。
idf_component_register(SRCS "runtime.c" "phase_engine.c" "acquisition.c"
  "output_guard.c" "run_state.c" "controller_worker.c"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "private"
  REQUIRES esp_common freertos heater_domain
  PRIV_REQUIRES heater_controller ads1220 heater_board esp_driver_gpio esp_driver_gptimer esp_hw_support esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-jump-tables -fno-tree-switch-conversion)
