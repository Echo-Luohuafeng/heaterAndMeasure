
# heater_and_measure

ESP32-S3 N16R8 四路特制 PT100 加热与四线电阻测量固件，使用 ESP-IDF 6.1.1、
VS Code ESP-IDF 扩展和当前 Dev Container。Flash 16 MB，PSRAM 按 Octal 80 MHz
配置，启用启动内存测试。Wi-Fi 用于网页、数据与 OTA；USB Serial/JTAG 保留日志
和停热救援。PSRAM 已实板验证为 8 MB；当前 CPU 为 240 MHz，启用性能优化编译。

每 20 ms 一个周期：前 15 ms 按四路独立占空比加热，后 5 ms 完成开关切换、
四路并行转换和数据读取。输入为整数 0～95，以 15 ms 加热窗口为基准；
95 对应全周期 71.25%。窗口内硬件 PWM 载波为 10 kHz。

## 模块与任务

`heater_board` 管理引脚和 MCPWM；`ads1220` 管理 SPI 与 ADC；
`heater_control` 执行阶段调度、后台安全采样、显式运行控制和控制租约；`heater_protocol` 解析命令与换算。
`telemetry_store` 从内部 RAM 队列取样，写入 4 MiB PSRAM 环形缓存，
PSRAM 不可用时降级为 32 KiB 内部 RAM；`network_manager` 管理配网和重连；
`device_web` 提供网页、HTTP/WebSocket 与 OTA；`host_link` 仅提供 USB STOP/PING。
`app_main()` 完成初始化和任务启动后返回。

| 任务 | 优先级 | 栈大小（字节） | 核心 | 等待方式 |
|---|---:|---:|---:|---|
| heater_cycle | 20 | 6144 | 1 | GPTimer / DRDY / 维护通知 |
| sample_store | 6 | 4096 | 0 | 采样队列 |
| HTTP 服务 | 4 | 10240 | 0 | socket / 服务工作队列 |
| web_push | 3 | 4096 | 0 | 固定 1000 ms 周期 |
| wifi_reconnect | 4 | 4096 | 0 | 通知及退避延时，仅 STA 配网后创建 |
| usb_rescue | 4 | 4096 | 0 | USB 阻塞接收 |
| boot_verify | 5 | 4096 | 0 | 启动自检，完成后删除 |
| ota_writer | 5 | 6144 | 0 | 上传接收，仅升级时创建 |

控制初始化任务也在 Core 1 分配 GPIO/GPTimer 中断，完成后删除。
这样控制及中断与默认运行在 Core 0 的 Wi-Fi 任务隔离；lwIP 也固定在 Core 0。
PSRAM 只承担历史与发送缓存，任务栈、控制状态及 OTA 写入缓冲使用内部 RAM。
后续变更需复查任务栈余量、迟到计数和实际时序；已验证结果见文末。

FreeRTOS tick 为 10 ms，周期和短等待由 1 MHz GPTimer 驱动。
中断执行 GPIO、PWM 和 FromISR 通知，不执行 SPI、日志或浮点换算。
GPIO、GPTimer 和比较器控制函数配置到 IRAM；板级 linker fragment 额外将
IDF 6.1.1 未默认映射的 MCPWM 强制电平和软件同步函数放入 IRAM。

## 硬件连接

依据 `reference/` 原理图和网表，GPIO 按标准 DevKitC-1 排针布局推定，
兼容板仍需对照实物丝印/连通性确认。排针脚号不是 GPIO 号。

| 通道 | PT100 接口 | PWM | ADC CS | ADC DRDY |
|---|---|---:|---:|---:|
| 1 | H6 | GPIO4 | GPIO8 | GPIO17 |
| 2 | H8 | GPIO5 | GPIO9 | GPIO18 |
| 3 | H7 | GPIO6 | GPIO10 | GPIO21 |
| 4 | H9 | GPIO7 | GPIO14 | GPIO38 |

MOSI/SCK/MISO 为 GPIO11/12/13，HEAT_REQ/MEAS_REQ 为 GPIO15/16。
PT100 接口脚序为 `1=F+、2=S+、3=S−、4=F−`。
H1/H2/H4/H5 分别为 48 V/12 V/5 V/3.3 V，全部共地。
图纸没有将 ADC 3.3 V 接到 MCU 排针，需确认 H5 供电。

ADS1220 使用 AIN0−AIN1、PGA 增益 4、Turbo 660 SPS、单次转换、
REFP0/REFN0 外部参考，IDAC1 1.5 mA 输出到 AIN2，IDAC2 关闭。
配置寄存器为 `04 90 47 60`，启动逐路读回核对，失败则关闭请求并停止初始化。

S3 SPI 主机只有三个硬件片选槽，使用一个 SPI 设备和四个 GPIO 片选。
SPI 为 mode 1、2 MHz，初始化和控制任务使用同一设备句柄独占总线。
小于等于四字节的传输使用内联缓冲，减少轮询开销。
IDAC 保持使能，由 PhotoMOS 隔离加热；周期间不发 POWERDOWN。
启动产生的初始转换丢弃。

## 周期时序

| 相对周期起点 | 动作 |
|---:|---|
| 0 µs | 同步载波，锁存四路命令，按通道许可启动 PWM |
| 15000 µs | PWM 强制低，功率端接地放电，加热功率归零 |
| 15100 µs | HEAT_REQ 拉低，断开功率路径 |
| 15200 µs | MEAS_REQ 拉高，接通测量开关 |
| 16800 µs | 通知任务启动四路转换，四路 START 命令须在 17100 µs 前全部完成 |
| 转换就绪 | 分别锁存 DRDY，依次读取 |
| 19050 µs | 中断拉低 MEAS_REQ，强制结束测量 |
| 19350 µs | 更新通道许可，非阻塞提交周期数据 |
| 19900 µs | PWM 保持低，HEAT_REQ 拉高，给自举电容预充电 |
| 20000 µs | 下一周期 |

放电和预充电阶段 HEAT_REQ 为高，但 PWM 为低，F+/F− 接地，没有 48 V
加热功率。互锁切换经过两个请求均为低的阶段。开关留 0.5 ms 导通时间，
随后留 1.1 ms 模拟稳定时间；读操作仅在测量截止前执行。
CPU 240 MHz、性能优化及独占 2 MHz SPI 用于减少缓存与调度开销。
START 窗口为 300 µs，每条命令发送前后均检查截止时间。
`/api/status` 的 timing 字段报告最迟 START 完成时间及启动/读取超时计数。
超时帧仍判为无效，测量开关仍由中断在 19050 µs 关闭。
中断迟到超过默认 50 µs 时立即关闭两个请求、丢弃该帧，重新安排未来的
绝对 20 ms 边界；不会将过去的时间重新设置为闹钟。

## Wi-Fi、网页与配网

首次 USB 完整烧录后，在 115200 波特率启动日志中记录：

```text
Recovery AP: Heater-xxxxxx; AP password / API token: <设备生成的32位口令>
```

每台设备生成独立口令并保存到 NVS。口令同时作为恢复热点密码和 API token，
不要提交到仓库。没有路由器配置时，连接该热点并打开 `http://192.168.4.1/`，
输入口令即可查看采样、控制加热、导出缓存和 OTA。

网页的 Wi-Fi 配网入口保存电脑所在的 2.4 GHz 网络 SSID/密码，停热并重启。
连接成功后关闭配置热点；路由器不可达时，按 1/2/4/8/16/30 秒间隔重试，
约 30 秒后开启恢复热点，继续尝试连接。网页 IP 可从路由器 DHCP 列表或 USB
日志 `Dashboard: http://<IP>/` 查看。建议在路由器设置 DHCP 地址保留。

正常访问使用电脑浏览器打开设备 IP，不需要互联网、CDN 或上位机服务。
默认最大发射功率为 10 dBm，可在 IDF PHY 配置中调整（10–20 dBm）。
此设置适合近距离开发，不能代替稳定供电。Wi-Fi 初始化时出现 Brownout 日志
需要检查 USB 输入、3.3 V 稳压和去耦，欠压保护保持开启。
最多两个 WebSocket 查看者，各自使用独立游标。新连接从最近一秒开始显示，
重连携带已收到的游标补传；`more=true` 表示尚有积压。完整历史可导出 CSV。
浏览器只保留最近一分钟曲线；长期自动保存请使用下方 Python 采集工具。

此版本用于受控局域网，HTTP/WebSocket 未加密；控制、数据、配网和 OTA API
均需口令。需要跨不可信网络部署时另行增加 HTTPS 与固件签名。

## 数据缓存与 HTTP API

上电默认不加热、不写入历史缓存。控制任务保留每 20 ms 的后台安全检测，
以便检查通道健康和完成 OTA 启动自检；这些空闲样本只用于内部检查，不进入 PSRAM 历史。
显式 START 后才每 20 ms 非阻塞提交完整采样，内部队列为 32 条。
缓存任务及时取样，网络每秒发送通常约 50 帧；有积压时单批最多 100 帧。
按约 64 字节/记录估算，4 MiB 能保留约 22 分钟，32 KiB 降级缓存约 10 秒，
实际容量以 `/api/status` 为准。满缓存覆盖最旧记录并计数，队列满则记录丢帧。
网络拥塞不等待采样任务。PSRAM 历史断电/重启后丢失，本版不持续写 Flash 日志。

| 接口 | 说明 |
|---|---|
| `GET /` | 随固件打包的网页 |
| `GET /api/status` | 启动 ID、版本、网络、缓存、维护/OTA、栈/内存和最近采样 |
| `GET /api/data?from=1&limit=250&boot=<boot_id>` | 从游标读取，limit 为 1–250；from=0 从最旧记录开始 |
| `POST /api/control` | `text/plain` 命令 START / SET / PING / STOP，立即回复 |
| `POST /api/reboot` | 确认输出关闭后重启，例如修复 ADC 接线后重新初始化 |
| `POST /api/wifi` | 表单 `ssid=...&password=...`，保存后重启 |
| `POST /api/ota` | 原始应用 `.bin` 字节流，成功后重启 |
| `GET /ws?token=<口令>&from=<游标>&boot=<启动ID>` | 鉴权握手后的每秒批量数据 |

HTTP API 使用 `Authorization: Bearer <口令>`。WebSocket 在握手前校验口令，
每秒收到 JSON：

```json
{"schema":1,"boot_id":"0123456789abcdef","oldest":1,"next":2,"lost":0,"more":false,"frames":[[1,1,20000,[0,0,0,0],[100.0,null,100.0,100.0],13,2,0,0,2048]]}
```

每条记录字段为：

```text
[cursor, sequence, cycle_start_us, [duty1,duty2,duty3,duty4],
 [R1,R2,R3,R4], valid_mask, faults, deadline_misses,
 telemetry_drops, control_stack_free_bytes]
```

电阻单位为 Ω，无效值为 `null`，不回填旧数据；六位小数不代表实际精度。
`valid_mask` 位 0–3 对应四个通道。`cycle_start_us` 是本次启动的 GPTimer 周期时间，
维护恢复时补偿暂停时长以保留时间缺口。`duty` 表示周期起点生效设定，
STOP/故障可能提前终止该周期加热。

`next` 是下一次读取的游标；`lost` 表示请求位置早于缓存最旧记录的数量。
HTTP 的 boot 参数变化会返回 `BOOT_CHANGED`；客户端必须按 boot_id 区分重启，
按游标去重，落盘后再前进游标。发送到 socket 不代表上位机已经保存。

| faults 位值（十进制） | 含义 |
|---:|---|
| 1 | 阶段/控制任务时序异常 |
| 2 | 未收到/读完全部 ADC |
| 4 | SPI 失败 |
| 8 | 饱和、非正值或超出合理范围的电阻 |
| 16 | 控制租约到期，保持停热至下一次 START/STOP |
| 32 | 当前周期收到 STOP |
| 64 | PWM 失败，加热锁定至重启 |

## 控制与 OTA

`SET 2 2 2 2` 只设置四路 0–95 整数：空闲时不启动加热或记录，运行时下一周期生效。
`START 2 2 2 2` 显式开始加热与记录；`START 0 0 0 0` 可用于零占空比测量记录。
`PING` 仅为已有运行续约，不启动空闲任务；`STOP` 立即取消运行、关闭加热并停止历史写入。
START 及运行中的 SET/PING 默认续约 5 秒，到期停热并停止记录。
已有历史保留供查看和导出，每次 START 使用新的运行 ID，旧队列数据不混入新运行。
STA 断开或热点客户端断开也取消加热；重连不会恢复 SET。
网页提供“开始加热”“应用四路设定”“加热保持”和“停止加热与记录”。
默认不勾选加热保持；只有开始运行后勾选才每秒 PING。已有运行也可通过勾选保持续约。
取消勾选停止续约，租约到期后停止；停止按钮立即停止。勾选本身不启动空闲设备。
保持功能在页面可运行、网络连接正常时有效；页面隐藏时仍尝试心跳，关闭时尝试 STOP。
浏览器挂起或网络断开后，设备仍按租约停热，页面不能保证后台永久运行。

上电设定为零，每路只有前一周期有效测量才允许加热；失效通道下一周期输出为零。
默认有效范围 90–180 Ω；全局时序异常取消全部许可。
USB 不再持续发送 DATA，保留日志与 STOP/PING，USB SET/START 回复 `ERR,USE_WIFI`。
旧 `tools/heater_monitor.py` 适用于历史 USB 固件，当前使用 `heater_wifi.py`。

ADC 寄存器验证等初始化失败时，输出关闭并进入恢复模式，网页、配网和 OTA
仍可使用。状态中的 `control_ready=false`、`control_init_error` 说明采样尚未启动，
不产生伪造数据。检查测量板 H5 的 3.3 V 供电、共地和 SPI 后，通过网页重启。
板级输出初始化失败等无法确认安全关断的情况仍停止启动。

两个 4 MiB 应用分区及 OTA 状态分区见 `partitions.csv`。
第一次迁移必须使用完整 `flash`，同时写 bootloader、分区表、初始 OTA 数据和应用；
不能只使用 `app-flash`。NVS 保持原偏移，本版不自动擦除 NVS。
网络 OTA 只上传 `build/default/heater_and_measure.bin`，不上传合并镜像、
bootloader 或分区表。每次发行建议更新根 CMakeLists.txt 中的 PROJECT_VER。

OTA 进入维护状态，清除 SET、关闭请求与 PWM，控制任务停止 GPTimer 和 ADC
操作后发出确认，收到确认才写 Flash。上传使用内部 RAM 4 KiB 缓冲，检查尺寸、
ESP32-S3 芯片标记及工程名，结束时由 IDF 校验镜像，再切换启动分区。
上传/校验失败保留原启动分区，维持停热，用户通过“开始加热”显式恢复控制任务并开始新运行。
异步 OTA 接收不占用 HTTP 服务主循环，上传时可读取状态和进度。

初次 USB 启动在 OTA 状态中登记有效基线。从采样正常版本进行 OTA，要求新版本
在约 10 秒内检测到十次连续健康采样快照后确认，失败则回滚；从 ADC 初始化失败的
恢复模式升级，允许新版本保留网络恢复模式，以便没有测量板时也能开发 OTA。
本次升级是否要求采样自检保存在 NVS，断电后仍有效。正常路由器离线不影响版本确认。
OTA 状态写入同样暂停控制，确认后恢复空闲安全检测，等待显式 START。任务看门狗触发重启以支持
启动卡死回滚。当前已确认版本的测量异常由通道保护处理，仍允许 OTA 修复。

## 构建与上位机使用

```bash
idf.py --preset default build
python tests/run_native.py
python tests/run_web.py
```

`run_web.py` 需要 Node.js，会优先使用 PATH，其次使用当前 VS Code Server 内置 Node。
首次迁移需停止实验，再用实际设备路径完整烧录：

```bash
idf.py --preset default -p /dev/serial/by-id/实际设备 flash monitor
```

记录启动口令、确认 PSRAM 自检/容量和网页地址后退出 monitor（Ctrl+]）。
Python 工具只依赖标准库；口令从交互输入、HEATER_TOKEN 环境变量或
`--token-file` 读取。CSV 用新建模式，拒绝覆盖已有文件。

```bash
python tools/heater_wifi.py --url http://192.168.1.50 status
python tools/heater_wifi.py --url http://192.168.1.50 monitor --duty 0 0 0 0 --seconds 10 --csv baseline.csv
python tools/heater_wifi.py --url http://192.168.1.50 monitor --history --seconds 10 --csv history.csv
python tools/heater_wifi.py --url http://192.168.1.50 monitor --duty 2 2 2 2 --seconds 5 --csv heated.csv
python tools/heater_wifi.py --url http://192.168.1.50 ota build/default/heater_and_measure.bin
python tools/heater_wifi.py --url http://192.168.1.50 reboot
```

采集工具每秒请求新数据，有积压时加快补传，CSV 落盘后推进游标。
未提供 --duty 只读取已有运行或历史，不开始记录；提供后发送一次 START，
包括全零占空比，并每秒 PING，结束时发送 STOP。
连接中断后只恢复数据读取，不重发 START；重启数据带新的 boot_id。
旧 /api/resume 和 CLI resume 入口已移除。原恢复操作用于 OTA 失败后的任务恢复，
现在由 START 承接；内部暂停/恢复仍供 OTA 和启动确认使用。

## 校准与实板验证

使用 `idf.py --preset default menuconfig` 的 Heater and resistance measurement
设置逐路参考电阻实际值（mΩ）与偏移（mΩ），通过
`idf.py --preset default save-defconfig` 保存并检查差异。
默认参考值 1000000 mΩ，偏移 0，尚未替实物完成校准。
换算为 `R = ADC有符号码 × Rref / (4 × 2^23) + offset`。

使用多个已知四线精密电阻覆盖 100～160 Ω，检查逐路偏移、比例和温漂。
快档没有 20 SPS 档的工频抑制；特制 PT100 在断电后的热变化也影响结果，
数据代表固定测量时段的电阻。

首次硬件运行需确认引脚、电源、Q901 和 UMW 驱动器行为，示波器验证
PWM 低侧充电、自举电压、15000 µs 关断、PhotoMOS 通断和采样窗口。
在不同占空比、Wi-Fi 传输/重连、慢客户端、ADC 缺失的条件下检查有效标记、
迟到计数、任务栈余量、内部堆内存和输出关断。验证 PSRAM 8 MB 容量与读写稳定性；
验证 OTA 暂停期间两个请求关闭、升级中断/断电后可启动旧版本、坏版本启动回滚。
已完成的软件及实板检查见下；精度、温漂、模拟稳定时间和示波器波形仍需专门验证。

## 已完成的实板检查（2026-11-17）

以下 1226 帧实板测试使用 `2.0.8-wifi`；当前运行控制版本为 `2.1.2-wifi`。设备网页地址为 `http://10.0.0.109/`。
设备口令与原 Flash 备份仅保存在被忽略的 `build/default/`，不进入仓库。

- ESP32-S3 rev 0.2、Flash 16 MB、PSRAM 8 MB；80 MHz PSRAM 启动读写测试通过，缓存 65536 条。
- 实机 HTTP 鉴权、非法参数、STOP、恢复模式保护及每秒 WebSocket 推送通过。
- 已通过 Wi-Fi 写入并确认新版本；截断镜像被拒绝且保留原固件，失败处理不复位。
- 使用隔离构建的启动即失败固件验证自动回滚，正常版本恢复并可访问。
- 0、单路 1、四路 1、四路 2、STOP 及 5 秒控制租约超时测试，共记录 1226 帧。
- 测试中四路全部有效，相邻记录间隔均为 20000 µs，启动/读取超时、时序迟到和队列丢帧均为零。
- 全部实际占空比不超过 2；测试结束为四路零占空比。控制任务最低栈余量 4952 字节。

原始记录为 `build/default/heater-test-max2.csv`，汇总为 `build/default/heater-test-max2.json`。
此结果覆盖当前接线与短时测试条件，不代替不同供电、负载、网络环境下的时序与模拟精度验证。

## 显式开始与加热保持验证

`2.1.2-wifi` 已验证上电空闲、SET 不启动、START 后记录、持续心跳保持运行、
停止心跳后租约到期冻结历史、STOP 后缓存不再增长，以及新的运行 ID 排除旧队列样本。
已删除页面恢复按钮及旧 `/api/resume`；OTA 失败后可通过 START 恢复控制并开始新段。
实板保持测试记录 428 帧，四路全部有效，测试最大占空比为 1，结束保持不加热、不记录。
结果保存在 `build/default/run-ui-validation.json`。
