#include "heater_protocol.h"

#include <string.h>

// 判断 `c` 是否为命令允许的空格、制表符或回车，换行由通信层分帧处理。
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// 先裁剪两端空白，再按完整命令及四路整数解析，逐位检查占空比上限。
// 使用局部结果，只有所有字符都合法时才写回输出，避免失败污染已有设定。
bool heater_command_parse(const char* line, size_t length,
                          heater_command_t* command) {
  if (line == NULL || command == NULL || length == 0 ||
      length > HEATER_COMMAND_MAX_LENGTH) {
    return false;
  }
  while (length > 0 && is_space(*line)) {
    ++line;
    --length;
  }
  while (length > 0 && is_space(line[length - 1])) {
    --length;
  }
  heater_command_t parsed = {0};
  if (length == 6 && memcmp(line, "RSTART", 6) == 0) {
    parsed.kind = HEATER_COMMAND_RESISTANCE_START;
  } else if (length > 7 && memcmp(line, "RSTART ", 7) == 0) {
    parsed.kind = HEATER_COMMAND_RESISTANCE_START;
    uint32_t id = 0;
    for (size_t i = 7; i < length; ++i) {
      if (line[i] < '0' || line[i] > '9' ||
          id > (UINT32_MAX - (unsigned)(line[i] - '0')) / 10U)
        return false;
      id = id * 10U + (unsigned)(line[i] - '0');
    }
    if (!id) return false;
    parsed.config_id = id;
  } else if (length == 4 && memcmp(line, "STOP", 4) == 0) {
    parsed.kind = HEATER_COMMAND_STOP;
  } else if (length == 4 && memcmp(line, "PING", 4) == 0) {
    parsed.kind = HEATER_COMMAND_PING;
  } else {
    size_t position;
    if (length >= 4 && memcmp(line, "SET", 3) == 0 && is_space(line[3])) {
      parsed.kind = HEATER_COMMAND_SET;
      position = 3;
    } else if (length >= 6 && memcmp(line, "START", 5) == 0 &&
               is_space(line[5])) {
      parsed.kind = HEATER_COMMAND_START;
      position = 5;
    } else
      return false;
    for (size_t channel = 0; channel < HEATER_CHANNEL_COUNT; ++channel) {
      if (position >= length || !is_space(line[position])) {
        return false;
      }
      while (position < length && is_space(line[position])) {
        ++position;
      }
      if (position >= length || line[position] < '0' || line[position] > '9') {
        return false;
      }
      unsigned value = 0;
      while (position < length && line[position] >= '0' &&
             line[position] <= '9') {
        value = value * 10U + (unsigned)(line[position++] - '0');
        // 每解析一位都检查上限，避免超长数字造成整数溢出。
        if (value > HEATER_MAX_DUTY) {
          return false;
        }
      }
      parsed.duty[channel] = (uint8_t)value;
    }
    if (position != length) {
      return false;
    }
  }
  *command = parsed;
  return true;
}
