# ESP-IDF 组件声明；公共类型依赖与实现依赖分开管理。
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "4.0.0-framework")
project(heater_and_measure)
