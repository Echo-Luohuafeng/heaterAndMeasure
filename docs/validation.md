# 验证范围与记录

## 软件检查

`tests/run_native.py` 独立链接生产组件，使用 AddressSanitizer 和 UndefinedBehaviorSanitizer。纯算法测试覆盖 PID、半百分点量化、无效输入、时间戳、重置和延迟热模型。实时故障注入通过私有测试契约推进真实阶段代码，不再包含生产 `.c` 文件；覆盖测量截止、迟到、背压、租约、维护、旧结果和 STOP 竞争。

应用服务测试验证被 STOP 打断的 START 被拒绝、STOP 绕过操作门闩及元数据容量拒绝不修改配置。

记录器测试验证无 PSRAM 降级、旧代次拒绝、64 份配置容量拒绝和历史覆盖后的元数据回收。维护测试验证完整事务期间互斥、不同任务无法释放事务和暂停失败不授权写入。OTA 测试覆盖碎片读、断开、错误镜像、写入错误、确认与回滚路径。

`tests/run_web.py` 验证真实网页脚本的 DOM/HTTP 交互、配置就绪、显式开始、心跳、CSV 和客户端读取。`tests/check_architecture.py` 检查组件依赖及算法禁止硬件依赖的规则。

这些检查不能模拟模拟电路建立时间、真实中断抖动、USB PHY、功率波形或 ADC 精度。

## 硬件记录

`reference/validation/` 保留此前实际测量的 CSV 和验证结果，记录对应旧固件版本。原构建目录中的样本属于测量证据而不是编译产物，因此清理时单独保留。旧记录不能视为本次 4.0.0-framework 重构的实板验证。

重构后的硬件验收需检查：四路有效掩码、无效帧处理、ADC START/读取截止、任务栈余量、租约到期、USB 拔线/挂起、串口 STOP、OTA 暂停与失败恢复，以及 GPIO/PWM 关断。按所选实验输出配置进行验证。硬件精度校准和示波器验证单独记录配置与测量结果。

## 4.0.0-framework 软件验收（2026-11-18）

- ESP-IDF v6.1.1 `idf.py --preset default build` 通过，无编译警告；应用大小为 `0x86d70` 字节，4 MiB 应用分区剩余约 87%。
- 原生 ASan/UBSan 测试、HTTP 客户端与指标测试、网页交互测试全部通过；增加了停止竞争、截断测量、配置身份、记录器元数据和维护事务测试。
- 架构依赖、中文用途注释与 Google 格式检查通过。
- ELF 符号表确认 `phase_alarm`、`adc_ready`、`runtime_stop_locked`、`runtime_pwm_failure`、`telemetry_store_set_run`、`heater_board_pwm_apply_ticks`、`mcpwm_generator_set_force_level` 和 `mcpwm_soft_sync_activate` 位于 `.iram0.text`。
- 已移除临时占空比限制选项；测试覆盖 3～95% 手动设定、95% 对应 190 个 PWM 计数，以及实验 max_duty 的半百分点限幅和 95% 越界拒绝。
- 本次未烧录或进行实板加热，硬件验收仍按上述项目执行。

验收完成后删除编译产物，只保留实际 sdkconfig、IDE 编译数据库和配置头文件；再次运行默认构建命令即可重新生成固件。
