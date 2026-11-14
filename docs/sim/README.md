# OBUSPA CPE 模拟器（阶段 1）

`obuspa --sim-procs N --sim-per-proc M ...` 启动一个 supervisor，fork 出 N×M 个 obuspa 子进程。
每个子进程是一台身份唯一、数据持久化的虚拟 CPE（独立的序列号、EndpointID、MAC、usp.db、mock.db、CLI socket）。
设计文档：`docs/superpowers/specs/2026-11-14-cpe-simulator-design.md`。

## 1. 快速开始

构建（Linux；依赖见 `test/sim/docker/Dockerfile`）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
make -C plugins/tr181_vendor            # 生成 plugins/tr181_vendor/libtr181_vendor.so
```

启动 2×5 = 10 台 CPE，连接到本机 MQTT broker（先编辑 `test/sim/mqtt_tmpl.txt` 里的 `BrokerAddress`/`BrokerPort`）：

```sh
build/obuspa --sim-root /tmp/simroot --sim-procs 2 --sim-per-proc 5 \
    --sim-reset test/sim/mqtt_tmpl.txt -x plugins/tr181_vendor/libtr181_vendor.so
```

supervisor 在前台运行，每个子进程启动打印 `sim: launch idx=<idx> pid=<pid>`，之后每 10 秒打印一行汇总：

```
sim: running=10/10 stopped=0 pending=0 restarts=0 crashes=0
```

Ctrl-C / SIGTERM：10 秒内所有子进程退出（不留孤儿），supervisor 返回 0。

对某一台 CPE 执行 CLI 命令（**选项必须放在 `-c` 之前**，`-c` 立即执行）：

```sh
R=/tmp/simroot; I=$(printf %08d 3)
build/obuspa -s $R/cpe-$I/cli.sock -f $R/cpe-$I/usp.db -c get Device.DeviceInfo.SerialNumber
build/obuspa -s $R/cpe-$I/cli.sock -f $R/cpe-$I/usp.db -c get Device.MQTT.Client.1.Status
build/obuspa -s $R/cpe-$I/cli.sock -f $R/cpe-$I/usp.db -c operate "Device.Reboot()"
```

## 2. 命令行参数

所有 `--sim-*` 选项也接受 `--sim-x=value` 和单横线 `-sim-x` 写法。出现任一 `--sim-*` 即进入模拟器模式；
其余参数（`-x`、`-v`、`-p` 等）原样转发给每个子进程。

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--sim-procs N` | 1 | 子进程总数基数（N >= 1）。总实例数 = N × per-proc |
| `--sim-per-proc M` | 1 | 每个 proc 的实例数（M >= 1）。当前每个实例是一个独立的 obuspa 进程 |
| `--sim-base K` | 0 | 全局索引起点：`idx = K + proc*M + inst`。多机部署时用它划分区间 |
| `--sim-root DIR` | `<localstatedir>/obuspa/sim`（编译时 `OBUSPA_LOCAL_STATE_DIR` + `/sim`） | 所有数据所在目录 |
| `--sim-oui OUI` | `012345`（`VENDOR_OUI`） | 厂商 OUI，用于 `${OUI}`、默认 EndpointID、`USP_BOARD_OUI` |
| `--sim-sn-fmt FMT` | `SIM%08u` | 序列号 printf 格式，必须恰好含一个 `%[flags][width]d/i/u/x/X` 转换；结果不得含空白，长度受限 |
| `--sim-eid-fmt FMT` | 空 | EndpointID 模板（可用 `${SN} ${IDX} ${MAC} ${MACX} ${OUI} ${PROC} ${INST}`，不可用 `${EID}`），必须含 `${SN}/${IDX}/${MAC}/${MACX}` 之一。为空时使用 `os::<OUI>-<SN>`（OUI、SN 做 percent-encode） |
| `--sim-ramp R` | 20 | 启动限速（个/秒，令牌桶，突发量 = max(R,1)）；0 = 不限速。**所有**启动（含重启、崩溃重试）都经过该限速器 |
| `--sim-reset FILE` | 空 | 恢复出厂模板文件（文本格式同 `-r`），每台 CPE 展开占位符后写入 `<dir>/reset.txt` 并以 `-r` 传给子进程。模拟模式下用它代替 `-r` |
| `--sim-log stdout\|syslog\|file` | `stdout` | 子进程日志去向：`stdout` 与 supervisor 共用（行首带 `[<SN>]` 标签）、`syslog`（`-l syslog`）、`file`（`<dir>/agent.log`）。模拟模式下用它代替 `-l` |
| `--sim-reidentify` | 关 | 见第 5 节 |

**与 `--sim-*` 冲突的选项**（出现即报错并返回 2）：`-f`、`-s`、`-r`、`-l`、`-c` 及其长格式
（`--dbfile` `--cli` `--resetfile` `--log` `--command`，单横线写法亦然）。`-f`/`-s` 由 supervisor 自动指定到每个实例目录。

其它规则：

- `--sim-procs`/`--sim-per-proc` 在已有 root 上修改是允许的：身份只取决于 `idx`，不依赖 procs/per-proc。
- 所有参数、全部实例的身份、以及模板对所有实例的展开，都在创建/删除任何文件之前验证；失败返回 2 且不改动磁盘。
- 同一个 `--sim-root` 只能有一个 supervisor（`sim.pid` + `flock`），第二个报 `another supervisor` 并返回 2。
- `-x` 指向不存在的插件**不会**让 obuspa 崩溃（只记日志后继续运行），所以这不是崩溃循环的触发方式。

## 3. 目录布局

```
<root>/
  sim.pid            supervisor 的 pid（flock 保证唯一）
  sim.conf           身份参数快照：base / oui / sn_fmt / eid_fmt
  sim.map            表格 idx,proc,inst,sn,eid,mac,dir（每次启动重写）
  cpe-00000000/      idx 以 8 位十进制补零
    usp.db           obuspa 数据库
    mock.db          tr181_vendor 插件的持久化（sqlite，TR181_MOCK_DB 指向它，supervisor 自动设置）
    reset.txt        展开占位符后的恢复出厂文件（仅在给了 --sim-reset 时）
    cli.sock         CLI socket
    agent.log        （仅 --sim-log file）
```

每个子进程的环境变量由 supervisor 设置：`USP_SIM_IDX`（让退出码生效）、`USP_SIM_MAC`、`USP_SIM_TAG`（= SN，日志前缀）、
`USP_BOARD_SERIAL`、`USP_BOARD_OUI`、`USP_ENDPOINT_ID`、`TR181_MOCK_DB`。插件侧说明见 `plugins/tr181_vendor/README.md`。

## 4. 身份与占位符

| 项 | 规则 |
|---|---|
| idx | `base + proc*per_proc + inst`，上限 0xFFFFFFFF |
| SN | `sn-fmt` 以 `idx` 格式化（默认 `SIM00000100` 对应 idx 100） |
| EID | 默认 `os::<OUI>-<SN>`，如 `os::012345-SIM00000100`；或由 `eid-fmt` 展开 |
| MAC | 6 个字节依次为 `02, idx>>24, idx>>16, idx>>8, idx&0xFF, 00`，例：idx 256 -> `02:00:00:01:00:00`。插件把它的前 5 字节用于所有 `*MACAddress` 种子，最后一字节保留种子原值（idx 100 的 Ethernet 接口 MAC 为 `02:00:00:00:64:53`） |

模板文件（`--sim-reset`）中可用的占位符（未知占位符、未闭合的 `${` 都会在启动前报错并指出文件行号）：

`${IDX}` `${PROC}` `${INST}` `${OUI}` `${SN}` `${EID}` `${MAC}`（带冒号）`${MACX}`（无冒号）

`test/sim/reset_tmpl.txt` 是最小示例（集成测试使用）；`test/sim/mqtt_tmpl.txt` 是 MQTT 版本，
基于仓库根目录的 `mqtt_factory_reset_example.txt`：

```
Device.MQTT.Client.1.BrokerAddress "127.0.0.1"      # <- 改成你的 broker
Device.MQTT.Client.1.BrokerPort "1883"              # <- 改成你的端口
Device.MQTT.Client.1.ClientID "${SN}"
Device.LocalAgent.MTP.1.MQTT.ResponseTopicConfigured "usp/agent/${EID}"
Device.LocalAgent.Controller.1.EndpointID "self::usp-controller"
```

使用前请直接编辑该文件（或复制一份）里的 `BrokerAddress`/`BrokerPort`/账号密码；集成测试 `it_mqtt_connect`
会用 `MQTT_BROKER=host:port` 生成一份临时副本。

## 5. 生命周期、退出码与 `--sim-reidentify`

子进程退出码（模拟模式下）：

| 退出码 | 触发 | supervisor 动作 |
|---|---|---|
| 0 | CLI `stop` | 保持停止（不再拉起；汇总里 `stopped` 加一） |
| 10 | `Device.Reboot()` | 重新拉起（保留 usp.db / mock.db） |
| 11 | `Device.FactoryReset()` | 删除该实例的 `mock.db`，核心重置 `usp.db`，重新拉起 |
| 其它 / 被信号杀死 | 崩溃 | 退避重启：1、2、4…最长 60 秒；子进程运行满 60 秒后连续崩溃计数清零 |

所有拉起都经过 `--sim-ramp` 限速。

`--sim-reidentify`：supervisor 启动时比较 `sim.conf` 与当前的 base / oui / sn_fmt / eid_fmt。

- 一致：什么也不做（打印一行提示）。
- 不一致且**没有**该选项：报错返回 2（stderr 含不同的字段名，如 `sn_fmt`），不改动任何文件。
- 不一致且有该选项：对**所有**实例恢复出厂（删除每个 `cpe-*` 下的 `usp.db` 和 `mock.db`），重写 `sim.conf`。

校验（身份 + 模板展开）总是先于任何删除操作。

## 6. 多机部署

用 `--sim-base` 给每台机器分配不重叠的 idx 区间，SN/EID/MAC 都由 idx 决定，因此不会冲突：

```sh
# 机器 A：idx 0..999        机器 B：idx 1000..1999
A$ obuspa --sim-base 0    --sim-procs 10 --sim-per-proc 100 ...
B$ obuspa --sim-base 1000 --sim-procs 10 --sim-per-proc 100 ...
```

各机器的 `--sim-sn-fmt` / `--sim-oui` / `--sim-eid-fmt` 必须一致。MAC 的 idx 区间用完 24 位前不会重复（前缀固定 `02`，
同一二层网段内多机共享时请注意 idx 不重叠）。

## 7. 系统调优

supervisor 启动时检查 `RLIMIT_NPROC` 与 `RLIMIT_NOFILE`，低于 `总数×8` / `总数+64` 时打印警告（不退出）。每个 CPE 约 8 个线程。

```sh
ulimit -n 65536                 # 打开文件数（supervisor 与子进程各自继承）
ulimit -u 200000                # 进程/线程数，需 >= CPE 数 × 8 + 余量
sysctl -w kernel.pid_max=4194304            # 线程数多时默认 32768 可能不够
sysctl -w kernel.threads-max=2000000
sysctl -w fs.inotify.max_user_instances=8192  # 若出现 "Too many open files" 来自 inotify
sysctl -w fs.file-max=2000000
```

systemd 下用 `LimitNOFILE=`、`LimitNPROC=`、`TasksMax=infinity`；容器里用 `--pids-limit -1 --ulimit nofile=...`。
同一个 broker 的连接数上限（mosquitto `max_connections`、系统 `ulimit -n`、`net.core.somaxconn`）也要调大。

## 8. 1000 台 CPE 运行手册（Linux 主机）

```sh
# 1. 调优（见第 7 节），并确认 ≥ 6 GiB 空闲内存、≥ 4 核
# 2. 编辑模板里的 broker
$EDITOR test/sim/mqtt_tmpl.txt
# 3. 启动（20 秒左右全部拉起；ramp 50 个/秒）
build/obuspa --sim-root /var/lib/cpesim --sim-procs 10 --sim-per-proc 100 --sim-ramp 50 \
    --sim-reset test/sim/mqtt_tmpl.txt -x plugins/tr181_vendor/libtr181_vendor.so > sim.log 2>&1 &
# 4. 等待 running=1000/1000
until grep -q 'running=1000/1000' sim.log; do sleep 1; done
# 5. 量内存：obuspa 进程 PSS 之和（smem 或 smaps_rollup）
smem -c "name pss" -P obuspa        # 或：
for p in $(pgrep -f /var/lib/cpesim/cpe-); do awk '/^Pss:/{print $2}' /proc/$p/smaps_rollup; done | awk '{s+=$1} END{print s/1024 " MiB"}'
# 6. 线程数（抽样）：ls /proc/<pid>/task | wc -l
# 7. broker 侧连接数：ss -tn state established '( sport = :1883 )' | tail -n +2 | wc -l
```

## 9. 基线（阶段 1，供阶段 2 对照）

环境：Colima VM（4 CPU / 8 GiB），Docker 容器内 Debian stable，obuspa **Release** 构建
（`cmake -DCMAKE_BUILD_TYPE=Release`），插件 `libtr181_vendor.so`（mock 后端，`TR181_MOCK_DB` 持久化开启），
broker 是同一容器内的 mosquitto（`mosquitto -d -p 1883`），容器使用 `--pids-limit -1 --ulimit nofile=1048576`。
启动参数：`--sim-procs 10 --sim-per-proc P --sim-ramp 50 --sim-reset test/sim/mqtt_tmpl.txt`（broker 地址 127.0.0.1:1883）。
测量在全部 CPE 稳定运行约 30 秒后进行；PSS 为各 obuspa 进程 `/proc/<pid>/smaps_rollup` 的 `Pss:` 之和（不含 supervisor 和 broker）。

| CPE 数 | 到 running=N/N | 总 PSS | PSS/CPE | 线程/CPE | broker ESTABLISHED 连接 | 状态为 Connected 的 CPE |
|---|---|---|---|---|---|---|
| 300（10×30） | ≈ 10 s | 1268 MiB（1,298,240 kB） | 4.2 MiB | 8 | 300 | 300/300 |
| 500（10×50） | ≈ 10 s | 2104 MiB（2,154,620 kB） | 4.2 MiB | 8 | 500 | 500/500 |
| **1000（10×100）** | ≈ 20 s | 4195 MiB（4,295,408 kB） | 4.2 MiB | 8 | **1000** | **1000/1000** |

说明：

- 1000 台是**实测**值，不是外推：VM 内存 7922 MiB，运行时已用 5132 MiB（含 supervisor 约 6 MiB PSS、mosquitto 与页缓存），空闲 742 MiB，可用 2790 MiB。
  这是该 VM 的极限附近，没有余量再叠加更多负载。
- “到 running=N/N”由 supervisor 每 10 秒一次的汇总行得出，精度为 ±10 s；受 `--sim-ramp 50` 限制，理论下限 = N/50 秒（300 台 6 s，1000 台 20 s）。
  1000 台时首次 `running=1000/1000` 汇总出现在约 20 秒，随后几乎所有 CPE 同时连接 broker，负载均值短暂冲到约 77（4 核）。
- PSS 与规模基本线性（约 4.2 MiB/CPE，其中共享库按进程数均摊），线程 8/CPE。按此外推：2000 台约 8.4 GiB、
  5000 台约 21 GiB、线程数 8/CPE（5000 台 = 4 万线程）；这是**线性外推，未实测**。
- 其中每进程 RSS 约 18 MB（单进程视角，共享页重复计入）。阶段 2（多实例同进程）的目标是把 PSS/CPE 降到明显低于该基线。

## 10. 测试

```sh
test/sim/it_sim.sh build/obuspa plugins/tr181_vendor/libtr181_vendor.so              # 12 个用例，it_mqtt_connect 无 broker 时跳过
MQTT_BROKER=127.0.0.1:1883 test/sim/it_sim.sh build/obuspa plugins/tr181_vendor/libtr181_vendor.so it_mqtt_connect
```

`it_mqtt_connect`：仅在设置了 `MQTT_BROKER=host:port` 时运行，用 `mqtt_tmpl.txt`（broker 替换为该值）启动 2×5 台 CPE，
60 秒内每台的 `Device.MQTT.Client.1.Status` 必须为 `Connected`。

macOS 上无法构建 obuspa，使用 `test/sim/docker/run.sh '<命令>'`（Debian 镜像，内置 mosquitto）。

## 11. 已知限制

- 冲突选项检测只匹配完整 token 或 `token=` 形式；附着写法（如 `-lfile`、`-fusp.db`）不会被发现。
- supervisor 运行期间替换 obuspa 可执行文件会导致之后的重新拉起失败（`/proc/self/exe` 变成 deleted）；升级后请重启 supervisor。
- 每个 CPE 是一个独立的 obuspa 进程（约 4.2 MiB PSS、8 线程），规模受内存和线程数限制；同进程多实例属于阶段 2。
- 恢复出厂（退出码 11）只删除 `mock.db`，核心自己重置 `usp.db`；若 `-x` 的插件不存在，只会没有 mock 数据，不会崩溃。
- 别名（Alias）一旦设置核心不允许修改；持久化类测试因此使用 `Device.LocalAgent.Controller.1.EndpointID`。
