# 四路 PT100 加热与电阻控制固件

ESP32-S3 定制板固件。四路组成同一实验，目标与控制参数可分别设置；主要控制量为电阻，当前提供手动 PWM 和恒电阻 PID。固件不进行温度换算。

## 阅读入口

建议按以下顺序学习，先理解启动、数据与时间，再阅读算法和通信：

1. [main/app_main.c](main/app_main.c)：装配通信对象、初始化组件并启动任务。
2. [业务模型](components/heater_domain/include/heater_domain.h)：命令、帧、配置、故障与独立状态维度。
3. [架构与状态归属](docs/architecture.md)：依赖、文件职责和跨任务协议。
4. [实时阶段](components/heater_runtime/phase_engine.c) 与 [采样](components/heater_runtime/acquisition.c)：20 ms 周期。
5. [算法开发](docs/controller-guide.md)：算法注册、参数、执行反馈和仿真。
6. [HTTP 协议](docs/protocol.md)：网页、CLI、实验记录与 OTA。
7. [时序与安全](docs/timing-and-safety.md)：许可、停止、租约及 Flash 暂停确认。
8. [硬件参考](reference/Hardware.md)：原理图与排针；硬件推导和软件行为分开说明。

## 开发环境

固定 ESP-IDF v6.1.1 `/opt/esp/idf`，Linux Dev Container，用户 `vscode`，目标 `esp32s3`。Flash 为 16 MB，PSRAM 为 8 MB Octal 80 MHz；CPU 为 240 MHz。根项目启用 `MINIMAL_BUILD`，第三方 USB 与 cJSON 依赖由 Component Manager 管理。

```bash
. /opt/esp/idf/export.sh
idf.py --preset default build
idf.py --preset default menuconfig
idf.py --preset default -p /dev/serial/by-id/实际设备 flash monitor
```

统一构建目录 `build/default`，实际配置 `build/default/sdkconfig`，编译数据库 `build/default/compile_commands.json`。CLI 必须显式指定端口，监视器波特率 115200，退出按 `Ctrl+]`。

`sdkconfig.defaults` 是默认配置，修改它不会覆盖已有实际配置。持久配置修改通过 `idf.py --preset default save-defconfig` 保存。运行 `python tools/clean_generated.py` 清理生成文件，保留实际配置、IDE 索引和历史实板证据；重新构建会恢复编译数据库及固件。

C/C++ 与 JavaScript 使用根目录 `.clang-format` 的 Google 风格；Python 使用 YAPF 的 Google 风格。手写文件使用中文注释，说明用途、所有权、单位、时序和故障处理。不要把任务栈、优先级及绑核理由只写在文档里。

## 连接与实验

USB 是 CDC-NCM 网络与 CDC-ACM 救援串口的复合设备。设备地址 `http://192.168.7.1/`；主机由设备 DHCP 获取地址，不替换电脑默认路由或 DNS。USB 口令从救援日志读取，首次生成后保存在 NVS。

1. 打开网页并填写设备口令。
2. 选择手动或恒电阻模式，填写四路参数。
3. 点击开始才创建运行、加热并记录。零占空比 START 可用于无加热测量实验。
4. 加热保持是可选心跳；默认租约为 5 秒。算法和数据查看不续约。
5. STOP、租约到期或 USB 断开/挂起会取消运行。重连及页面重开不自动开始。
6. 导出 CSV 时同时保存 metadata JSON。

PWM 接口支持 **0～95%**；恒电阻实验通过 `max_duty` 设置输出限幅，网页及示例默认为 95%。占空比指 15 ms 加热窗口内的百分比，闭环分辨率为 0.5 个百分点；锁存 PWM 不是实际能量测量。

```bash
python tools/heater_cli.py --url http://192.168.7.1 status
python tools/heater_cli.py --url http://192.168.7.1 controllers
python tools/heater_cli.py --url http://192.168.7.1 controller-config examples/controller-pid.json
python tools/heater_cli.py --url http://192.168.7.1 monitor --resistance --seconds 30 --csv experiment.csv
python tools/heater_cli.py --url http://192.168.7.1 stop
python tools/evaluate_control.py experiment.csv --tolerance 0.05 --hold-seconds 2 --output metrics.json
```

CLI 可用 `HEATER_TOKEN` 或 `--token-file` 读取口令；未提供时隐藏输入。配置命令等待相应 config_id 准备完成；监控不会因断线重发 START。

历史优先使用 4 MiB PSRAM，失败时降级为 32 KiB 内部 RAM，只在显式运行时记录，不写 Flash。元数据容量为 64 份，仍被历史引用的配置不能回收；容量不足时拒绝新实验，避免导出无法解释的数据。设备重启会清空历史与算法配置。

## 升级与验证

[partitions.csv](partitions.csv) 提供两个 4 MiB 应用分区。首次完整烧录后，日常 OTA 仅上传本工程应用 `.bin`。升级暂停采样，失败保持停热；成功后重启并执行启动确认。已验证过的硬件记录见 [验证说明](docs/validation.md)，它们不能证明重构版本已经通过实板测试。

```bash
python tests/run_native.py
python tests/run_web.py
python tests/check_architecture.py
```

主机测试包含 PID 仿真、命令校验、时序故障、STOP 竞争、结果代次、历史元数据和 Flash 事务。硬件还需检查采样截止、无效帧、任务栈、输出关闭，以及示波器波形和精度校准。

旧 Wi-Fi 架构说明、旧串口 DATA 监控脚本和过程构建日志不再作为阅读入口。生成文件位于忽略的 `build/`、`managed_components/`；源码、原理图及有价值的实板测量记录单独保留。
