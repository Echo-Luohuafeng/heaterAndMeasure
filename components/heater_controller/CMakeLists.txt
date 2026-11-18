# 算法模块只依赖纯值类型，可独立进行主机仿真。
idf_component_register(SRCS "controllers.cpp" "pid_controller.cpp" "heater_controller.cpp" INCLUDE_DIRS "include" REQUIRES heater_domain)
