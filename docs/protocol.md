# HTTP 与实验数据协议

## 传输与鉴权

设备 `http://192.168.7.1/`，USB CDC-NCM，HTTP/WS；日志和救援使用同一 USB 复合设备的 CDC-ACM。API 需要 `Authorization: Bearer <32 位口令>`；WS 握手通过查询参数 token 校验。页面与静态资源无需口令。两个 WS 查看者各自维护游标，WS 不接受控制命令。

| 路由 | 方法 | 含义 |
|---|---|---|
| `/api/status` | GET | 运行、配置、当前安全测量、历史最新帧、内存及诊断 |
| `/api/controllers` | GET | 算法及参数描述表 |
| `/api/controller/config` | POST | JSON 配置，仅空闲可用；返回 config_id，随后等待 ready |
| `/api/control` | POST | text/plain：START、SET、RSTART、STOP、PING |
| `/api/experiments` | GET | 固件、时序、校准和保留配置元数据 |
| `/api/data` | GET | 历史分页，from/limit/boot；最多 250 帧 |
| `/ws` | GET 升级 | 每秒最多 100 帧，最多两个查看者 |
| `/api/ota` | POST | 本工程应用 .bin，维护暂停后写入备用分区 |
| `/api/reboot` | POST | 确认安全暂停后重启 |

## 命令

`START d1 d2 d3 d4` 创建手动运行；`SET d1 d2 d3 d4` 只修改设定，空闲不启动，运行时续约。命令占空比是四路 0～95 的整数。`RSTART config_id` 启动指定已准备配置；裸 RSTART 兼容旧客户端。`PING` 仅为正在运行的实验续约。`STOP` 取消运行及记录，保留历史和恒电阻配置。

配置 JSON 必须包含 `algorithm/target/parameters/tolerance_ohm/max_duty`，拒绝重复及未知字段、错误数量、非有限值和越界。ready 对应同一个 config_id；state 为 0 未配置、1 准备中、2 就绪、3 失败。`max_duty` 可设为 0～95，网页及示例默认为 95；可按实验需要降低。配置成功响应不表示开始运行。

START/RSTART 响应包含 run_id 和起始历史游标 next。当前没有客户端专属控制所有权，持有口令的客户端均可控制；查看页面不自动续约。

## schema 2

批次对象包含 `schema/boot_id/oldest/next/lost/more/frames`。每个 frame 是 11 项数组：

| 索引 | 含义 |
|---|---|
| 0 | 历史 cursor，安全测量快照为 0 |
| 1 | 控制周期 sequence |
| 2 | cycle_start_us |
| 3 | 四路锁存占空比，允许 0.5 个百分点 |
| 4 | 四路电阻 Ω，无效值 null |
| 5 | valid_mask |
| 6 | faults |
| 7 | deadline_misses |
| 8 | telemetry_drops |
| 9 | control_stack_free_bytes |
| 10 | 实验对象 |

实验对象包含 `run_id/config_id/mode/algorithm/target/requested/limited_mask/blocked_mask/source_sequence/computation_us`。mode 0 手动、1 恒电阻。故障位：1 时序、2 ADC 超时、4 SPI、8 样本、16 租约、32 STOP、64 PWM、128 算法。

`status.latest` 是历史最新帧；`status.measurement` 是后台安全测量，空闲也更新；`measurement_age_us` 表示安全测量距发布时刻的时间。维护期间应识别旧测量，不能把历史最新值解释为当前输入。

cursor 从 1 递增，与周期 sequence 不同。from=0 从最旧记录开始；过旧游标返回 lost，boot 不匹配返回 409。数据仅保存在 RAM；服务发送成功不保证电脑落盘。CLI 在 CSV flush/fsync 后推进游标，断线只恢复读取。CSV 与 metadata JSON 使用新建模式，必须一起保存。CLI 在配置变化时更新元数据，并以 sessions 保留跨 boot_id 的快照。

维护失败并释放操作门闩后，新的有效配置请求可以恢复已确认暂停的后台安全采样，再准备配置；仍需 RSTART 才开始加热和记录。Flash 事务进行中或暂停未确认时拒绝该操作。
