# phosphor-state-manager 代码导读（OpenBMC 新人向）

> 面向刚转入 OpenBMC / openubmc 开发的同学。建议先了解：systemd target/job 概念、
> OpenBMC 的 power-on 流程（obmc-chassis-poweron@0.target 等标准 target）。

## 1. 这个仓库是干什么的

维护 BMC 自身、机箱（电源）、Host、Hypervisor 的**状态机**，是整个系统
"开机/关机/重启"状态的权威来源：

- 对外提供 D-Bus 状态接口：`CurrentHostState`、`CurrentPowerState`、
  `BMCState`、`RequestedHostTransition` 等（Web、Redfish、IPMI 都读这里）；
- 对内把 **systemd target 的 job 完成/失败事件翻译成状态属性**；
- 收到 transition 请求时**拉起对应的 systemd target**（如
  `obmc-chassis-poweron@0.target`），交给 systemd 管的整条服务链去做真正的上电。

一句话：**状态属性 ↔ systemd target 的双向翻译官。**

## 2. 目录结构

```
├── bmc_state_manager.*           # xyz.openbmc_project.State.BMC（BMCState 属性）
├── chassis_state_manager.*       # xyz.openbmc_project.State.Chassis（电源状态）
├── chassis_state_manager_smp.*   # 多路（SMP）机箱扩展，支持多 chassis
├── chassis_check_power_status.cpp# 开机时校正电源状态（读 power-good）
├── chassis_wait_for_smp_poweron.*# 等待 SMP 上电完成
├── host_state_manager.*          # xyz.openbmc_project.State.Host（核心，见下）
├── hypervisor_state_manager.*    # Hypervisor 状态（跟随 boot progress 变化）
├── scheduled_host_transition.*   # 定时开机（xyz.openbmc_project.State.ScheduledHostTransition）
├── host_check.cpp                # Host 状态检查工具
├── host_reset_recovery.cpp       # Host 异常后的恢复（quiesce/reboot count 处理）
├── secure_boot_check.cpp         # 安全启动检查
├── settings.cpp                  # 读 xyz.openbmc_project.Settings（如 BootMode）
├── discover_system_state.cpp     # 开机时探测系统当前状态（含电源恢复策略 applyPowerRestoreDelay）
├── host_condition_gpio/          # 用 GPIO 表达 host 条件的小守护进程
├── systemd_target_signal.cpp     # 监听 systemd JobRemoved → 分发到各状态机
├── systemd_target_parser.cpp     # 从 systemd 解析 target 依赖（Wants/After）
├── systemd_service_parser.cpp    # 解析 systemd service 文件
└── test/                         # gtest 覆盖较好，读逻辑不懂时可对照测试
```

## 3. 核心机制：systemd job → 状态属性 → transition 请求

### 3.1 状态上报（被动）

```
systemd JobRemoved 信号 (systemd_target_signal.cpp)
   → sysStateChangeJobRemoved(...)        # 各 manager 都有同名回调
      → 根据 target 名 + job 结果更新 Current*State / 发起校正
```

开机时 `discover_system_state.cpp` 探测并恢复状态（含 PowerRestorePolicy 延迟上电）。

### 3.2 状态切换（主动）

写属性的入口是 `requestedHostTransition`（host_state_manager.cpp:388），流程：

```
请求 On/Off/Reboot
 ├─ 校验：BMC 是否 Ready（可配置 ONLY_ALLOW_BOOT_WHEN_BMC_READY）
 ├─ 校验：固件是否在升级（isFirmwareUpdating，可配置）
 ├─ executeTransition()  → 真正去 systemctl start 对应 target
 ├─ 写回 RequestedHostTransition 属性
 └─ serialize()          # 状态持久化（重启后恢复用）
```

其余 manager（BMC/Chassis）同构：**transition setter = 拉起 systemd target；
Current 状态 = systemd job 结果回填。** 理解这一个模式就理解了半个仓库。

### 3.3 四个状态对象的关系（新人必读）

BMC / Chassis / Host / Hypervisor 不是并列的四个功能，而是**同一台机器上四个层次的嵌套状态机**：

```
BMC（管理者自身，Standby → Ready）
 └── Chassis（电源域：电有没有通，CurrentPowerState On/Off）
      └── Host（计算域：CPU/OS 启动运行到哪一步，Running/Off/Quiesced）
           └── Hypervisor（虚拟化层：hypervisor 软件走到哪个阶段）
```

- **Chassis 是电源域**：只回答"通电了没有、供电好不好"，不管 CPU 在干什么。
  Chassis On + Host Off = x86 语境下的"插电待机（S5）"。
- **Host 是计算/启动域**：依赖 Chassis（没电不可能 Running）。开机时序：
  拉 Chassis 电源 → Host 开始 POST → OS 起来。
- **BMC 是管理者自己**：状态与前两者独立，同时是门槛——
  `requestedHostTransition`（host_state_manager.cpp:388）在
  `ONLY_ALLOW_BOOT_WHEN_BMC_READY` 开启时会先查 `isBmcReady()`，
  BMC 未就绪直接拒绝开机（抛 `BMCNotReady`）。
- **Hypervisor 是 Host 之内的一层**：不是另一台机器，而是 Host 启动链上
  多出来的一段（主要 OpenPOWER 虚拟化平台：Host firmware → hypervisor → 分区 OS）。
  其状态靠 `bootProgressChangeEvent`（hypervisor_state_manager.cpp:90）
  从 Host 的 boot progress 推导；x86 平台通常不用。

一次完整开机即：BMC Ready → Chassis 通电 → Host boot（有 hypervisor 则经过该层）→ Host Running。

### 3.4 一个 BMC 可以控制多个 OS（多 Host 实例）

**仓库原生就是按多实例设计的**，能否真的开多个 OS 由平台层配置决定：

1. **Host 状态机按实例号实例化**：`host_state_manager_main.cpp` 接受
   `-h/--host` 参数指定 host instance id（第 26 行），据此创建
   `/xyz/openbmc_project/state/host<N>`、bus name `State.Host<N>`，
   持久化路径也按 hostId 分开（`HOST_STATE_PERSIST_PATH`）。
   跑 N 个进程实例即 host0、host1……对应的 systemd target 也带实例号
   （如 `obmc-host-start@<N>.target`；quiesce 见 host_reset_recovery.cpp:34）。
2. **Chassis 有现成的多机箱聚合器**：`ChassisSMP`（chassis_state_manager_smp.cpp:44）
   构造时传入 `numChassis`，内部为 1..N 每个机箱缓存
   PowerState/PowerStatus/Present，聚合后以一个总视图上报——
   服务于"一个 BMC 板管多个独立供电节点"的形态。
3. **上层按节点寻址**：Redfish URL 和 IPMI 的 host id 都映射到这些 `<N>` 实例。

边界：**框架支持 ≠ 平台一定有多 OS**。每个 host 实例能否独立开关，
取决于机器层把 `obmc-host-start@<N>.target` 挂到了哪些电源控制服务上
（如 x86-power-control 的 `Power@<N>.service`）。单路服务器只配 host0
就是一 BMC 管 OS；多节点/多分区机器配多个 host 实例即可一 BMC 管多个 OS。
数量由机器层 systemd 配置和 bbappend 决定。

### 3.5 Host 的特殊逻辑

- **quiesce / reboot count**：host_reset_recovery.cpp 处理 Host 异常重启次数，
  超过阈值进 Quiesced，防止无限重启循环；
- **boot progress**：hypervisor_state_manager.cpp 用 `bootProgressChangeEvent`
  跟踪 Host 启动进度（OSStart → BootComplete 等）；
- **scheduled_host_transition**：到点写一次 requestedHostTransition，实现定时开机。

## 4. D-Bus 接口速查

| 对象 | 接口 | 常用属性/方法 |
|---|---|---|
| `/xyz/openbmc_project/state/bmc0` | `xyz.openbmc_project.State.BMC` | `BMCState`（Ready 等）、`RequestBMCTransition` |
| `/xyz/openbmc_project/state/chassis0` | `xyz.openbmc_project.State.Chassis` | `CurrentPowerState`、`RequestedPowerTransition` |
| `/xyz/openbmc_project/state/host0` | `xyz.openbmc_project.State.Host` | `CurrentHostState`、`RequestedHostTransition` |
| `/xyz/openbmc_project/state/scheduled/host0` | `...ScheduledHostTransition` | 定时开机时间 |

> 对象路径末尾的 `<N>` 是实例号（host0/chassis0/...），多节点平台会有
> host1、chassis1 等更多实例（见 3.4 节）。

```bash
busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 \
  xyz.openbmc_project.State.Host CurrentHostState
```

## 5. 构建与测试

meson；会产出多个守护进程（`phosphor-bmc-state-manager`、
`phosphor-chassis-state-manager`、`phosphor-host-state-manager`、
`phosphor-scheduled-host-transition` 等，各自 main）。
`test/` 下 gtest 覆盖了 systemd 信号解析、SMP chassis、reset recovery 等，
**改逻辑前先跑 `test/test_host_state_manager.cpp` 相关用例。**

## 6. 新手学习路径

1. `host_state_manager.cpp` 的 `requestedHostTransition` + `executeTransition`
   —— 一条完整切换链路；
2. `systemd_target_signal.cpp` —— 反方向的 job 回填；
3. `discover_system_state.cpp` —— 开机如何知道"现在到底是什么状态"；
4. `host_reset_recovery.cpp` —— quiesce 机制（出问题时最先被翻的代码）。

## 7. 常见改动场景

- **开机被拒绝/卡在过渡状态**：查 transition 校验（BMCReady / FW update）、
  systemd target 是否被机器层改名；
- **新增状态**：状态枚举由 sdbusplus YAML 接口定义（`xyz/openbmc_project/State/Host.interface.yaml` 等），
  改接口要同步 phosphor-dbus-interfaces；
- **PowerRestorePolicy 不生效**：看 `discover_system_state.cpp` 的
  `applyPowerRestoreDelay` 与 settings 读取。
