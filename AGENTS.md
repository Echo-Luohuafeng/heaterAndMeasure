# 开发约定

## 环境与工程

- 目标为 `esp32s3`；使用 VS Code ESP-IDF 扩展和 Linux Dev Container，固定 ESP-IDF v6.1.1（`/opt/esp/idf`）、用户 `vscode`。终端环境未加载时执行 `. /opt/esp/idf/export.sh`。
- Flash 为 16 MB，正常通信使用 USB-OTG CDC-NCM HTTP/WebSocket，日志及 STOP 救援命令使用同一复合设备的 CDC-ACM；运行时内置 USB Serial/JTAG 与 OTG 不能共用内部 PHY。PSRAM 已实板确认 8 MB、Octal 80 MHz 并通过启动测试；使用 4 MiB 历史缓存。CPU 使用 240 MHz、性能优化编译，ADC SPI 为 2 MHz 且由一个设备句柄独占。
- 当前安全输出上限为 2（`CONFIG_HEATER_OUTPUT_DUTY_LIMIT`），不得未经用户明确授权提高。已完成 USB NCM/ACM、HTTP/WebSocket、OTA、额外供电下拔线及四路最大占空比 2 的实板测试；旧 Wi-Fi 版本已验证启动失败回滚；后续变更仍需检查采样时序、无效帧、任务栈余量及输出关断。硬件精度校准和示波器验证另行进行。
- 算法放在 `heater_controller`，继承 C++ `Controller` 并注册参数描述；算法任务在 Core 0，k 帧结果用于 k+2 周期，不访问 GPIO、不续约、不写 Flash。恒电阻配置需空闲且准备完成后 RSTART；PWM 支持 0.5% 步进，仍保持全局上限 2。数据 schema 2，实验配置和 CSV 须一起保存。
- 上电默认空闲，SET 只更新设定，START/RSTART 才开始加热与历史记录；STOP 或控制租约到期停止记录。后台安全样本用于通道许可和 OTA 自检，不写入历史缓存。
- 分区布局见 `partitions.csv`，使用两个 4 MiB 应用分区及 `otadata`。首次迁移需 USB 完整烧录；日常 OTA 仅上传应用 `.bin`。OTA、运行时 NVS 保存及启动版本确认写 Flash 前，必须获得控制任务的暂停确认。
- 入口为 `main/app_main.c`；业务模块放在 `components/<组件名>/`，组件内包含 `CMakeLists.txt` 和公共头文件目录 `include/`。
- 根工程启用 `MINIMAL_BUILD`；通过 `idf_component_register` 声明源文件、头文件目录及依赖。实现依赖使用 `PRIV_REQUIRES`，公共头文件依赖使用 `REQUIRES`；使用 FreeRTOS 的组件显式声明 `freertos` 依赖。
- 统一使用预设 `default`，`binaryDir` 保持相对路径 `build/default`。VS Code 的目标、构建目录、`sdkconfig` 路径须与预设一致，编译数据库为 `build/default/compile_commands.json`。
- 默认配置保存到 `sdkconfig.defaults`，实际配置为 `build/default/sdkconfig`。已有配置通过 `menuconfig` 调整，默认文件修改不会自动覆盖它；持久修改用 `idf.py --preset default save-defconfig` 保存并检查差异。`fullclean` 会删除实际配置。
- `build/`、`managed_components/` 不提交；第三方组件使用 ESP-IDF Component Manager 管理。

## FreeRTOS

- 后续开发强制使用 ESP-IDF 内置 FreeRTOS。调度器由 IDF 启动；`app_main` 完成初始化、创建通信对象和启动任务后返回，业务按时序需求划分任务。
- 任务循环通过阻塞等待或延时让出 CPU；毫秒转换使用 `pdMS_TO_TICKS()` 并确认 tick 精度，固定周期任务优先使用 `xTaskDelayUntil()`。
- 检查任务及通信对象创建失败；任务参数在使用期间保持有效，任务退出调用 `vTaskDelete(NULL)`。
- 明确任务名称、优先级和栈大小（ESP-IDF 使用字节），验证栈余量和时序；默认双核调度，需要绑核时说明原因。
- 使用队列、任务通知等机制通信，使用互斥锁等机制保护共享数据。中断使用适用的 `...FromISR()` 接口并按需请求调度；中断、临界区和软件定时器回调禁止阻塞，耗时处理交给任务。

## 常用命令

在开发容器的项目根目录执行，串口替换为实际设备：

```bash
idf.py --preset default build
idf.py --preset default menuconfig
idf.py --preset default -p /dev/ttyACM0 flash monitor
```

- 串口优先使用 `/dev/serial/by-id/`，CLI 必须显式传入 `-p`；监视器波特率为 115200，退出按 `Ctrl+]`。
- 代码、依赖或构建配置变更后运行构建；新业务需另行验证硬件运行。

## 容器与设备权限

- 持久环境变更写入 `.devcontainer/Dockerfile` 或 `devcontainer.json`，随后重建容器；重建或迁移前检查宿主机绑定文件是否存在。
- 设备组 GID 默认 18；迁移时核对宿主机 `dialout` GID，通过 `ESP_DEVICE_GID` 同时传给镜像构建和容器启动。
- USB NCM/ACM 使用 VID:PID `303a:4011`；ROM 下载模式仍为 `303a:1001`。权限问题在宿主机配置 udev，将对应 USB 和串口节点设为 `dialout` 组、权限 `0660`，重载后重新插拔。内置 JTAG 仅在内部 PHY 未被 OTG 占用时使用 `board/esp32s3-builtin.cfg`；NCM 固件调试需外部 JTAG。
