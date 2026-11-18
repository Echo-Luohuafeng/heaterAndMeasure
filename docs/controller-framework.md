# 恒电阻控制框架

当前提供手动占空比和四路恒电阻 PID。算法在 Core 0 的 `heater_algorithm` 任务
执行（优先级 7、栈 6144 字节），Core 1 保留 20 ms 采样和输出保护。
第 k 帧测量产生第 k+2 周期的结果；计算及传递必须在该周期开始前完成。
缺少结果、来源身份错误或无效输出会关断，旧运行结果无法重新启动加热。

## 算法开发

公共 C++ 接口是 `components/heater_controller/include/controller.hpp` 中的
`heater::Controller`；C 接口在同目录的 `heater_controller.h`。新增算法：

1. 继承 `Controller`，实现 `configure/reset/step`，需要执行器反馈时实现 `on_applied`。
2. 提供具有静态生命周期的实例及 `noexcept` 工厂，避免运行期间分配/释放。
3. 在 `controllers.cpp` 的注册表加入稳定数字 ID、名称、算法版本、参数描述和校验函数。
4. 添加主机单元/被控对象仿真，并在输出上限 2 内进行硬件验证。

参数采用固定 32 个 float 的存储；描述表指定名字、偏移、元素个数、范围及默认值，
最多 16 个命名参数。HTTP 和网页从描述表读取参数，不需要针对新算法添加路由或 UI。
配置准备在空闲时由算法任务完成，`controller.ready=true` 后才能 RSTART。
重启清空算法和配置；STOP 后保留配置，下一次 START 重置算法历史。
型号、模型编号等可以使用数值参数；复杂模型文件管理不属于本版参数 API。

`step` 输入为四路电阻、有效标记、采样时间和序号。子类返回候选占空比、来源序号和成功标记；C 桥接执行层统一限幅并转换为 0.5%
比较器计数，算法不必重复实现输出量化。算法不能直接访问 GPIO/SPI、续约、
操作 NVS/Flash、阻塞等待或在中断中执行。新的复杂算法必须实测 20 ms 预算；
本版没有实现更慢控制周期、在线优化或网络远程推理。

## PID 定义

四路独立；P/I 作用于目标减测量的误差，D 作用于测量值并使用一阶滤波，避免目标阶跃
产生微分冲击。Kd=0 即 PI。Kp 单位为 %/Ω、Ki 为 %/(Ω·s)、Kd 为 %·s/Ω。
`derivative_tau_s` 是微分滤波时间常数，`tracking_gain` 是执行限制的回算系数（1/s）。
条件积分处理饱和，实际输出回算只作用于限幅或安全禁止，普通量化误差不回算积分。
单路无效测量输出零并冻结积分、清除微分历史；时序/PWM 故障仍由原安全层处理。

占空比仍指 15 ms 加热窗口内的百分比。10 kHz PWM 和 2 MHz 计时器对应 200 个
比较器计数，分辨率为 0.5%，保持载波及采样时序。全局上限 2，实验可设置更低上限。
目标限制为当前测量有效范围（默认 90–180 Ω）；能否达到目标由供电、散热和允许输出决定。
示例参数用于功能演示，不是针对实际负载完成整定的控制参数。

## 使用

网页选择恒电阻，填写四路目标及算法参数，应用配置，再点击开始。模式、目标或参数
变化需先停止，以形成独立实验段。加热保持仍是可选的上位机心跳；自动调节不会续约。

```bash
python tools/heater_wifi.py --url http://192.168.7.1 controllers
python tools/heater_wifi.py --url http://192.168.7.1 controller-config examples/controller-pid.json
python tools/heater_wifi.py --url http://192.168.7.1 monitor --resistance --seconds 30 --csv experiment.csv
python tools/evaluate_control.py experiment.csv --tolerance 0.05 --hold-seconds 2 --output metrics.json
```

`monitor --resistance` 发 RSTART、每秒 PING、结束时 STOP；断线后仅恢复读取，不自动重新开始。
CSV 和 `.csv.metadata.json` 使用新建模式，拒绝覆盖文件。网页 CSV 同时下载 metadata JSON。
参数配置及历史元数据都在 RAM，运行期不持久写 Flash。

## HTTP 与数据

- `GET /api/controllers`：算法及参数描述表。
- `POST /api/controller/config`：JSON 包含 `algorithm/target/parameters/tolerance_ohm/max_duty`；
  严格校验重复/未知字段、数组长度、NaN/Inf、参数范围，配置不开始运行。
- `POST /api/control`：原 text/plain 命令继续有效；`RSTART` 启动已准备的恒电阻模式。
  原 START 四路整数仍启动手动模式，空闲 SET 可切回手动；恒电阻运行中拒绝手动 SET/START。
- `GET /api/experiments`：启动 ID、固件、周期、延迟、逐路校准及最近八份恒电阻配置。
  更早配置覆盖后需依赖之前导出的元数据。
- `/api/status.controller`：配置及准备状态、算法任务栈余量、累计故障、续约计数及剩余租约。

数据 schema 升为 2，保留原数组前十个字段，字段 3 允许 0.5% 实际 PWM；第 11 个
字段为实验对象：run_id/config_id/mode/algorithm/target/requested/limited_mask/blocked_mask/
source_sequence/computation_us。mode=0 手动、1 恒电阻；算法 0 手动、1 PID。
新增全局故障位 128 表示算法失败。旧 CSV 客户端需更新，新 Python 客户端兼容 schema 1/2。

实际 PWM 是周期起点锁存的命令，不是对 GPIO 波形或加热能量的测量；STOP/异常可能
提前截断加热窗口。blocked_mask 反映周期起点的通道禁止，faults 另行反映后续关断。
固定 4 MiB 缓存当前容量为 34952 帧，约 11.65 分钟；容量仍由实际结构尺寸计算。

评价脚本按启动/运行/配置/算法分组，输出 RMSE、IAE、ISE、末五秒平均误差、超调、
进入误差带并保持指定时长的首次时间、限幅/禁止比例、计算耗时和输出变化量。
缺失/无效样本中断稳定窗口，不跨缺口积分；PWM 积分是输出使用量代理，不是实际能耗。
对比时保持相同的目标轨迹、初始条件、供电、滤波、控制周期和执行延迟。

## 验证

```bash
python tests/run_native.py
python tests/run_web.py
```

原生测试包含 PID 被控对象仿真、参数校验、失效输入、时间戳、重置、半百分点执行、
截止时间、旧运行拒绝、租约与原采样/OTA 保护。网页测试包含配置、RSTART、CSV 与旧手动控制。

## 本次实板结果（2026-11-18）

已通过 USB 网络 OTA 更新设备，并验证 PID、配置准备、限幅拒绝、手动兼容、
串口 STOP 和独立 5 秒租约。闭环实验使用目标为开始电阻加 0.2 Ω，Kp=4、Ki=0.3、
Kd=0.01，输出上限 1.5（全局仍为 2）。35 秒记录 1756 帧，实际 PWM 包含 0、0.5、1.0%。
所有相邻记录间隔 20 ms，四路有效，无 ADC 启动/读取超时、时序迟到或队列丢帧；
算法最大耗时 224 µs，控制/算法任务最低栈余量分别为 4336/4924 字节。

末五秒四路平均误差（实际减目标）约为 −0.0144、−0.0039、−0.0053、−0.0090 Ω。
按误差带 ±0.05 Ω 且连续保持 2 秒的严格判据，这次短实验尚未得到稳定时间；
需后续结合实际供电、热惯性和测量噪声整定，不能将均值接近目标当作始终恒温。
本次没有做示波器验证或温度标定。

记录在 `build/default/resistance-hardware-samples.csv`，指标在
`build/default/resistance-hardware-metrics.json`，综合结果在
`build/default/resistance-hardware-validation.json`。实验元数据随综合结果保存；
常规 CLI 和网页导出另提供 metadata JSON。测试结束保持 STOP、四路设定零，不再记录。
