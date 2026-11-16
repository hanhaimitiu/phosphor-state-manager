# host_condition_gpio 是干什么的

> 位置：`phosphor-state-manager/host_condition_gpio/`（属于 state-manager 仓库，但编译出来是**独立的可执行文件 + 独立 D-Bus 服务**）
> 产物：`/usr/bin/phosphor-host-condition-gpio`，服务 `phosphor-host-condition-gpio@.service`
> 本文所有结论都带 `文件:行号`，可以直接跳过去核对。
> 📌 路径说明：`host_condition.cpp` / `host_condition.hpp` / `host_condition_main.cpp` / `phosphor-host-condition-gpio@.service` 这四个文件都在 **`host_condition_gpio/` 子目录**里；`host_check.cpp`、`host_state_manager.cpp`、`meson.build`、`meson.options` 则在仓库根目录。

---

## 0. 先讲一个真实场景：BMC 重启后"失忆"了

想象机房里这样一幕：

1. 服务器（主机）正在跑业务，操作系统正常。
2. BMC 自己重启了——可能是运维升级 BMC 固件、可能是 BMC 崩了自动重启、也可能是打了 `reboot` 命令。
3. **主机没断电**，还在正常跑。但 BMC 是独立的小电脑，它一重启，内存里"主机当前是什么状态"这个信息就**全丢了**。
4. BMC 重新起来后，状态管理器（`host_state_manager`）要做的第一件事是：把 `CurrentHostState` 属性填成什么？填 `Off` 就错了——主机明明在跑，你告诉 Redfish/Web "主机已关机"，运维可能就去按开机键，反而把正常运行的机器搞坏。

**那 BMC 怎么知道主机还在不在跑？** 它需要去"问"。能回答这个问题的东西有一组，它们都实现了同一个 D-Bus 接口 `xyz.openbmc_project.Condition.HostFirmware`，其中就包括**本文的主角**——用一根 GPIO 硬件线来回答的 `host_condition_gpio`。

**一句话定位**：

> `host_condition_gpio` 是一个**只读 GPIO 电平、并把结果翻译成"主机固件在不在运行"（Off / Running / Unknown）的小守护进程**。它本身不做任何控制动作，只是挂到 D-Bus 上"供人查询"，真正用它做决策的是 `host_check.cpp` 和 `host_state_manager.cpp`。

用你熟悉的 x86-power-control 做对比，一眼就懂它在哪：

| | x86-power-control | host_condition_gpio |
|---|---|---|
| 对 GPIO | **写**（拉高/拉低电源线，去控制主机） | **读**（看一根线的电平，去感知主机） |
| 角色 | 手 | 眼睛 |
| 方向 | 输出（动作） | 输入（查询） |
| 是否改变硬件状态 | 会 | 不会 |

---

## 1. 它在整条链路里的位置（谁用它）

这是理解它的关键——**单独看这几十行代码看不出价值，它是一条链的第一环**：

```
BMC 启动
   │
   ▼
host_state_manager::determineInitialState()          host_state_manager.cpp:51
   │  要决定 CurrentHostState 初始值是 Running 还是 Off
   │  条件①：systemd target 是否 active（BMC 重启前的状态残留）
   │  条件②：isHostRunning(id) ← 主动去问"主机在不在跑"
   ▼
isHostRunning(id)                                     host_check.cpp:161
   │  ① 先查 chassis 电源是否 on（:173）——没电就不用问了，直接 false
   │  ② 有电 → 5 次重试（每次 sleep 1s），调 checkFirmwareConditionRunning
   ▼
checkFirmwareConditionRunning(bus)                    host_check.cpp:41
   │  用 ObjectMapper GetSubTree 找出所有实现 Condition.HostFirmware 的服务（:44-48）
   │  逐个 D-Bus Get 读 CurrentFirmwareCondition 属性（:89-100）
   │  只要有一个返回 Running → 主机在跑（:107-109）
   ▼
▲ 这里查的服务之一，就是我们的主角 ↓
   │
   ├─ IPMI 实现   （实时去问主机，主机没起来时要等 3 秒超时）
   ├─ PLDM 实现   （读缓存，快）
   └─ host_condition_gpio（本文） ← 读一根 GPIO 线
```

问到 `Running` 之后，`isHostRunning` 还会做一件事：在 `/run/openbmc/host@{id}-on` 建一个标记文件（`host_check.cpp:201-211`，路径常量在 `meson.build:71`），供其他服务直接判断"主机在跑"，不必都来查 D-Bus。回到 `determineInitialState`，条件成立就把初始状态设成 `Running`（`host_state_manager.cpp:57-58`），否则 `Off`（`:63-64`）。

> 📌 注意一个细节：`determineInitialState` 用的是 `currentHostState(..., true)` / `requestedHostTransition(..., true)`（`host_state_manager.cpp:57/58/63/64`），第二个参数 `true` 是 **skipSignal**——只更新内存里的值，**不触发一次状态迁移**。这正是"恢复状态"和"发起状态切换"的区别（Java 类比：这是给字段赋初值，不是调用状态机的 `transition()`）。

> 📌 又说一句实话：`host_check.cpp:73-80` 有一段注释说"当前有两个实现，IPMI 和 PLDM"，那是**旧注释**——`host_condition_gpio` 是后来加入的第三种实现。代码逻辑（遍历所有实现）没问题，只是注释没跟上。

---

## 2. 代码逐段拆解

### 2.1 main：一个标准的 D-Bus 守护进程

`host_condition_main.cpp`：

```cpp
if (argc == 2) { hostId = argv[1]; } else { return 0; }   // :14-21  必须有且只有一个参数（host 编号）
auto bus = sdbusplus::bus::new_default();                  // :23     连系统总线
objPathInst = HOST_GPIOS_OBJPATH + "/host" + hostId;       // :25     对象路径 /xyz/openbmc_project/Gpios/host0
busName     = HOST_GPIOS_BUSNAME + hostId;                 // :26     服务名 ...Gpio0
manager_t objManager(bus, objGroupName.c_str());          // :29     挂 ObjectManager（mapper 才能发现它）
Host host(bus, objPathInst, hostId);                       // :32     创建唯一的状态对象
bus.request_name(busName.c_str());                        // :34     占住 D-Bus 名
while (true) { bus.process_discard(); bus.wait(); }        // :36-40 事件循环：处理消息、阻塞等下一条
```

服务文件 `phosphor-host-condition-gpio@.service`：
- `Type=dbus` + `BusName=xyz.openbmc_project.State.HostCondition.Gpio%i`（`:7-8`）：systemd 靠 D-Bus 名判断服务就绪，`%i` 就是传入的 hostId（比如 `0`）。
- `ExecStart=/usr/bin/phosphor-host-condition-gpio %i`（`:6`）：把实例号传给 main。
- `Restart=always`（`:5`）：挂了自动拉起。

**Java 类比**：`while(true){ ... bus.wait(); }` 相当于 `while(true){ msg = queue.take(); handle(msg); }`——阻塞等消息的死循环；`manager_t` 相当于把服务对象注册到服务注册中心（这里就是 D-Bus 的 ObjectMapper），别人才能通过 `GetSubTree` 找到它。

### 2.2 构造函数：把 hostId 变成具体的 GPIO 线名

`host_condition.hpp:25-30`：

```cpp
Host(bus, path, hostId) : HostIntf(bus, path.c_str()), lineName("host" + hostId)
{
    scanGpioPin();   // 构造时就探测这根线到底叫什么、什么极性
}
```

`lineName` 初值是 `"host" + hostId`，比如 `host0`。接着 `scanGpioPin()` 会把它补全成真实的线名。

### 2.3 scanGpioPin：自动探测线名和极性

`host_condition.cpp:14-33`——这段代码是整个组件最有意思的地方，**它不写死线名和极性，而是自动探测**：

```cpp
if (gpiod::find_line(lineName + "-ready"))      // 找 host0-ready
{ lineName += "-ready";      isActHigh = true;  } // 找到 → 高有效
else if (gpiod::find_line(lineName + "-ready-n"))// 找 host0-ready-n
{ lineName += "-ready-n";    isActHigh = false; } // 找到 → 低有效
else { lineName = ""; }                           // 都没有 → 清空，等于"禁用自己"
```

三种约定：

| 板子上实际的线名 | 极性 | 含义 |
|---|---|---|
| `host0-ready` | 高有效（Active High） | 线为高 = 主机固件在跑 |
| `host0-ready-n` | 低有效（Active Low，名字里的 `-n` 就是 negative） | 线为低 = 主机固件在跑 |
| 两个都不存在 | — | `lineName` 清空，这个服务恒返回 `Unknown`（相当于平台没接这根线，自动让位给 IPMI/PLDM 实现） |

**为什么要支持两种？** 因为不同主板的硬件设计不一样：有的板子拉高表示 ready，有的拉低表示 ready（硬件工程师常把低有效信号命名为 `xxx_n`）。这段代码让同一份软件适配两种板子——**这正是 OpenBMC "换平台尽量改配置/接线、不改 C++" 思路的一个体现**，只不过这里是用运行时探测代替了配置文件。

### 2.4 currentFirmwareCondition：每次查询都实时读 GPIO

`host_condition.cpp:35-67`——这是 override 了 D-Bus 接口生成的 getter：

```cpp
Host::FirmwareCondition Host::currentFirmwareCondition() const override
{
    auto retVal = Host::FirmwareCondition::Unknown;       // :37  默认 Unknown
    if (!lineName.empty())                                // :42  没探测到线 → 直接返回 Unknown
    {
        auto line = gpiod::find_line(lineName);           // :44  找到线
        try
        {
            int32_t gpioVal = 0;
            line.request({lineName, gpiod::line_request::DIRECTION_INPUT,
                (isActHigh) ? 0 : gpiod::line_request::FLAG_ACTIVE_LOW}); // :50-52 申请为输入；低有效加 FLAG
            gpioVal = line.get_value();                   // :54  读电平
            line.release();                               // :55  立刻释放
            retVal = (0 == gpioVal) ? Off : Running;      // :57-58  0→Off，非0→Running
        }
        catch (std::system_error&) { error("Error when read gpio value"); } // :60-63 读失败→Unknown + 日志
    }
    return retVal;
}
```

四个必须记住的点：

1. **这是 override 的 D-Bus 属性 getter**（`host_condition.hpp:33`）。`xyz.openbmc_project.Condition.HostFirmware` 接口的 `CurrentFirmwareCondition` 属性，默认的 getter 只会返回生成类里的缓存字段；这里 override 掉，改成"每次被调用都去读硬件"。**不 override 就完全没有 GPIO 语义**——这正是我之前在那份 C++ 文档里强调的 override 机制的一次正确应用。

2. **值不是缓存的，是实时的**。`request → get_value → release` 每次调用都做一遍（`:50-55`），所以永远反映当前电平。代价是每次查询都要申请/释放 GPIO 线，这也是为什么 `host_check.cpp` 里要"PLDM 优先"（读缓存快，GPIO/IPMI 慢）。

3. **`FLAG_ACTIVE_LOW` 帮你做了极性翻转**。gpiod 加了这个标志后，`get_value()` 返回的是**逻辑值**：物理低电平会被翻成逻辑 1。所以代码里统一写 `0 → Off，非 0 → Running`（`:57-58`），对高有效和低有效两种线都成立，不需要分情况判断。这是很多人第一次读会困惑的地方。

4. **读失败返回 `Unknown`，不是 `Off`**（`:60-63` + `:37`）。这个区别很要紧：如果读不到就报 `Off`，`host_check` 可能误判"主机没在跑"；报 `Unknown` 则不会命中 `Running`（`host_check.cpp:107`），而 `host_check` 会继续问下一个实现——**优雅降级**。

### 2.5 编译开关与 D-Bus 身份

`meson.build:39-40` 把两个 option 写进 `config.h`：`HOST_GPIOS_BUSNAME`、`HOST_GPIOS_OBJPATH`；默认值在 `meson.options:88-107`：

| 项 | 默认值 | 出处 |
|---|---|---|
| 编译开关 `host-gpios` | `enabled` | `meson.options:87-92` |
| 服务名 busname | `xyz.openbmc_project.State.HostCondition.Gpio` | `meson.options:95-99`（再加 hostId → `...Gpio0`） |
| 对象根路径 objpath | `/xyz/openbmc_project/Gpios` | `meson.options:102-106`（再加 `/host0`） |
| 最终对象路径 | `/xyz/openbmc_project/Gpios/host0` | `host_condition_main.cpp:25` |
| 接口 | `xyz.openbmc_project.Condition.HostFirmware` | `host_condition.hpp:5`（sdbusplus 生成的 server 头） |
| 属性 | `CurrentFirmwareCondition`，枚举 `Off/Running/Unknown` | 实现于 `host_condition.cpp:35` |

平台如果不用这套 GPIO 机制，编译时把 `host-gpios` 关掉即可（`meson.build:378` 的 `subdir('host_condition_gpio')` 受它控制）。这时 `host_check` 照样工作，只是少了一个信息来源。

---

## 3. 一条完整查询的"血缘追踪"

把前面的碎片串成一次真实调用（BMC 重启、主机还在跑）：

```
1. host_state_manager::determineInitialState()        host_state_manager.cpp:51
       → 调 isHostRunning(0)
2. isHostRunning()                                     host_check.cpp:161
       → isChassiPowerOn(bus, 0)：Chassis.CurrentPowerState == On（:125-159）✓ 有电，继续
       → 循环 5 次：sleep 1s（:195）→ checkFirmwareConditionRunning(bus)
3. checkFirmwareConditionRunning()                     host_check.cpp:41
       → ObjectMapper.GetSubTree("/", ["xyz.openbmc_project.Condition.HostFirmware"])（:44-48）
         找出所有实现：IPMI / PLDM / Gpio
       → 对每个 (path, service) 发 D-Bus Get 读 CurrentFirmwareCondition（:89-100）
4. 轮到 Gpio 服务：phosphor-host-condition-gpio 0
       → D-Bus 调 currentFirmwareCondition()           host_condition.cpp:35
       → find_line("host0-ready") → request(INPUT) → get_value() → release()
       → 电平为高 → 返回 Running
5. host_check.cpp:107 收到 Running → return true
       → 创建 /run/openbmc/host@0-on 标记文件（:201-211）
6. 回到 determineInitialState()
       → currentHostState = Running, requestedHostTransition = On（:57-58，skipSignal=true）
```

如果第 4 步这根线没接（`scanGpioPin` 清空了 lineName），它返回 `Unknown` → 第 5 步不命中 → `host_check` 继续问下一个实现（IPMI/PLDM）。**多个实现互为备份，这就是这套设计的容错思路。**

---

## 4. Java 类比 + 容易踩的点

| C++ 里的东西 | Java 类比 | 注意 |
|---|---|---|
| `currentFirmwareCondition() override` | override 父类 getter | Java 里 getter 一般返回字段；这里每次调都**读硬件**，别当缓存看 |
| `gpiod::line` 的 request/release | try-with-resources 打开/关闭一个设备句柄 | C++ 这里手动 release（`:55`）；忘了就占着线，别的进程申请不到 |
| `FLAG_ACTIVE_LOW` | 读值时自动取反的适配器 | 返回的已经是逻辑值，不要再自己反转一次 |
| `while(true){ bus.wait(); }` | `BlockingQueue.take()` 循环 / Netty eventLoop | 阻塞等消息，不是忙等 |
| 栈上的 `bus` / `host` / `objManager` | 局部变量 | 无 GC，作用域结束即析构；但这里都是 main 里活到进程结束，没悬空风险 |

**可能让你困惑的三件事**：

1. **"为什么属性 getter 里做 I/O？"** —— Java 里 getter 应该是纯取值。但在 sdbusplus 里，getter 就是 D-Bus `Get` 属性的处理函数，写成实时读硬件是 OpenBMC 的常见手法（IPMI 实现甚至真的去和主机通信）。别按 Java 的 POJO 直觉优化它。
2. **"为什么不用信号订阅，而要被轮询？"** —— 这个服务只暴露属性、不主动发信号。是否查询、何时查询由消费方（`host_check` 只在 BMC 启动时查一次）决定。对"启动时拍个快照"的场景，轮询比订阅简单可靠。
3. **"为什么 `lineName` 为空不报错？"** —— 这是设计：平台没这根线是合法情况，返回 `Unknown` 让其他实现顶上（`:29-32` + `:42`）。

---

## 5. 排错速查

| 现象 | 先看哪里 |
|---|---|
| 服务没起来 / D-Bus 名没占住 | `systemctl status phosphor-host-condition-gpio@0`；确认 `%i` 传的是 hostId；`BusName`（service `:8`）与 `HOST_GPIOS_BUSNAME + id`（main `:26`）是否一致 |
| 属性恒为 `Unknown` | 线名对不对：`host0-ready` / `host0-ready-n`（`host_condition.cpp:19-28`）；用 `gpiodetect` / `gpioinfo` 看板子实际线名 |
| 读到的值和预期相反 | 极性搞反了：线名末尾有没有 `-n`；高有效用 `host0-ready`，低有效用 `host0-ready-n`（`:19-27`） |
| `host_check` 找不到这个服务 | 确认 `manager_t`（main `:29`）已创建、`phosphordbusinterfaces` 依赖在（meson.build）；`busctl tree xyz.openbmc_project.State.HostCondition.Gpio0` 看对象树 |
| BMC 重启后主机状态仍判错 | 这是**多方共同决策**：Chassis 电源（host_check `:173`）、5 次 mapper 重试（`:168-189`）、其他实现（IPMI/PLDM）都可能影响，别只盯 GPIO |
| 想手工验证 | `busctl get-property xyz.openbmc_project.State.HostCondition.Gpio0 /xyz/openbmc_project/Gpios/host0 xyz.openbmc_project.Condition.HostFirmware CurrentFirmwareCondition`（会实时触发一次 GPIO 读取） |
