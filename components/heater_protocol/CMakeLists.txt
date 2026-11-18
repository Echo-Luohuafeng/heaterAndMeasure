# 文本协议仅依赖业务值类型。
idf_component_register(SRCS "heater_protocol.c" INCLUDE_DIRS "include" REQUIRES heater_domain)
