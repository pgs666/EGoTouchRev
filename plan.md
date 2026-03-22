**架构蓝图 V1.0（面向“Device自维持 + 外部异步控制 + SystemDetector联动”）**

以下蓝图基于你当前仓库现状设计，目标是：
1. 设备采集循环从 `App` 下沉到 `Device`，实现“自维持运行”。
2. 外部（UI/System/策略）通过异步命令控制设备，而不是直接抢占硬件调用。
3. `SystemDetector` 成为系统状态事件源，并通过策略层安全联动 `Device`。

---

**1. 目标与范围**

**1.1 设计目标**
- `Device` 自主维护采集线程、错误恢复、运行状态。
- `App` 从“直接驱动硬件”变为“策略编排与可视化”。
- `Host/SystemDetector` 输出标准化系统事件，不直接耦合硬件实现。
- 保留你现有 `HimaxChip` 的底层寄存器/命令逻辑，减少推倒重来。

**1.2 非目标**
- 本阶段不重写 `Engine` 算法链。
- 本阶段不实现真实 VHF 注入，只保留扩展位。
- 本阶段不优化到极限性能，先保证稳定性和可控性。

**1.3 固件约束（必须遵守）**
- 固件不可修改，正常模式固定为正常帧率。
- 发送 `EnterIdle` 后进入低帧率模式，检测到手指触碰后由固件自行恢复活跃采样行为。
- 屏幕熄灭后芯片会直接掉电，亮屏后必须重新 `Init`。
- 若通信故障经 `check_bus` 判定为 `bus dead`，软件侧不做自动恢复，需重启系统后恢复。

---

**2. 总体分层（目标形态）**

```text
App
	-> Coordinator (编排器)
	-> PowerPolicyController (策略决策)
	-> DiagnosticUI (控制/观测，不直接碰硬件)

Host
	-> SystemMonitor (Windows状态采集)
	-> ISystemMonitor (抽象事件流)

Device
	-> DeviceRuntime (状态机 + 线程 + 命令队列 + 恢复策略)
	-> IDeviceRuntime (抽象控制面)
	-> HimaxChip (底层硬件动作集合，保留)
	-> HalDevice (IOCTL封装，保留)

Engine
	-> FramePipeline (算法链)

Data Path: DeviceRuntime -> App/Engine
Control Path: UI/SystemPolicy -> DeviceRuntime
```

---

**3. 关键接口蓝图**

**3.1 Device 控制接口（建议）**
- `StartRuntime()`
- `StopRuntime()`
- `SubmitCommand(DeviceCommand cmd)`  
- `TryPopFrame(HeatmapFrame& out)` 或 `SubscribeFrame(callback)`
- `GetRuntimeSnapshot()`（状态+指标）

**3.2 Command 模型（异步）**
- `DeviceCommandType`：
	- `Init`
	- `Deinit`
	- `StartStreaming`
	- `StopStreaming`
	- `EnterIdle`
	- `ExitIdle`
	- `StartCalibration`
	- `EnableFreqShift`
	- `DisableFreqShift`
	- `SetScanRate`
	- `SetPolicyProfile`
	- `Shutdown`
- `DeviceCommand` 字段：
	- `id`
	- `type`
	- `priority`
	- `payload`
	- `deadline`
	- `source`（UI/System/Policy）

**3.2.1 V1 命令实现边界（固件受限）**
- 先实现并联调：`Init/Deinit/StartStreaming/StopStreaming/EnterIdle/Shutdown`。
- `send_command` 路径当前仅落地 `EnterIdle`。
- `StartCalibration/EnableFreqShift/DisableFreqShift/SetScanRate` 保留命令位，但暂不实现具体硬件动作（返回 `NotImplemented` 或 `Rejected`）。

**3.3 Runtime 状态快照**
- `lifecycleState`（后述状态机）
- `connectionState`
- `afeMode`
- `isStreaming`
- `lastError`
- `consecutiveFrameTimeouts`
- `recoverCount`
- `fps`
- `queueDepth`

**3.4 Host 事件接口**
- `ISystemMonitor::Start()`
- `ISystemMonitor::Stop()`
- `ISystemMonitor::PollEvents(std::vector<SystemEvent>&)`
- 或 `Subscribe(std::function<void(const SystemEvent&)>)`

---

**4. DeviceRuntime 状态机设计（核心）**

**4.1 状态定义**
- `Uninitialized`
- `Initializing`
- `Ready`
- `Streaming`
- `Idle`
- `Recovering`
- `Stopping`
- `Error`

**4.2 状态转换规则**
1. `Uninitialized -> Initializing` 由 `Init` 命令触发。  
2. `Initializing -> Ready` 当 `HimaxChip::Init()` 成功。  
3. `Ready -> Streaming` 当 `StartStreaming` 命令触发并创建/唤醒采集循环。  
4. `Streaming -> Idle` 当 `EnterIdle` 命令触发且硬件命令成功。  
5. `Idle -> Streaming` 由固件在触摸触发后自动回到活跃采样（V1 不依赖显式 `ExitIdle`）。  
6. `Streaming/Idle -> Recovering` 当连续超时或通信错误达到阈值。  
7. `Recovering -> Streaming` 恢复成功。  
8. `Recovering -> Error` 达到恢复上限。  
9. 任意活跃状态 -> `Stopping` 由 `StopRuntime/Shutdown/Deinit` 触发。  
10. `Stopping -> Uninitialized` 关闭完成。  

**4.3 恢复策略**
- 第一层：轻恢复（重置 block/timeout + 重试 `GetFrame`）。
- 第二层：中恢复（`IntClose/IntOpen`）。
- 第三层：重恢复（`Deinit -> Init`）。
- 失败升级使用指数退避：`100ms, 300ms, 1s, 3s`。
- 超过上限进入 `Error` 并等待人工/策略重启。
- 任一通信异常后增加 `check_bus` 判定；若判定 `bus dead`，直接进入 `FatalBusDead`（或 `Error` 子态），停止自动恢复并上报“需重启系统”。

---

**5. 设备自维持循环设计**

**5.1 线程模型**
- `CommandThread`：串行执行控制命令，保证硬件控制调用单通道化。
- `AcquisitionThread`：只做 `WaitInterrupt/GetFrame` + 发布帧，不处理重控制。
- 可选 `HealthThread`：周期性健康检查与指标采样。

**5.2 采集循环伪流程**
1. 检查状态是否允许采集（必须是 `Streaming`）。  
2. `WaitInterrupt(timeout)`。  
3. 超时则计数，低于阈值继续，高于阈值进入 `Recovering` 请求。  
4. 成功中断后 `GetFrame(master + slave)`。  
5. 组装 frame（timestamp + rawData + metadata）。  
6. 发布到输出队列（覆盖式 ring buffer，防背压卡死）。  
7. 更新统计（fps、延迟、丢帧数）。  

**5.3 并发安全原则**
- 对 `HimaxChip` 的硬件访问路径必须受统一序列化锁保护。
- `HalDevice` 当前实现复用了 `m_xfer_buffer`/`m_ov`，必须避免多线程并发直接调用。
- 采集与控制冲突时，由 `CommandThread` 发“状态门控”，采集线程只读状态，不直接执行高风险控制动作。

---

**6. 你当前代码缺失项对应表（面向落地）**

1. 缺失 `DeviceRuntime`（现在 `Chip` 只有同步 API）。
2. 缺失命令队列和优先级调度器。
3. 缺失统一状态机（目前 `m_connState` 粒度不够）。
4. 缺失恢复策略编排器（目前错误只返回，不闭环恢复）。
5. 缺失 frame 发布通道（目前外部读 `back_data`，耦合重且竞态风险高）。
6. 缺失 runtime metrics。
7. 缺失 `Host -> Policy -> Device` 的稳定桥接层。

---

**7. SystemDetector 蓝图（Windows 平板）**

**7.1 必采系统状态**
- `DisplayOn`
- `DisplayOff`
- `Suspend`
- `Resume`
- `Shutdown`
- `SessionLock`
- `SessionUnlock`

**7.2 建议采集状态**
- `ACPowerConnected`
- `ACPowerDisconnected`
- `BatteryLevelLow`
- `BatterySaverOn`
- `BatterySaverOff`

**7.3 平板场景可选状态**
- `SlateModeOn/Off`
- `LidOpen/LidClose`（如果设备提供）
- `ThermalThrottleOn/Off`（如可接入）

**7.4 事件标准化结构**
- `SystemEventType`
- `timestamp`
- `source`（WinMsg/PowerSetting/NamedEvent）
- `payload`（如 battery level）

---

**8. SystemDetector 与 Device 的连接方式**

**8.1 推荐链路**
`SystemMonitor -> PowerPolicyController -> DeviceRuntime::SubmitCommand()`

**8.2 为什么不建议直接连接**
- `SystemMonitor -> Chip` 直接调用会导致 Host 与 Device 紧耦合。
- 后续加 VHF 或多设备支持时会爆炸式扩散依赖。

**8.3 事件-动作映射矩阵（建议默认）**
- `DisplayOff` -> `StopStreaming` + `Deinit`（强制，因芯片随灭屏掉电）
- `DisplayOn` -> `Init` + `StartStreaming`
- `Suspend` -> `StopStreaming` + `Deinit`（可配置）
- `Resume` -> `Init` + `StartStreaming`
- `BatterySaverOn` -> `SetPolicyProfile(PowerSave)`
- `BatterySaverOff` -> `SetPolicyProfile(Balanced)`
- `Shutdown` -> `Shutdown`（最高优先级）

**8.4 去抖与合并**
- 300-800ms 时间窗内合并重复事件。
- `Suspend/Resume` 期间屏幕事件降权。
- 同类连续事件只保留最后一条。

---

**9. 命令优先级与抢占策略**

**9.1 优先级**
- `P0`: `Shutdown`, `StopRuntime`, `Deinit`
- `P1`: `Suspend-related`, `Recover`
- `P2`: `EnterIdle`, `ExitIdle`, `Start/StopStreaming`
- `P3`: `Calibration`, `FreqShift`, `ScanRate`

**9.2 抢占规则**
- 高优先级命令可取消未执行低优先级命令。
- 正在执行的命令不可硬中断，除非到达可安全取消点。
- `Deinit` 到来时，清空低优先级控制命令。

---

**10. Coordinator 重构职责（保持轻量）**

**10.1 Coordinator 只保留**
- 模块生命周期管理（DeviceRuntime/SystemMonitor/Engine）
- 策略执行（事件映射）
- UI 查询入口（只读状态、下发命令）

**10.2 Coordinator 不再做**
- 手写硬件轮询
- 直接读写 `Chip::back_data`
- 直接处理底层恢复细节

---

**11. 分阶段实施计划**

**Phase 1：控制面搭建**
1. 引入 `IDeviceRuntime` + `DeviceRuntime` 壳。  
2. 接管 `Init/Deinit/Start/StopStreaming`。  
3. `send_command` 仅接入 `EnterIdle`，其余命令先保留占位回执。  
4. 保持现有 `GetFrame` 方式，先跑通状态机。  

**Phase 2：自维持采集与恢复**
1. 采集线程迁入 `DeviceRuntime`。  
2. 接入 `WaitInterrupt` 主路径。  
3. 上线恢复策略与指标，并加入 `check_bus` 判死分支（`bus dead` 直接终止自恢复）。  

**Phase 3：SystemDetector联动**
1. 定义 `ISystemMonitor` 与 `SystemEvent`。  
2. 做 `PowerPolicyController`。  
3. 联动命令映射与去抖。  

**Phase 4：稳定性增强**
1. 命令优先级与取消机制。  
2. 细化错误分类和恢复分级。  
3. 压测与长稳测试。  

---

**12. 验收标准（建议）**

1. 连续运行 24h 无死锁、无线程泄漏。  
2. 热插拔/断连恢复成功率 > 99%。  
3. 屏灭到 Idle 切换 < 500ms。  
4. 屏亮恢复并重新出帧 < 1.5s。  
5. 命令执行时序可追踪（每条命令有 id、耗时、结果）。  
6. `App` 不再直接调用 `HimaxChip::*` 控制接口。  
7. `DisplayOff` 后必须完成 `Deinit`，`DisplayOn` 后可成功 `Init + StartStreaming`。  
8. 发生 `bus dead` 时能被正确识别并输出“需重启系统”的明确诊断。  

---

**13. 风险与规避**

1. `HalDevice` 并发冲突风险高。  
规避：统一命令串行化 + 明确硬件访问锁域。  

2. 系统事件风暴导致状态抖动。  
规避：事件去抖、合并与优先级。  

3. 恢复逻辑误触发频繁重置。  
规避：分级恢复和阈值调参。  

4. UI 直接控制硬件残留导致双通道控制。  
规避：只保留 `Coordinator/DeviceRuntime` 命令入口。  

---

**14. 建议新增的核心类型清单（仅蓝图）**

- `Device/include/IDeviceRuntime.h`
- `Device/include/DeviceRuntimeTypes.h`
- `Device/include/DeviceRuntime.h`
- `Host/include/ISystemMonitor.h`
- `Host/include/SystemStateEvent.h`
- `Host/include/SystemMonitor.h`
- `App/include/PowerPolicyController.h`

---

如果你要，我下一版可以直接给你“接口草案版”（头文件级别签名设计，不写实现），你可以拿去直接开工。  
可选下一步：
1. 我先产出 `DeviceRuntime` 的完整接口签名草图。  
2. 我先产出 `SystemEvent -> DeviceCommand` 的策略表（含优先级与去抖参数）。  
3. 我先产出状态机图（Mermaid）和命令时序图。

---

**15. V1.1 决策补完（本轮已确认）**

**15.1 命令执行契约（接口级）**
- `SubmitCommand(DeviceCommand cmd)` 返回 `std::future<CommandResult>`。
- `CommandResult` 至少包含：
	- `id`
	- `status`（`Executed/Rejected/Canceled/Expired/Failed`）
	- `error`（`ChipError` 或扩展错误码）
	- `queuedAt`
	- `startedAt`
	- `finishedAt`
- `deadline` 语义：命令执行前若已过期，直接丢弃并返回 `Expired`。
- 高优先级命令允许取消“未开始执行”的低优先级命令，被取消命令回执 `Canceled`。

**15.2 帧输出契约（多消费者）**
- 主数据面采用 `TryPop`。
- `UI/Engine/DVR` 各自拥有独立 ring buffer，避免抢帧。
- 每个消费者独立维护：
	- `queueDepth`
	- `dropCount`
	- `consumerLagMs`

**15.3 SystemDetector 路线与默认策略**
- 第一版事件源改为 Win32 电源/会话 API（不依赖 Global 命名事件）。
- 默认 `DisplayOff` 动作：`StopStreaming + Deinit`。
- 默认 `Suspend` 动作：`StopStreaming + Deinit`。
- 启动默认策略：不主动 `Init`，等待首次激活事件再初始化。

**15.4 恢复预算与调试旁路**
- 重恢复（`Deinit -> Init`）预算：每 10 分钟最多 3 次。
- UI 直连 `Chip` 仅允许 `Debug` 编译开关下启用；Release 统一走 `DeviceRuntime` 命令入口。
- `check_bus == bus dead` 不计入重恢复预算，直接进入“需重启系统”终止态。

---

**16. V1.1 必补缺口（实现前写死）**

1. 写清 `StartRuntime/StopRuntime` 与 `Init/Deinit/StartStreaming` 的状态边界，禁止歧义路径。
2. 写清硬件访问锁域：哪些 API 必须串行化、哪些可并行、可安全取消点在哪里。
3. 建立 `ChipError -> 恢复层级` 映射表（轻/中/重恢复触发条件与重置条件）。
4. 建立 `SystemEvent` 标准化映射表（原始 Win32 事件 -> 规范事件 -> 优先级）。
5. 明确事件冲突处理顺序（如 `DisplayOff` 与 `Suspend/Resume` 同窗冲突）。
6. 在文档中定义命令去重规则（同类命令合并窗口、同 key 覆盖策略）。
7. 补齐可执行测试矩阵（单元、故障注入、长稳、事件风暴、恢复预算耗尽场景）。
8. 明确 `NotImplemented` 命令清单与 UI/策略层降级行为（避免误触发未定义硬件动作）。

---

**17. 仍待拍板问题（阻塞实现）**

1. `P0` 命令是否受 `deadline` 约束（建议：`P0` 不过期，仅在系统终止态拒绝）。
2. `Recovering` 状态是否接受未来扩展控制命令（建议：统一拒绝并回执 `Rejected`）。
3. 首次激活后“何时宣告 Streaming”：
	- `StartStreaming` 发出即宣告，还是
	- 第一帧成功后再宣告（建议后者）。
4. 事件乱序容错策略：`LastStateWins` 还是严格顺序回放（建议 `LastStateWins`）。
5. 是否保留 Global 命名事件兼容层作为 fallback（建议：V1 不启用，留可插拔适配层）。
