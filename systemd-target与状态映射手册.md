# phosphor-state-manager 映射手册：状态属性 ↔ transition ↔ systemd target

> 这份是**字段级速查手册**：把四层状态机的「状态属性取值」「transition 请求取值」以及它们
> 各自映射到哪个 systemd target 逐条列全，每条带源码行号。
>
> 配合另外两份看：`状态管理业务详解.md`（业务是什么）、`四层状态机转换图.html`（可交互图）。
> 所有 target 名里的 `N` 是 host/chassis 实例号（单路服务器通常是 0）。

---

## 1. 三层状态机的两张表（核心映射）

每个 manager 都遵循同一个模式，记住这一句就懂半个仓库：

> **transition setter = 拉 systemd target；Current 状态 = systemd job 结果回填。**

下面按层把两张表（transition→target、target→状态）列全。

### 1.1 Host 层（`host_state_manager.cpp`）—— 最核心

**表 A：transition 请求 → 拉起的 target**（`createSystemdTargetMaps()`，`host_state_manager.cpp:91-130`）

| transition 请求 | target（`N`=实例号） | 源码行 | 说明 |
|---|---|---|---|
| `Off` | `obmc-host-shutdown@N.target` | :101 | 关机 |
| `On` | `obmc-host-start@N.target` | :102 | 开机 |
| `Reboot` | `obmc-host-reboot@N.target` | :103 | 重启 |
| `GracefulWarmReboot` | `obmc-host-warm-reboot@N.target`（`ENABLE_WARM_REBOOT=1`）<br>否则 `obmc-host-reboot@N.target` | :110-122 | 优雅热重启 |
| `ForceWarmReboot` | `obmc-host-force-warm-reboot@N.target`（`ENABLE_WARM_REBOOT=1`）<br>否则 `obmc-host-reboot@N.target` | :114-126 | 强制热重启（仅 `ENABLE_FORCE_WARM_REBOOT` 时加入允许集，:84-87） |

> 📌 编译期开关 `ENABLE_WARM_REBOOT` 决定热重启是否真的走独立 target；不支持的平台
> 会被映射到普通 `obmc-host-reboot@N.target`。这是 C++ 编译期分支，不是运行时配置。

允许提交哪些 transition 由 `setupSupportedTransitions()` 决定（`host_state_manager.cpp:76-89`）：
默认 `{On, Off, Reboot, GracefulWarmReboot}`，`ForceWarmReboot` 视编译开关。

**表 B：完成的 target → 回填的状态属性**（`sysStateChangeJobRemoved()`，`host_state_manager.cpp:269-317`）

| 完成的 target | 回填 | 附带动作 | 源码行 |
|---|---|---|---|
| `obmc-host-stop@N.target` | `CurrentHostState = Off` | `BootProgress=Unspecified`、`OperatingSystemState=Inactive`、删 running 标志文件 | :279-287 |
| `obmc-host-startmin@N.target` | `CurrentHostState = Running` | 删 running 标志文件 | :288-301 |
| `obmc-host-quiesce@N.target` | `Quiesced` 或触发 Reboot | `AutoReboot` 真 → 请求 `Transition::Reboot`；否则置 `Quiesced` | :302-316 |

另外在 `sysStateChangeJobNew()`（target 刚开始时，:319-345）：
- `obmc-host-diagnostic-mode@N.target` → `CurrentHostState = DiagnosticMode`（:328-332）
- `obmc-host-crash@N.target` 且当前是 Running → 递减 reboot count（:333-345）

**表 C：Host 状态枚举取值**（sdbusplus 生成的 `HostState`，本仓库内使用的）

| 取值 | 业务含义 | 出现位置 |
|---|---|---|
| `Off` | 主机未运行 | :63, :283 |
| `Running` | OS 正常运行 | :57, :293 |
| `Quiesced` | 异常停摆态 | :314 |
| `DiagnosticMode` | 诊断模式 | :331 |
| `Standby` | 虚拟化固件已起（Hypervisor 层置位） | hypervisor_state_manager.cpp:66 |

### 1.2 Chassis 层（`chassis_state_manager.cpp`）

**表 A：transition → target**（`createSystemdTargetTable()`，`chassis_state_manager.cpp:59-67`）

| transition | target | 源码行 | 说明 |
|---|---|---|---|
| `Off` | `obmc-chassis-hard-poweroff@N.target` | :63 | 用**硬断电** target，确保立即执行 |
| `On` | `obmc-chassis-poweron@N.target` | :64 | 上电 |
| `PowerCycle` | `obmc-chassis-powercycle@N.target` | :65 | 先断后通 |

（另有 `obmc-chassis-poweroff@N.target`:43、`obmc-chassis-blackout@N.target`:48 用于状态判定/异常，不直接由 transition 表触发。）

**表 B：完成的 target → 状态**（`sysStateChange()`，`chassis_state_manager.cpp:530-582`）

| 完成的 target | 回填 | 源码行 |
|---|---|---|
| `obmc-chassis-poweroff@N.target` | `CurrentPowerState = Off` + 记录变更时间 | :552-559 |
| `obmc-chassis-poweron@N.target` | `CurrentPowerState = On` + 记录变更时间 + 删 on 标志文件 | :560-579 |

**表 C：PowerState 枚举取值**：`On` / `Off`（接口还定义了过渡态 `TransitioningToOn` / `TransitioningToOff`）。

### 1.3 BMC 层（`bmc_state_manager.cpp`）

**表 A：transition → target**（`SYSTEMD_TABLE`，`bmc_state_manager.cpp:39-40`）

| transition | target | 源码行 |
|---|---|---|
| `Reboot` | `reboot.target` | :40 |
| `HardReboot` | 不查表，直接调 systemd 的 `Reboot` 方法 | :143-165 |

`HardReboot` 特殊流程（`executeTransition()`，:139-165）：先置 `BMCState=NotReady`（:151）、
`stateSignal.reset()` 停掉状态监听（:152），再调 `Reboot`，意图是让 Redfish 客户端停止通信、
避免启动时产生无意义的事件日志。

**表 B：状态发现**（`discoverInitialState()`，`bmc_state_manager.cpp:112-137`，靠读 target ActiveState 而非 JobRemoved）

| 探测结果 | 置为 | 源码行 |
|---|---|---|
| `obmc-bmc-service-quiesce@0.target` active | `Quiesced` | :115-120 |
| 否则 `multi-user.target` active | `Ready` | :124-128 |
| 否则 | `NotReady` | :130-134 |

**表 C：BMCState 枚举取值**：`NotReady` / `Ready` / `Quiesced`。

---

## 2. transition setter 的校验字段（三层同构）

写 transition 属性时，各层都会做类似的校验。字段/编译开关逐条列出：

| 校验项 | Host | Chassis | BMC |
|---|---|---|---|
| 入口函数 | `requestedHostTransition()` :390 | `requestedPowerTransition()` :584 | `requestedBMCTransition()` :236 |
| BMC 就绪校验（非 Off 请求） | `ONLY_ALLOW_BOOT_WHEN_BMC_READY` → 抛 `BMCNotReady` :395-403 | 同开关 :589-600 | — |
| 固件升级校验（非 Off 请求） | `CHECK_FWUPDATE_BEFORE_DO_TRANSITION` → 抛 `Unavailable` :412-423 | 同开关 :602-615 | — |
| 重启计数递减（非 Off 请求） | `decrementRebootCount()` :425 | — | — |
| 真正执行 | `executeTransition()` :428→:142 | `startUnit()` :625 | `executeTransition()` :139 |
| 写回属性 + 持久化 | `server::…(value)` + `serialize()` :430-432 | 写回属性 :626 | 写回属性 :256 |

> 📌 关键：**Off 请求总是被允许**（绕过 BMC-ready 和 FW-update 校验），
> 因为"断电"是安全操作，必须永远能执行。三层的校验条件都是 `value != Off`。

---

## 3. D-Bus 接口与属性字段速查

状态属性都通过 sdbusplus 从 YAML 接口定义生成（定义在 `phosphor-dbus-interfaces`，不在本仓库）。
下表是本仓库实际读写的字段：

| 对象路径 | 接口 | 属性/方法 | 类型/取值 | 本仓库读写位置 |
|---|---|---|---|---|
| `/xyz/openbmc_project/state/bmc0` | `xyz.openbmc_project.State.BMC` | `BMCState`（ro） | NotReady/Ready/Quiesced | bmc_state_manager.cpp:128/133/151 |
| 同上 | 同上 | `RequestBMCTransition`（method） | Reboot/HardReboot | bmc_state_manager.cpp:236 |
| `/xyz/openbmc_project/state/chassis0` | `xyz.openbmc_project.State.Chassis` | `CurrentPowerState`（ro） | On/Off | chassis_state_manager.cpp:557/566 |
| 同上 | 同上 | `RequestedPowerTransition`（rw） | On/Off/PowerCycle | chassis_state_manager.cpp:584 |
| `/xyz/openbmc_project/state/host0` | `xyz.openbmc_project.State.Host` | `CurrentHostState`（ro） | Off/Running/Quiesced/DiagnosticMode/Standby | host_state_manager.cpp:283/293/314/331 |
| 同上 | 同上 | `RequestedHostTransition`（rw） | On/Off/Reboot/GracefulWarmReboot/ForceWarmReboot | host_state_manager.cpp:39 |
| 同上 | 同上 | `BootProgress`（ro） | ProgressStages 枚举 | host_state_manager.cpp:284/436 |
| 同上 | 同上 | `OperatingSystemState`（ro） | OSStatus 枚举 | host_state_manager.cpp:285 |
| 同上 | 同上 | `RestartCause`（ro） | 重启原因枚举 | host_state_manager.cpp（bootProgress 附近写入） |
| 同上 | 同上 | `AllowedHostTransitions`（ro） | transition 集合 | host_state_manager.cpp:88 |
| `/xyz/openbmc_project/state/scheduled/host0` | `…ScheduledHostTransition` | 定时开机时间 | 时间戳 | scheduled_host_transition.cpp |

`BootProgress` / `OperatingSystemState` 的取值（bootprogress / osstatus 命名空间）：
- `ProgressStages`: `Unspecified` / `SystemInitComplete` / `OSRunning`（hypervisor_state_manager.cpp:63-79）
- `OSStatus`: Inactive / 各 OS 阶段

---

## 4. 开机启动时的数据文件与 service

| 文件 | 作用 | 源码位置 |
|---|---|---|
| `data/phosphor-chassis-availability-default.json` | 机箱可用性监控默认配置 | data/ |
| `data/phosphor-service-monitor-default.json` | 服务监控默认配置 | data/ |
| `data/phosphor-target-monitor-default.json` | target 监控默认配置 | data/ |
| `service_files/phosphor-discover-system-state@.service` | 开机探测并恢复状态、执行电源恢复策略 | discover_system_state.cpp:37 |

**电源恢复策略关键字段**（`discover_system_state.cpp`）：

| 字段/设置 | 含义 | 源码行 |
|---|---|---|
| `PowerRestorePolicy` | always-on / always-off / previous（读 one-time 与 user 设置） | :122-165 |
| `PowerRestoreDelay` | 恢复上电前的延迟（微秒） | :176-183 |
| `applyPowerRestoreDelay()` | 延迟执行：先 `waitBmcReady` 再 sleep | :37-49 |
| 复位原因判定 | pinhole/watchdog/cold reset 时不执行电源恢复策略 | :96/106/116 |

---

## 5. 常见"字段对不上"问题排查

| 现象 | 可能原因 | 去哪看 |
|---|---|---|
| 写了 RequestedHostTransition 但没反应 | 校验拦了（BMCNotReady / FW update）或 target 被机器层改名 | host_state_manager.cpp:390 / :91-130 |
| CurrentHostState 不更新 | 对应 target 的 JobRemoved 没来或 result≠done | host_state_manager.cpp:269 |
| 热重启 target 名和预期不符 | `ENABLE_WARM_REBOOT` 编译开关把它映射到普通 reboot | host_state_manager.cpp:108-127 |
| Chassis Off 用的 target 和 On 不对称 | Off 故意用 `hard-poweroff` | chassis_state_manager.cpp:63 |
| 开机后状态恢复错乱 | 持久化 `deserialize()` 失败或电源恢复策略 | host_state_manager.cpp:67 / discover_system_state.cpp |
| 多实例 target 号不对 | host/chassis 实例号 `N` 由启动参数 `-h` 决定 | host_state_manager_main.cpp:26 |
