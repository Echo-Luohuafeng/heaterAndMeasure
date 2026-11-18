// 文本命令是外部适配格式，内部组件只依赖业务模型。
#pragma once
#include "heater_domain.h"
// 解析长度为 `length` 的命令到非空的 `command`，`line` 无需以 NUL 结尾。
// 接受 START/SET 加四路 0～95 整数，以及无参数的 STOP/PING；不接受换行符。
// 指针为空、长度或语法非法时返回 false，且不改写 `command`。
bool heater_command_parse(const char* line, size_t length,
                          heater_command_t* command);
