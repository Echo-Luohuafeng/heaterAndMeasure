
# heater_and_measure

ESP32-S3 N16R8 四路特制 PT100 加热与四线电阻测量固件，使用 ESP-IDF 6.1.1、
VS Code ESP-IDF 扩展和当前 Dev Container。Flash 16 MB，PSRAM 未启用，
USB Serial/JTAG 用于日志和上位机通信，GPIO48 RGB 灯保留。

每 20 ms 一个周期：前 15 ms 按四路独立占空比加热，后 5 ms 完成开关切换、
四路并行转换和数据读取。输入为整数 0～95，以 15 ms 加热窗口为基准；
95 对应全周期 71.25%。窗口内硬件 PWM 载波为 10 kHz。

## 模块与任务

`heater_board` 管理引脚和 MCPWM；`ads1220` 管理共用 SPI 总线、四路 GPIO 片选
和 ADC 配置；`heater_control` 执行阶段调度与采样；`heater_protocol` 提供纯 C
命令解析、补码解码和比例换算；`host_link` 执行独立的 USB 收发。
`app_main()` 完成初始化和任务启动后返回，采用 IDF 默认双核调度。

| 任务 | 优先级 | 栈大小（字节） | 等待方式 |
|---|---:|---:|---|
| heater_cycle | 20 | 6144 | 定时器及 DRDY 任务通知 |
| host_rx | 4 | 4096 | USB 接收阻塞等待 |
| host_tx | 3 | 4096 | 输出队列阻塞等待 |

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
SPI 为 mode 1、1 MHz，初始化和控制任务独占总线。
IDAC 保持使能，由 PhotoMOS 隔离加热；周期间不发 POWERDOWN。
启动产生的初始转换丢弃。

## 周期时序

| 相对周期起点 | 动作 |
|---:|---|
| 0 µs | 同步载波，锁存四路命令，按通道许可启动 PWM |
| 15000 µs | PWM 强制低，功率端接地放电，加热功率归零 |
| 15100 µs | HEAT_REQ 拉低，断开功率路径 |
| 15200 µs | MEAS_REQ 拉高，接通测量开关 |
| 17000 µs | 通知任务启动四路转换，四路 START 命令须在 17200 µs 前全部完成 |
| 转换就绪 | 分别锁存 DRDY，依次读取 |
| 19050 µs | 中断拉低 MEAS_REQ，强制结束测量 |
| 19350 µs | 更新通道许可，非阻塞提交周期数据 |
| 19900 µs | PWM 保持低，HEAT_REQ 拉高，给自举电容预充电 |
| 20000 µs | 下一周期 |

放电和预充电阶段 HEAT_REQ 为高，但 PWM 为低，F+/F− 接地，没有 48 V
加热功率。互锁切换经过两个请求均为低的阶段。开关留 0.5 ms 导通时间，
随后留 1.3 ms 模拟稳定时间；读操作仅在测量截止前执行。
160 MHz 实板上，四路软件片选 START 命令及任务调度超过 100 µs，
因此启动窗口为 200 µs；每条命令发送前后均检查截止时间。
超时帧仍判为无效，测量开关仍由中断在 19050 µs 关闭。
中断迟到超过默认 50 µs 时立即关闭两个请求、丢弃该帧，重新安排未来的
绝对 20 ms 边界；不会将过去的时间重新设置为闹钟。

## 上位机协议

内置 USB Serial/JTAG，ASCII 命令支持以 CR（`\r`）、LF（`\n`）或 CRLF（`\r\n`）结束，
CRLF 只执行一次命令，空行忽略：

```text
SET 5 5 5 5
PING
STOP
```

- SET 恰好四个 0～95 整数，下一周期开始统一生效，回复 `ACK,SET`；
  设定持续保持，直到新的 SET、STOP 或设备重启，实际输出仍受测量和时序保护约束。
- PING 为可选通信检查，回复 `ACK,PING`，不改变加热状态。
- STOP 立即关闭加热请求并取消当前 SET，回复 `ACK,STOP`。
- SET 无命令超时，无需定期发送 PING；上位机停止发送或串口断开后设定继续保持。
- 非法/越界命令回复 `ERR,COMMAND`，保持原设定；超过 64 字节的命令丢弃至换行，
  回复 `ERR,LINE_TOO_LONG`。
- ACK 表示接收，实际值以 DATA 帧为准；队列拥塞时 ACK/遥测可能丢失。

健康运行每 20 ms 发送一行，字段依次为：

```text
DATA,sequence,cycle_start_us,duty1,duty2,duty3,duty4,R1,R2,R3,R4,valid_mask,faults,deadline_misses,telemetry_drops,control_stack_free_bytes
```

实际行包含数值。电阻单位为 Ω，六位小数不代表实际精度。
cycle_start_us 为 GPTimer 启动后的周期起点时间。
valid_mask 位 0～3 对应通道 1～4，四路有效时为十进制 15。
无效通道为 `nan`，不回填旧数据。duty 表示周期起点的生效设定，
STOP/故障可能提前终止该周期加热。
迟到、丢遥测计数和控制任务历史最低栈余量（字节）供实板诊断。

| faults 位值（十进制） | 含义 |
|---:|---|
| 1 | 阶段/控制任务时序异常 |
| 2 | 未收到/读完全部 ADC |
| 4 | SPI 失败 |
| 8 | 饱和、非正值或超出合理范围的电阻 |
| 16 | 保留（旧版命令超时位，当前固件不使用） |
| 32 | 当前周期收到 STOP |
| 64 | PWM 失败，加热锁定至重启 |

上电占空比为零。冷启动首帧可能因时序检查返回 `nan`，后续周期继续采样。
每路必须在前一周期完成有效测量才允许加热；
失效通道下一周期实际占空比为零。默认有效保护范围 90～180 Ω，
为额定 100～160 Ω 留出边界噪声裕量。全局时序异常取消全部通道许可。
USB 发送与控制任务分离，队列满时丢遥测，周期任务不等待发送。
日志可能出现在同一端口，按 DATA/ACK/ERR/READY 前缀识别协议。

## 构建与使用

```bash
idf.py --preset default build
python tests/run_native.py
idf.py --preset default -p /dev/serial/by-id/实际设备 flash
```

关闭 IDF monitor 等占用端口的程序，先读取零占空比数据：

```bash
python tools/heater_monitor.py -p /dev/serial/by-id/实际设备 --seconds 10 --csv baseline.csv
```

以四路 5% 占空比发送一次 SET 并监视，退出时发送 STOP：

```bash
python tools/heater_monitor.py -p /dev/serial/by-id/实际设备 --duty 5 5 5 5 --seconds 5
```

工具正常退出发送 STOP；未提供 --duty 时只接收，不发命令。
依赖容器已安装的 pyserial，CSV 新建文件，避免覆盖记录。

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
在不同占空比、USB 拥塞、上位机断开和 ADC 缺失的条件下检查有效标记、
迟到计数、任务栈余量和输出关断。编译及软件测试不代表这些实板验证已完成。
