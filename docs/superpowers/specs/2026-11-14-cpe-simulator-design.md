# OBUSPA 多 CPE 模拟器设计（多进程 + 多实例）

日期：2026-11-14
状态：待审批

## 1. 目的与背景

把 obuspa 改造成 CPE 模拟器：一台机器上按"进程数 × 每进程实例数"拉起大量虚拟 CPE（例如 10 × 100 = 1000），
每个 CPE 是一个功能完整的 USP Agent（含 tr181_vendor 插件数据模型），拥有独立的数据库、EndpointID、SerialNumber。

用途（用户明确要求"都要支持，尽可能多"）：
- **压测**：Controller / MQTT Broker 的连接数、上线风暴、消息吞吐。
- **功能验证**：批量上线、配置下发、Get/Set/Add/Delete/Operate、订阅通知、重启、恢复出厂等，行为与单个 obuspa 一致。

## 2. 目标 / 非目标 / 反目标

目标
- 命令行可配置进程数 `--sim-procs N` 和每进程实例数 `--sim-per-proc M`。
- 每个 CPE 身份唯一、可预测、可复现（OUI、SN 格式可预设）。
- 每个 CPE 持久化独立：重启后身份与配置保留——包括核心 DB 参数和 tr181 插件 mock 后端的参数（用户确认）。
- 每个 CPE 的 MTP 参数（ClientID、Topic、用户名/密码等）可按身份模板化生成。
- 多台压测机可并行运行而不撞号。

非目标
- 不做独立于 obuspa 的轻量协议模拟器；模拟的就是真实 obuspa 行为。
- 不提供 GUI / Web 管理界面。
- 不模拟真实硬件行为（数据由 tr181 mock 后端提供）。

反目标（技术上能跑但算失败）
- 实例之间串数据（A 的 Set 影响到 B、A 的通知以 B 的身份发出）。
- 默认构建下单 Agent 的行为或性能退化；不使用模拟器参数时，obuspa 必须与现在完全一致。
- 上线风暴失控：1000 台同一瞬间连接把自身或 Broker 打挂（必须有可控的启动节奏）。

## 3. 现状与约束

- 核心约 11.7 万行，约 60 个源文件中有约 210 个模块级全局变量（数据模型、DB 句柄、MTP 连接、订阅、队列、定时器等）。
- 单个 Agent 自身已是多线程：数据模型主线程 + 各 MTP 线程（STOMP/CoAP/MQTT/WSClient/WSServer/UDS）+ BulkData 等（见 `src/core/main.c:373-420`）。线程间通过模块静态变量 + 队列通信，因此不能简单把全局变量改为 `__thread`。
- 身份来源（`src/core/device_local_agent.c`）：OUI ← 环境变量/`VENDOR_OUI`；SN ← vendor hook / `USP_BOARD_SERIAL` / WAN MAC；EndpointID ← `USP_ENDPOINT_ID` / vendor hook / `os::<OUI>-<SN>`。DB 中已有值优先于默认值。
- 默认路径：DB `OBUSPA_LOCAL_STATE_DIR/usp.db`，CLI socket `/tmp/usp_cli`（`src/vendor/vendor_defs.h`），均需按实例区分。
- tr181_vendor 插件的 mock 后端（`plugins/tr181_vendor/src/tr181_backend_mock.c`）是进程内全局哈希表，数据纯内存。
- 主循环基于 `select()`，单进程 FD 上限 `FD_SETSIZE`（通常 1024）。阶段 2 中每进程实例数 × 每实例 fd 数必须受此约束，或改用 poll。
- 第三方库（mosquitto、sqlite、libwebsockets、curl、OpenSSL）均支持每实例独立 context。

## 4. 方案（已选 C）

| 方案 | 做法 | 取舍 |
|---|---|---|
| A 仅多进程 | supervisor fork N 个独立 Agent 进程 | 改动最小、易合上游；每 CPE 约 10–20MB + 一组线程，`per-proc` 参数无意义 |
| B 进程 × 实例 | 210 个全局收拢为 `usp_instance_t`，线程局部"当前实例"指针，一个进程跑 M 个完整 CPE | 密度最高、完全符合"10 进程 × 100"；重构量大、上游合并冲突多、需隔离测试 |
| **C 先 A 后 B（选定）** | 阶段 1 交付多进程模拟器，CLI、身份规则、目录布局、mock 每实例数据一次定好；阶段 2 做实例化重构，使 `--sim-per-proc` 真正在进程内生效 | 先拿到可用的 1000 台；风险最大的重构独立进行，且有阶段 1 作为行为基线；对外用法不变 |

用户选择 C，理由：尽快可用，且风险分离。

## 5. 对外接口（两个阶段共用，阶段 1 即定型）

### 5.1 命令行

```
obuspa --sim-procs N --sim-per-proc M [--sim-base B] [--sim-root DIR]
       [--sim-oui OUI] [--sim-sn-fmt FMT] [--sim-eid-fmt FMT]
       [--sim-ramp RATE] [其余 obuspa 原参数 -r -x -v -p -i ...]
```

| 参数 | 默认 | 含义 |
|---|---|---|
| `--sim-procs N` | 1 | 工作进程数 |
| `--sim-per-proc M` | 1 | 每进程 CPE 数（阶段 1：每个 CPE 仍是独立进程，supervisor 共拉起 N×M 个进程；阶段 2：进程内 M 个实例） |
| `--sim-base B` | 0 | 全局编号起始偏移，多台压测机用不同 B 避免撞号 |
| `--sim-root DIR` | `OBUSPA_LOCAL_STATE_DIR/sim` | 实例目录根 |
| `--sim-oui OUI` | `VENDOR_OUI` | 所有实例的 ManufacturerOUI |
| `--sim-sn-fmt FMT` | `SIM%08u` | SN 模板（printf 风格，唯一参数为全局 idx） |
| `--sim-eid-fmt FMT` | 空 → `os::<OUI>-<SN>` | EndpointID 模板，支持占位符（见 5.3） |
| `--sim-ramp RATE` | 20 | 启动节奏：每秒启动的 CPE 数（0 = 不限速） |
| `--sim-reidentify` | 关 | 接受与 `sim.conf` 不一致的身份参数并应用到已有实例（见 5.4） |

未给出任何 `--sim-*` 参数时，obuspa 行为与现在完全一致。

### 5.2 编号

- 工作进程号 `p ∈ [0, N)`，进程内实例号 `i ∈ [0, M)`。
- 全局编号 `idx = B + p × M + i`，所有身份由 idx 派生，保证可复现。
- MAC：`02:<idx 的 40 位，大端>`（本地管理地址段），供 tr181 mock 的 MACAddress 等参数和"WAN MAC"使用。

### 5.3 模板占位符

在 `--sim-eid-fmt` 与 factory reset 文件（`-r`）中可用：

| 占位符 | 值 |
|---|---|
| `${IDX}` | 全局编号（十进制） |
| `${PROC}` / `${INST}` | p / i |
| `${OUI}` | OUI |
| `${SN}` | 序列号 |
| `${EID}` | EndpointID |
| `${MAC}` / `${MACX}` | `02:00:00:00:00:2A` / `02000000002A` |

用途：为每台 CPE 生成不同的 MQTT ClientID、Username/Password、ResponseTopic 等，例如
`Device.MQTT.Client.1.ClientID "${SN}"`、`Device.LocalAgent.MTP.1.MQTT.ResponseTopicConfigured "usp/agent/${EID}"`。

### 5.4 存储布局

```
<sim-root>/
  sim.conf          # 本 sim-root 的身份参数：base、oui、sn-fmt、eid-fmt
  sim.pid           # supervisor pid
  sim.map           # idx, proc, inst, SN, EID, MAC, 目录 对照表（CSV），供 Controller 侧导入/核对
  cpe-<idx>/        # 目录按 idx 命名（idx 补零到 8 位）
    usp.db          # 该 CPE 的 sqlite DB（首次启动由展开后的 factory reset 生成）
    mock.db         # tr181 mock 后端持久化数据（见 5.5）
    reset.txt       # 展开占位符后的 factory reset 文件（每次启动重新生成，便于排查）
    cli.sock        # 该 CPE 的 CLI Unix socket
    agent.log       # 该 CPE 日志（指定 -l 为文件时；stdout/syslog 时加 [SN] 前缀）
```

- 目录按 idx 而不是 SN 命名（实现者的决定：SN 模板变化时目录不会变成孤儿；idx 才是稳定主键）。
- DB 已存在时沿用（身份与配置持久）；删除实例目录即该 CPE 恢复出厂。
- 身份参数一致性（实现者的决定）：首次运行写入 `sim.conf`。之后以不同的 oui/sn-fmt/eid-fmt/base 复用同一 sim-root 时，supervisor 报错退出并给出差异；只有加 `--sim-reidentify` 才接受新参数，这时会删除各实例 DB 中保存的 OUI/SN/EndpointID 覆盖值，让新身份生效（否则 DB 中的旧值优先，新参数会被悄悄忽略）。
- 对单个 CPE 执行 CLI：`obuspa -c -s <sim-root>/cpe-<idx>/cli.sock get Device.DeviceInfo.`（复用现有 `-s`）。

### 5.5 tr181 mock 后端

- 每个 CPE 一份数据：种子值来自 CSV，SN、MAC、ManufacturerOUI 等身份相关参数按实例改写。
- 已核实：插件自己注册了 `Device.DeviceInfo.SerialNumber`、`ManufacturerOUI`，CSV 种子值分别是 `C100023710001066` 和 `666666`；插件没有注册 SN/EndpointID 的 vendor hook，所以核心会使用环境变量 `USP_BOARD_SERIAL`。不改写的话，Controller Get 到的 SN 对所有 CPE 都一样，并且和 EndpointID 对不上。因此这两个参数必须由 sim 身份改写（MACAddress 类参数同理）。
- **持久化（用户确认要保留）**：mock 的 Set/AddInstance/DeleteInstance 写入实例目录下的 `mock.db`，启动时先载入 CSV 种子值，再叠加 `mock.db` 中的值。只读的动态参数（统计计数等）不持久化。存储格式由实现者决定（建议 sqlite，与核心一致，或者追加写的 KV 文件）。
- FactoryReset 时 `mock.db` 与 `usp.db` 一起清空。
- 非模拟模式（单个 obuspa + 插件）同样受益于持久化；持久化文件路径默认放在 DB 同目录。

### 5.6 Supervisor 行为

- 前台运行，按 `--sim-ramp` 节奏启动全部 CPE；子进程异常退出时自动重启（带退避，防止崩溃死循环）。
- 区分退出原因（实现者的决定）：目前 Reboot、FactoryReset、CLI `stop` 都以 `exit(0)` 结束（`src/core/dm_exec.c:1702-1754`）。在模拟模式（环境变量 `USP_SIM_IDX` 存在）下改为不同的退出码：Reboot = 10、FactoryReset = 11、stop = 0。非模拟模式行为不变。
  - Reboot：立即重启该 CPE，不计入异常。
  - FactoryReset：同上；`usp.db`、`mock.db` 的清理沿用核心现有的 factory reset 流程，`mock.db` 由插件在 FactoryReset 时自行清除。
  - stop（CLI）：该 CPE 保持停止，不再拉起。
  - 其他退出码或信号：视为崩溃，带退避重启。
- SIGINT/SIGTERM：向所有子进程转发并等待退出。
- 启动时写 `sim.map`，并周期性打印汇总（存活数 / 重启次数）。

## 6. 阶段 1：多进程

- 在 `main()` 中识别 `--sim-*`，进入 supervisor 模式；supervisor 计算每个 CPE 的身份，生成目录与展开后的 reset 文件，然后 fork + exec 自身（不带 `--sim-*`），通过环境变量
  `USP_BOARD_OUI / USP_BOARD_SERIAL / USP_ENDPOINT_ID / USP_SIM_IDX / USP_SIM_MAC`（新增后两者）以及 `-f -r -s -l` 参数传入身份与路径。
- 核心改动仅限：main 中的 supervisor 入口、新增模块 `src/core/sim_supervisor.c`、身份获取处读取 `USP_SIM_MAC`（用于替代 WAN MAC）、`dm_exec.c` 在模拟模式下使用区分原因的退出码（见 5.6）。
- tr181 mock 后端在 `TR181_BE_Init()` 读取 `USP_SIM_*` 环境变量改写身份相关种子值，并实现 `mock.db` 持久化（见 5.5）。
- 阶段 1 中 N×M 个 CPE 各占一个进程；`--sim-per-proc` 只影响编号与分组，不影响进程内实例数。

验收：N×M 个 CPE 全部以各自身份连上 Controller；`sim.map` 与 Controller 看到的设备一一对应；重启 supervisor 后身份与配置不变；不带 `--sim-*` 时行为不变。

## 7. 阶段 2：进程内多实例

- 编译开关（实现者的决定，用来满足反目标"默认构建不退化"）：多实例支持由 `-DUSP_MULTI_INSTANCE`（CMake 选项 `ENABLE_MULTI_INSTANCE`，默认 OFF）控制。OFF 时访问宏展开为原来的普通全局变量，代码和性能与现在完全一致；只有 ON 时才走实例指针。阶段 1 的多进程模式在两种构建下都可用；`--sim-per-proc M>1` 时，OFF 构建退化为阶段 1 的行为（每个 CPE 一个进程），并打印一条提示。
- 引入 `usp_instance_t`：各模块的全局变量收拢为模块级子结构（`inst->dm`、`inst->db`、`inst->mqtt` …）。
- 每个实例拥有自己的一组线程（数据模型线程 + 所需 MTP 线程 + BDC）；实例内每个线程启动时设置线程局部指针 `usp_cur_inst`；模块代码通过宏访问（如 `G(mqtt).clients`）以做半机械化替换。
- 进程级真正共享的只有：OpenSSL 初始化、插件 `.so` 代码、日志后端、只读的 schema 数据（可选优化）。
- vendor 插件接口（用户确认：只需保证 tr181_vendor）：新增可选的每实例上下文接口（例如 `USP_GetInstanceContext()` / `USP_SetInstanceContext()`），旧接口不变，保持向后兼容。在多实例构建中，只有声明支持多实例的插件（tr181_vendor）可以用于 M>1；其他插件在 M>1 时由 supervisor 拒绝启动并给出提示，单实例模式下照常工作。
- 插件 `.so` 每个进程加载一次，`VENDOR_Init/Start/Stop` 对每个实例各调用一次（在该实例的线程上下文中），插件通过实例上下文接口取得自己那一份状态。
- **致命错误只停该 CPE（用户确认）**：核心中的 `USP_ASSERT`、`exit()` 以及"致命错误终止"类路径，在多实例构建中改为终止当前实例：通知该实例的所有线程退出，释放该实例的资源（fd、DB 句柄、MTP 连接、定时器），然后由进程内的实例管理器按 5.6 的规则（Reboot/FactoryReset/stop/崩溃退避）重新拉起或保持停止。其他实例不受影响。
  - 实例的退出必须可靠：每个实例线程有可检查的停止标志，阻塞点（select、队列等待）可以被唤醒。
  - 实例内存泄漏不得累积：实例反复崩溃重启 1000 次后，进程 RSS 不应持续增长（作为验收项）。
  - 兜底：进程级的不可恢复错误（如内存耗尽、信号 SIGSEGV）仍然导致整个工作进程退出，由 supervisor 拉起。这时同进程的 M 个 CPE 一起掉线，按 `--sim-ramp` 节奏重连。
- FD 上限：每进程实例数 × 每实例 fd 数需小于 `FD_SETSIZE`，否则改用 `poll()`。
- supervisor 不变：每个工作进程拉起 M 个实例，对外用法与阶段 1 一致。

验收：
- 阶段 1 的全部验收项在阶段 2 下通过。
- 新增实例隔离测试：并发 Set、订阅、通知、重启、单实例致命错误，校验无串数据，且其他实例不受影响。
- 内存（实现者的决定）：1000 个 CPE 全部连上、空闲 5 分钟后测总 PSS（Linux `smem` 或 `/proc/*/smaps_rollup`），阶段 2（10×100）不超过阶段 1 的 1/3。达不到目标时，再做共享只读 schema 的优化。
- `ENABLE_MULTI_INSTANCE=OFF` 构建与改造前的 obuspa 功能一致（跑现有测试）。

## 8. 交给实现者决定的部分

- 模板展开的具体实现、`sim.map` 列格式细节。
- 子进程崩溃重启的退避参数。
- 阶段 2 中全局变量迁移的顺序与宏的具体形式。
- 汇总打印的格式与频率。
