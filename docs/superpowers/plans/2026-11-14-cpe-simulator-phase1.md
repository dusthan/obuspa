# CPE 模拟器 阶段 1（多进程）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `obuspa --sim-procs N --sim-per-proc M ...` 启动一个 supervisor，按节奏拉起 N×M 个独立 obuspa 进程，每个进程是身份唯一、数据持久的虚拟 CPE。

**Architecture:** `main()` 最开始检测到 `--sim-*` 参数就进入 `SIM_Main()`（supervisor）。supervisor 为每个 CPE 计算身份、准备 `cpe-<idx>/` 目录、展开 reset 模板，然后 fork+exec 自身（去掉 `--sim-*`，追加 `-f/-s/-r`，并通过环境变量传入身份），回收子进程并按退出码决定是否重启。纯逻辑（身份、模板、参数、策略）放在不依赖 USP 头文件的 C 文件中，可以在任何机器上单元测试；核心只做 4 处小改动；tr181 mock 增加身份改写和 sqlite 持久化。

**Tech Stack:** C99、POSIX（fork/execv/waitpid/flock）、sqlite3（插件持久化）、CMake + autotools（`Makefile.am`）、bash（集成测试）。

**Spec:** `docs/superpowers/specs/2026-11-14-cpe-simulator-design.md`（本计划只实现第 6 节"阶段 1"以及第 5 节的对外接口）

## Global Constraints

- 不带任何 `--sim-*` 参数时，obuspa 行为与改造前完全一致：退出码、日志格式、MAC/SN 获取方式都不变。
- 编号规则：`idx = B + p × M + i`，并且要求 `idx ≤ 0xFFFFFFFF`。
- 目录：`<sim-root>/cpe-%08lu/`，内含 `usp.db`、`mock.db`、`reset.txt`、`cli.sock`、`agent.log`；根目录下有 `sim.conf`、`sim.pid`、`sim.map`。
- 默认值：`--sim-procs 1`、`--sim-per-proc 1`、`--sim-base 0`、`--sim-root OBUSPA_LOCAL_STATE_DIR "/sim"`、`--sim-oui VENDOR_OUI`、`--sim-sn-fmt "SIM%08u"`、`--sim-eid-fmt ""`（空表示 `os::<OUI>-<SN>`，按核心规则做百分号编码：字母数字以及 `-._` 不编码，其余编码为大写十六进制）、`--sim-ramp 20`（每秒启动数，0 表示不限速）。
- 子进程环境变量：`USP_BOARD_OUI`、`USP_BOARD_SERIAL`、`USP_ENDPOINT_ID`、`USP_SIM_IDX`（十进制）、`USP_SIM_MAC`（`02:00:00:00:2A:00` 形式）、`USP_SIM_TAG`（=SN）、`TR181_MOCK_DB`（`<dir>/mock.db`）。
- 退出码（只在 `USP_SIM_IDX` 存在时生效）：Stop = 0（保持停止）、Reboot = 10、FactoryReset = 11；其他退出码或被信号终止都算崩溃。
- 模板占位符只有 `${IDX} ${PROC} ${INST} ${OUI} ${SN} ${EID} ${MAC} ${MACX}` 这几个；出现未知占位符时报错，不得原样输出。
- 运行目标平台是 Linux。纯逻辑单元测试（`make -C test/sim check`）只需要 `cc`，任何平台都能跑。

### 相对于 spec 的实现决定（Task 1 中同步更新 spec）

1. **MAC**：`02:<idx 的 32 位大端>:<子字节>`。CPE 主 MAC（`USP_SIM_MAC`、`${MAC}`）的子字节为 `00`；mock 中每个 `*MACAddress` 种子值的子字节取原值的最后一个字节，这样同一台 CPE 的各接口 MAC 互不相同。（spec 原写法是 40 位 idx；32 位已足够，并且能留出接口子字节。）
2. **模拟模式下的 reset 文件与日志改用 `--sim-reset FILE`、`--sim-log stdout|syslog|file`（默认 stdout）**，并禁止同时使用 `-f -s -r -l -c` 及其长选项形式。原因：supervisor 不必完整解析 obuspa 的选项，也避免 1000 个子进程同时打开用户给的同一个日志文件。
3. **`--sim-reidentify` = 对所有实例恢复出厂**（删除 `usp.db` 和 `mock.db`），而不是只删除 DB 中的 3 个身份覆盖值。原因：reset 模板里用 `${SN}` 生成的 MTP 参数（ClientID 等）也必须跟着身份一起变，只删身份字段会留下新旧混杂的状态。
4. **`mock.db` 在 FactoryReset 时由 supervisor 删除**（收到退出码 11 之后），插件不接管 vendor hook 结构。插件只在设置了 `TR181_MOCK_DB` 时才持久化；非模拟模式下不设置该变量就保持原来的纯内存行为。

## Review Focus

1. **同一个 sim-root 上启动第二个 supervisor**：用户会以为可以重复启动。预期第二个立即报错退出（`sim.pid` 加 `flock`），不能和第一个抢着拉起同一批 CPE。→ Task 5 集成测试 `it_second_supervisor_refused`
2. **Ctrl-C / SIGTERM supervisor**：预期 10 秒内所有子 obuspa 退出，不留孤儿进程（否则下次启动会撞 socket 和 DB）。→ Task 5 集成测试 `it_no_orphans_after_sigterm`
3. **身份模板不会变化**（`--sim-sn-fmt "ABC"`、`--sim-eid-fmt "os::x"`）：预期启动时报错，不能让 1000 台用同一个身份上线。→ Task 1 单元测试 `test_sn_fmt_requires_conversion`、`test_eid_fmt_requires_varying_placeholder`
4. **子进程秒退导致崩溃循环**（例如 `-x` 插件路径写错）：预期每个实例按退避间隔重启（1s 起，翻倍，最长 60s），supervisor 不会被 fork 风暴拖垮，汇总里能看到崩溃计数。→ Task 3 单元测试 `test_backoff_*`，Task 5 集成测试 `it_crash_loop_backoff`
5. **`--sim-root` 过长导致 `cli.sock` 超出 `sun_path`（108 字节）**：预期启动时就报错并给出路径，而不是 1000 个子进程各自在 CLI 初始化时失败。→ Task 1 单元测试 `test_socket_path_too_long`

---

## File Structure

| 文件 | 职责 |
|---|---|
| `src/core/sim_identity.{h,c}` | 纯逻辑：`sim_config_t`、身份计算（SN/EID/MAC/目录）、模板展开、`sim.conf` 读写比较、`sim.map` 行。不依赖 USP 头文件 |
| `src/core/sim_args.{h,c}` | 纯逻辑：`SIM_IsRequested`、解析 `--sim-*`、检测冲突选项、生成子进程 argv |
| `src/core/sim_policy.{h,c}` | 纯逻辑：退出码定义与分类、崩溃退避、启动节奏令牌桶 |
| `src/core/sim_supervisor.{h,c}` | `SIM_Main()`：目录准备、加锁、配置校验、fork/exec/reap 循环、信号处理、汇总打印 |
| `src/core/main.c` | 在 `main()` 开头挂接 `SIM_Main` |
| `src/core/dm_exec.c` | 模拟模式下按退出动作使用不同退出码 |
| `src/core/nu_macaddr.c` | `USP_SIM_MAC` 覆盖 WAN MAC |
| `src/core/usp_log.c` | `USP_SIM_TAG` 存在时给每条日志加 `[TAG] ` 前缀 |
| `src/core/CMakeLists.txt`、`Makefile.am` | 加入 4 个新的源文件 |
| `plugins/tr181_vendor/src/tr181_backend_mock.c` | 身份种子改写和 sqlite 持久化 |
| `plugins/tr181_vendor/test/test_plugin.c` | 新增 mock 持久化与改写测试 |
| `test/sim/Makefile`、`test/sim/test_sim_*.c` | 纯逻辑单元测试（`make -C test/sim check`） |
| `test/sim/it_sim.sh`、`test/sim/reset_tmpl.txt` | Linux 集成测试 |
| `docs/sim/README.md` | 使用说明和 1000 台运行手册 |

---

### Task 1: 身份与模板（`sim_identity`）

**Files:**
- Create: `src/core/sim_identity.h`, `src/core/sim_identity.c`
- Create: `test/sim/Makefile`, `test/sim/test_sim_identity.c`, `test/sim/test_util.h`
- Modify: `docs/superpowers/specs/2026-11-14-cpe-simulator-design.md`（写入上面"相对于 spec 的实现决定"的 1–4 条）

**Interfaces:**
- Produces:
  ```c
  #define SIM_SN_MAX 64
  #define SIM_EID_MAX 256
  #define SIM_SUN_PATH_MAX 108
  typedef struct {
      unsigned procs, per_proc;          // >=1
      unsigned long base;
      char root[4096];
      char oui[16];
      char sn_fmt[64];
      char eid_fmt[SIM_EID_MAX];         // "" => os::<OUI>-<SN>
      double ramp;                       // CPE/s, 0 = unlimited
      bool reidentify;
      char reset_tmpl[4096];             // "" => none
      char log_mode[8];                  // "stdout" | "syslog" | "file"
  } sim_config_t;
  typedef struct {
      unsigned long idx; unsigned proc, inst;
      char oui[16]; char sn[SIM_SN_MAX]; char eid[SIM_EID_MAX];
      unsigned char mac[6];
      char dir[4096];
  } sim_identity_t;
  void SIM_ConfigDefaults(sim_config_t *cfg, const char *default_root, const char *default_oui);
  int  SIM_ValidateConfig(const sim_config_t *cfg, char *err, size_t err_len);   // 0 ok, -1 err
  int  SIM_MakeIdentity(const sim_config_t *cfg, unsigned proc, unsigned inst, sim_identity_t *out, char *err, size_t err_len);
  void SIM_FormatMac(const unsigned char mac[6], bool colons, char *buf, size_t len);   // colons: "02:00:00:00:2A:00"; else 12 hex digits "020000002A00"
  void SIM_PercentEncode(const char *src, char *dst, size_t dst_len);   // same rule as core GetDefaultEndpointID
  int  SIM_ExpandTemplate(const char *in, const sim_identity_t *id, char *out, size_t out_len, char *err, size_t err_len);
  int  SIM_ConfWrite(const char *path, const sim_config_t *cfg);
  int  SIM_ConfCompare(const char *path, const sim_config_t *cfg, char *diff, size_t diff_len); // 0 same, 1 differs, -1 missing
  void SIM_MapWriteHeader(FILE *f);   // "idx,proc,inst,sn,eid,mac,dir\n"
  void SIM_MapWriteRow(FILE *f, const sim_identity_t *id);
  ```
  `sim.conf` 只比较身份相关字段：`base oui sn_fmt eid_fmt`。`procs`、`per_proc` 改变时允许复用同一个 root：SN/EID/目录只取决于 idx，所以每个 idx 的身份不变，只是 idx 的总范围变了。

- [ ] **Step 1: 写失败测试** `test/sim/test_sim_identity.c`（用 `test_util.h` 中的 `CHECK(cond, fmt, ...)` 宏，`main` 返回失败数）

  ```c
  test_identity_basic:      cfg base=0,M=100; (p=2,i=5) -> idx 205, sn "SIM00000205",
                            eid "os::012345-SIM00000205", mac 02:00:00:00:CD:00,
                            dir "<root>/cpe-00000205"
  test_identity_base:       base=1000000, (0,0) -> idx 1000000, sn "SIM01000000"
  test_eid_percent_encode:  oui "01 23", sn_fmt "A/B%u", idx 7 -> eid "os::01%2023-A%2FB7"
  test_eid_fmt_custom:      eid_fmt "proto::${OUI}.${SN}" -> "proto::012345.SIM00000003"
  test_sn_fmt_requires_conversion:   "ABC" -> SIM_ValidateConfig == -1, err mentions "sn-fmt"
  test_sn_fmt_rejects_two_conversions: "%u-%u" -> -1;  "%%%05x" -> 0
  test_eid_fmt_requires_varying_placeholder: "os::x-${OUI}" -> -1; "os::${IDX}" -> 0; "${MACX}" -> 0
  test_idx_overflow:        base=0xFFFFFFFF, procs=1, per_proc=2 -> -1
  test_socket_path_too_long: root of 100 'a' -> SIM_ValidateConfig == -1, err contains "cli.sock"
  test_template_all_placeholders: "${IDX} ${PROC} ${INST} ${OUI} ${SN} ${EID} ${MAC} ${MACX}"
                            -> "205 2 5 012345 SIM00000205 os::012345-SIM00000205 02:00:00:00:CD:00 02000000CD00"
  test_template_unknown_placeholder: "x ${SNN} y" -> -1, err contains "${SNN}"
  test_template_unterminated: "x ${SN" -> -1
  test_template_overflow:   out_len 8 with long input -> -1
  test_conf_roundtrip:      write -> compare same == 0; change sn_fmt -> 1 and diff contains "sn_fmt";
                            change procs only -> 0; missing file -> -1
  test_map_row:             row == "205,2,5,SIM00000205,os::012345-SIM00000205,02:00:00:00:CD:00,<root>/cpe-00000205\n"
  ```
  `test/sim/Makefile`：`check` 目标用 `cc -std=c99 -Wall -Wextra -Werror -I../../src/core` 编译每个 `test_sim_*.c` 及其对应的 `../../src/core/sim_*.c`，依次运行，任意一个非零就失败。

- [ ] **Step 2: 运行，确认失败**
  Run: `make -C test/sim check`
  Expected: 编译失败（`sim_identity.h` 不存在）

- [ ] **Step 3: 实现 `sim_identity.c`**
  - SN 格式校验：去掉 `%%` 后恰好有一个转换，形如 `%[flags 0-+ #][width]` 加 `d|i|u|x|X`，不允许长度修饰符；调用时传 `(unsigned)idx`。生成的 SN 不能为空，不能含空白，长度 `< SIM_SN_MAX`。
  - EID 模板非空时必须包含 `${SN} ${IDX} ${MAC} ${MACX}` 中至少一个；展开时 `${EID}` 不可用（自引用要报错）。
  - socket 路径检查：`strlen(root) + strlen("/cpe-00000000/cli.sock") < SIM_SUN_PATH_MAX`。
  - 只用 libc，不包含任何 USP 头文件。

- [ ] **Step 4: 运行，确认通过**
  Run: `make -C test/sim check`
  Expected: 所有测试 PASS，退出码 0

- [ ] **Step 5: 更新 spec 第 5.2、5.4、5.5、5.6、5.1 节中对应的 4 条实现决定，然后提交**
  ```bash
  git add src/core/sim_identity.[ch] test/sim docs/superpowers/specs/2026-11-14-cpe-simulator-design.md
  git commit -m "feat(sim): identity, template and sim.conf logic"
  ```

---

### Task 2: 参数解析（`sim_args`）

**Files:**
- Create: `src/core/sim_args.h`, `src/core/sim_args.c`, `test/sim/test_sim_args.c`

**Interfaces:**
- Consumes: `sim_config_t`、`SIM_ConfigDefaults`、`SIM_ValidateConfig`（Task 1）
- Produces:
  ```c
  bool SIM_IsRequested(int argc, char **argv);   // 任一参数以 "--sim-" 或 "-sim-" 开头
  // 解析 --sim-X V 与 --sim-X=V（单横线形式也接受，与 getopt_long_only 一致）。
  // 剩余参数原样按顺序复制到 fwd（不含 argv[0]），*fwd_argc 为数量；fwd 由调用者 free（仅数组本身）。
  int  SIM_ParseArgs(int argc, char **argv, sim_config_t *cfg, char ***fwd, int *fwd_argc, char *err, size_t err_len);
  // 子进程 argv: [self, fwd..., "-f", dir/usp.db, "-s", dir/cli.sock, ("-r", dir/reset.txt)?, ("-l", dir/agent.log | "syslog")?, NULL]
  char **SIM_BuildChildArgv(const char *self, char **fwd, int fwd_argc, const sim_config_t *cfg, const sim_identity_t *id);
  ```
  选项：`--sim-procs --sim-per-proc --sim-base --sim-root --sim-oui --sim-sn-fmt --sim-eid-fmt --sim-ramp --sim-reset --sim-log`（带值），`--sim-reidentify`（不带值）。

- [ ] **Step 1: 写失败测试** `test/sim/test_sim_args.c`
  ```c
  test_is_requested:        {"obuspa","-p","--sim-procs","2"} -> true; {"obuspa","-p"} -> false
  test_parse_both_forms:    "--sim-procs 10 --sim-per-proc=100 -sim-base 5" -> procs 10, per_proc 100, base 5
  test_parse_forwards_rest: "-v 4 --sim-procs 2 -x a.so -p" -> fwd == {"-v","4","-x","a.so","-p"}
  test_parse_reidentify_flag: "--sim-reidentify" -> cfg.reidentify == true
  test_parse_bad_number:    "--sim-procs 0" / "--sim-procs abc" / "--sim-ramp -1" -> -1
  test_parse_unknown_sim:   "--sim-proc 3" -> -1, err contains "--sim-proc"
  test_parse_missing_value: "--sim-procs" (last) -> -1
  test_conflicting_options: each of "-f x","--dbfile x","-dbfile=x","-s x","--cli x","-r x","--resetfile x",
                            "-l x","--log x","-c","--command" combined with "--sim-procs 1" -> -1,
                            err suggests "--sim-reset"/"--sim-log" where applicable
  test_parse_log_mode:      "--sim-log file" ok; "--sim-log foo" -> -1
  test_child_argv:          fwd {"-x","a.so"}, reset_tmpl set, log_mode "file" ->
                            {"self","-x","a.so","-f",D"/usp.db","-s",D"/cli.sock","-r",D"/reset.txt","-l",D"/agent.log",NULL}
                            log_mode "stdout" -> no "-l"; log_mode "syslog" -> "-l","syslog"; no reset_tmpl -> no "-r"
  ```
- [ ] **Step 2: 运行确认失败** — `make -C test/sim check`，Expected: 编译失败
- [ ] **Step 3: 实现 `sim_args.c`**（不使用 getopt，避免和 main 的 getopt 状态互相干扰；冲突检测按完整 token 或 `token=` 前缀匹配，同时检查单横线和双横线形式）
- [ ] **Step 4: 运行确认通过** — `make -C test/sim check`，Expected: PASS
- [ ] **Step 5: 提交** — `git add src/core/sim_args.[ch] test/sim/test_sim_args.c test/sim/Makefile && git commit -m "feat(sim): --sim-* argument parsing"`

---

### Task 3: 重启策略（`sim_policy`）

**Files:**
- Create: `src/core/sim_policy.h`, `src/core/sim_policy.c`, `test/sim/test_sim_policy.c`

**Interfaces:**
- Produces:
  ```c
  #define SIM_EXIT_STOP           0
  #define SIM_EXIT_REBOOT         10
  #define SIM_EXIT_FACTORY_RESET  11
  typedef enum { SIM_ACT_STAY_DOWN, SIM_ACT_RESTART, SIM_ACT_FACTORY_RESET, SIM_ACT_CRASH } sim_action_t;
  sim_action_t SIM_ClassifyExit(int wait_status);   // 输入 waitpid 的 status
  // 崩溃退避：第 n 次连续崩溃（n>=1）等待 min(1<<(n-1), 60) 秒；实例连续运行 >= 60 秒后连续崩溃计数清零
  unsigned SIM_BackoffSeconds(unsigned consecutive_crashes);
  bool     SIM_ShouldResetCrashCount(double uptime_seconds);
  typedef struct { double rate, tokens, burst; double last; } sim_ramp_t;
  void SIM_RampInit(sim_ramp_t *r, double rate, double now);   // burst = max(1, rate); rate 0 => unlimited
  bool SIM_RampTake(sim_ramp_t *r, double now);                 // 有令牌就取走一个并返回 true
  ```
- [ ] **Step 1: 写失败测试** `test/sim/test_sim_policy.c`（status 用 `W_EXITCODE(code,0)`、`W_EXITCODE(0,SIGKILL)` 构造；若平台没有该宏，在 `test_util.h` 中补充定义 `((code)<<8 | (sig))`）
  ```c
  test_classify:   exit 0 -> STAY_DOWN; 10 -> RESTART; 11 -> FACTORY_RESET; 1 -> CRASH; 255 -> CRASH; SIGKILL -> CRASH; SIGSEGV -> CRASH
  test_backoff_sequence: n=1..8 -> 1,2,4,8,16,32,60,60
  test_backoff_reset:    SIM_ShouldResetCrashCount(59.9) false, (60.0) true
  test_ramp_limits:      rate 20, now 0 -> 20 takes true then false; at now 0.5 -> 10 more true
  test_ramp_unlimited:   rate 0 -> 10000 takes all true
  ```
- [ ] **Step 2: 运行确认失败** — `make -C test/sim check`
- [ ] **Step 3: 实现 `sim_policy.c`**
- [ ] **Step 4: 运行确认通过** — `make -C test/sim check`，Expected: PASS
- [ ] **Step 5: 提交** — `git commit -m "feat(sim): exit classification, crash backoff, ramp limiter"`

---

### Task 4: 核心挂接点（退出码 / MAC / 日志前缀 / 构建）

**Files:**
- Modify: `src/core/dm_exec.c:1702-1754`（`DM_EXEC_HandleScheduledExit`）
- Modify: `src/core/nu_macaddr.c:109`（`nu_macaddr_wan_macaddr`）
- Modify: `src/core/usp_log.c:557`（`LogMessageToFile`）
- Modify: `src/core/CMakeLists.txt`、`Makefile.am`（加入 `sim_identity.c sim_args.c sim_policy.c sim_supervisor.c`）
- Create: `src/core/sim_supervisor.h`、`src/core/sim_supervisor.c`（这一步只放占位实现 `int SIM_Main(int argc, char **argv) { fprintf(stderr,"sim: not implemented\n"); return 1; }`，Task 5 再实现）
- Modify: `src/core/main.c`（`main()` 在 `USP_LOG_Init()` 之前插入 `if (SIM_IsRequested(argc, argv)) return SIM_Main(argc, argv);`）

**Interfaces:**
- Consumes: `SIM_EXIT_*`（Task 3）、`SIM_IsRequested`（Task 2）
- Produces: `int SIM_Main(int argc, char **argv);`（Task 5 实现）；子进程中以下环境变量生效：`USP_SIM_IDX` 决定退出码、`USP_SIM_MAC` 决定 WAN MAC、`USP_SIM_TAG` 决定日志前缀。

- [ ] **Step 1: `dm_exec.c`** — 末尾的 `exit(0)` 改为 `exit(SimExitCode(exit_action))`。`static int SimExitCode(exit_action_t a)`：没有 `USP_SIM_IDX` 时返回 0；有则 `kExitAction_Reboot→SIM_EXIT_REBOOT`、`kExitAction_FactoryReset→SIM_EXIT_FACTORY_RESET`，其余返回 `SIM_EXIT_STOP`。
- [ ] **Step 2: `nu_macaddr.c`** — 函数开头：`USP_SIM_MAC` 非空并能按 `XX:XX:XX:XX:XX:XX` 解析时，写入 buf 并返回 `USP_ERR_OK`；解析失败则 `USP_LOG_Warning` 后继续走原逻辑。
- [ ] **Step 3: `usp_log.c`** — 只在第一次调用时读取 `USP_SIM_TAG` 并缓存（`static`，用 `pthread_once` 保证线程安全）。非空时 syslog 和 fd 两条输出路径都改为 `"[%s] %s"`；为空时输出与原来逐字节一致。
- [ ] **Step 4: 构建验证（Linux）**
  Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && ./build/obuspa --sim-procs 1; echo rc=$?`
  Expected: 构建无警告新增；输出 `sim: not implemented`，`rc=1`
  Run: `autoreconf -fi && ./configure && make -j`（如果该 Linux 机器本来就用 autotools 构建）
  Expected: 构建成功
- [ ] **Step 5: 非模拟模式回归（Linux）**
  Run: `./build/obuspa -f /tmp/r.db -s /tmp/r.sock -v 3 & sleep 3; ./build/obuspa -c -s /tmp/r.sock -f /tmp/r.db stop; wait $!; echo rc=$?`
  Expected: `rc=0`，日志行没有 `[` 前缀
- [ ] **Step 6: 提交** — `git commit -m "feat(sim): core hooks for exit codes, MAC override, log tag"`

---

### Task 5: Supervisor（`SIM_Main`）+ 集成测试

**Files:**
- Modify: `src/core/sim_supervisor.c`（完整实现）
- Create: `test/sim/it_sim.sh`、`test/sim/reset_tmpl.txt`

**Interfaces:**
- Consumes: Task 1–3 的全部接口
- Produces: `int SIM_Main(int argc, char **argv)`：正常关闭返回 0，配置错误返回 2，运行时错误返回 1。

`reset_tmpl.txt`：
```
Device.LocalAgent.Controller.1.Alias "ctrl-${IDX}"
Device.LocalAgent.Controller.1.Enable "false"
Device.LocalAgent.Controller.1.EndpointID "self::ctrl-${SN}"
```

`SIM_Main` 流程（按顺序）：
1. `SIM_ConfigDefaults(cfg, OBUSPA_LOCAL_STATE_DIR "/sim", VENDOR_OUI)` → `SIM_ParseArgs` → `SIM_ValidateConfig`，出错时打印到 stderr 并返回 2。
2. `mkdir -p root`；打开 `root/sim.pid`，加 `flock(LOCK_EX|LOCK_NB)`，失败时报 "another supervisor is running on <root> (pid N)" 并返回 2；写入自己的 pid。锁 fd 设置 `FD_CLOEXEC`。
3. `SIM_ConfCompare(root/sim.conf)`：结果为 1 且没有 `--sim-reidentify` 时打印差异并返回 2；为 1 且带 `--sim-reidentify` 时，删除所有 `cpe-*/usp.db` 和 `cpe-*/mock.db`；为 -1 或 1 时都写入新的 `sim.conf`。
4. 读入 reset 模板，对每个实例：`SIM_MakeIdentity` → `mkdir dir` → 展开模板后写入 `dir/reset.txt`（任一实例展开失败就整体返回 2，并指出出错的行号）；写入 `sim.map`（先写 tmp 文件再 rename）。
5. 自身路径：`readlink("/proc/self/exe")`，失败时退回 `realpath(argv[0])`。
6. 主循环（每 50ms 一轮）：`waitpid(-1, &st, WNOHANG)` 回收全部已退出子进程，按 `SIM_ClassifyExit` 处理：
   - `STAY_DOWN`：标记 stopped；
   - `RESTART`：立即排队；
   - `FACTORY_RESET`：删除 `dir/mock.db` 后排队；
   - `CRASH`：连续崩溃计数 +1（若 `SIM_ShouldResetCrashCount(uptime)` 则先清零），在 `SIM_BackoffSeconds` 之后排队。
   排队中的实例按 idx 顺序，经 `SIM_RampTake` 限速后 fork：子进程调用 `setenv` 设置 Global Constraints 中的 7 个变量，然后 `execv(self, SIM_BuildChildArgv(...))`；exec 失败时 `_exit(127)`。
7. 每 10 秒向 stdout 打印一行：`sim: running=R/T stopped=S pending=P restarts=X crashes=C`。
8. 收到 SIGINT/SIGTERM（信号处理函数只设置 `volatile sig_atomic_t` 标志）后：不再拉起新实例，向所有存活子进程发 SIGTERM，最多等 10 秒，之后对剩余进程发 SIGKILL，回收完毕后删除 `sim.pid` 并返回 0。
9. 启动时检查 `RLIMIT_NPROC`、`RLIMIT_NOFILE`；若低于 `T×8` 和 `T+64`，打印警告（不退出）。

- [ ] **Step 1: 写集成测试 `test/sim/it_sim.sh <obuspa> <plugin.so>`**（bash，`set -euo pipefail`，每个用例是一个函数，在临时 root 中运行，失败时打印 supervisor 日志；`cli()` 封装 `$OBUSPA -c -s $ROOT/cpe-$(printf %08d $1)/cli.sock -f $ROOT/cpe-.../usp.db "${@:2}"`；`wait_for` 轮询条件，超时 30 秒）
  ```
  it_identities:     --sim-procs 2 --sim-per-proc 3 --sim-base 100 --sim-ramp 0 --sim-reset reset_tmpl.txt -x $PLUGIN
                     -> 6 个 cpe-0000010[0-5] 目录；sim.map 有 6 行数据；
                     每个 idx: cli get Device.DeviceInfo.SerialNumber == SIM00000<idx>,
                               cli get Device.LocalAgent.EndpointID == os::012345-SIM00000<idx>,
                               cli get Device.LocalAgent.Controller.1.Alias == ctrl-<idx>,
                               cli get Device.Ethernet.Interface.1.MACAddress == 02:00:00:00:<idx hex>:53
  it_persist_reboot: cli set Device.LocalAgent.Controller.1.Alias foo; cli set Device.WiFi.SSID.1.SSID bar;
                     cli operate "Device.Reboot()" -> 同一 idx 的新 pid 出现；两个值仍为 foo / bar
  it_factory_reset:  cli operate "Device.FactoryReset()" -> 重启后 Alias == ctrl-<idx>，SSID == CSV 种子值
  it_stop_stays_down: cli stop -> 该 idx 进程退出且 15s 内不再出现；summary 中 stopped=1
  it_crash_restart:  kill -9 <某子进程> -> 约 1s 后同一 idx 重新出现
  it_restart_keeps_identity: SIGTERM supervisor，用相同参数重启 -> SN/EID 不变，Alias 保持 foo
  it_conf_mismatch:  相同 root，改用 --sim-sn-fmt "X%06u" -> 返回码 2，stderr 含 "sn_fmt"；
                     加 --sim-reidentify -> 启动成功，SN == X000100，Alias == ctrl-100
  it_second_supervisor_refused: 运行中再启动一个同 root 的 supervisor -> 返回码 2，含 "another supervisor"
  it_no_orphans_after_sigterm:  SIGTERM supervisor -> 10s 内 `pgrep -f "$ROOT/cpe-"` 为空，sim.pid 已删除
  it_crash_loop_backoff: -x /nonexistent.so --sim-procs 1 --sim-per-proc 2 运行 8s ->
                     每个 idx 的启动次数 <= 5（日志中 "sim: launch idx=" 计数），supervisor 仍能响应 SIGTERM
  it_no_sim_unchanged: 普通 obuspa 启动 + CLI stop -> rc 0，日志无 "[" 前缀
  ```
  supervisor 每次 fork 打印一行 `sim: launch idx=<idx> pid=<pid>`，供测试计数使用。
- [ ] **Step 2: 运行，确认失败（Linux）**
  Run: `test/sim/it_sim.sh build/obuspa plugins/tr181_vendor/libtr181_vendor.so`
  Expected: `it_identities` 失败（占位实现返回 "not implemented"）
- [ ] **Step 3: 按上面的流程实现 `sim_supervisor.c`**
- [ ] **Step 4: 运行集成测试（Linux）**
  Run: 同 Step 2
  Expected: 除 `it_persist_reboot` 中的 SSID 断言、`it_factory_reset` 中的 SSID 断言、`it_identities` 中的 MACAddress 断言（依赖 Task 6）以外，其余全部 PASS。脚本通过 `SKIP_PLUGIN_CHECKS=1` 跳过这三处断言。
- [ ] **Step 5: 提交** — `git commit -m "feat(sim): multi-process supervisor and integration tests"`

---

### Task 6: tr181 mock 身份改写与持久化

**Files:**
- Modify: `plugins/tr181_vendor/src/tr181_backend_mock.c`
- Modify: `plugins/tr181_vendor/test/test_plugin.c`
- Modify: `plugins/tr181_vendor/Makefile`（.so 与测试程序都链接 `-lsqlite3`）**——该文件目前不在仓库中，见执行前置条件**
- Modify: `plugins/tr181_vendor/README.md`（"持久化"一节：`TR181_MOCK_DB`；FactoryReset 时由使用者或 supervisor 删除该文件）

**Interfaces:**
- Consumes: 环境变量 `USP_BOARD_OUI`、`USP_BOARD_SERIAL`、`USP_SIM_MAC`、`TR181_MOCK_DB`
- Produces: 对外接口 `tr181_backend.h` 不变

行为：
- `TR181_BE_Init()`：加载种子 → 改写身份 → 若设置了 `TR181_MOCK_DB` 则打开 sqlite（`CREATE TABLE IF NOT EXISTS kv(path TEXT PRIMARY KEY, value TEXT)`、`inst(path TEXT PRIMARY KEY, present INTEGER)`），先按 `inst` 表重放增删（`present=0` 时删除该实例及其所有子路径的值），再用 `kv` 表覆盖值。
- 身份改写：`Device.DeviceInfo.SerialNumber`、`DeviceID.SerialNumber` ← `USP_BOARD_SERIAL`；`Device.DeviceInfo.ManufacturerOUI` ← `USP_BOARD_OUI`；所有以 `MACAddress` 结尾、值能解析为 MAC 的种子 ← `USP_SIM_MAC` 的前 5 个字节加原值的最后一个字节。对应环境变量不存在时不改写。
- `Set` → `INSERT OR REPLACE kv`；`AddInstance` → `inst present=1`；`DeleteInstance` → `inst present=0`，并 `DELETE FROM kv WHERE path = ? OR path LIKE ? || '.%'`（用前缀匹配，注意 `_`、`%` 需要转义）。每次写操作独立提交；数据库打开时设置 `PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL`。
- sqlite 出错时：`USP_LOG_Error` 并返回 `TR181_BE_ERROR`（Set 失败会让 Controller 收到错误，不得静默丢弃写入）。
- `TR181_BE_Stop()` 关闭数据库。

- [ ] **Step 1: 在 `test_plugin.c` 中写失败测试**（每个测试前用 `setenv/unsetenv` 设置环境，并使用 `mkstemp` 生成的临时 db 路径）
  ```c
  test_mock_identity_rewrite: USP_BOARD_SERIAL=SIM00000007, USP_BOARD_OUI=ABCDEF, USP_SIM_MAC=02:00:00:00:07:00
      -> Get DeviceInfo.SerialNumber == "SIM00000007"; ManufacturerOUI == "ABCDEF";
         Ethernet.Interface.1.MACAddress == "02:00:00:00:07:53"; WiFi.SSID.1.MACAddress == "02:00:00:00:07:50"
  test_mock_no_env_unchanged: 不设任何环境变量 -> SerialNumber == "C100023710001066"
  test_mock_persist_set:      TR181_MOCK_DB=tmp; Init; Set WiFi.SSID.1.SSID "abc"; Stop; Init -> Get == "abc"
  test_mock_persist_add_delete: AddInstance Device.Hosts.AccessControl.<n+1>; Set 其中一个参数; Stop; Init ->
                              实例存在且值保留；DeleteInstance; Stop; Init -> 实例不存在，Get 返回 NOT_FOUND
  test_mock_persist_delete_seed: 删除一个种子实例 -> 重启后仍然不存在
  test_mock_no_db_memory_only: 未设置 TR181_MOCK_DB -> Set 后 Stop/Init 回到种子值，且不产生任何文件
  ```
- [ ] **Step 2: 运行确认失败（Linux）** — `make -C plugins/tr181_vendor check`，Expected: 新测试 FAIL
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行（Linux）**
  Run: `make -C plugins/tr181_vendor check && test/sim/it_sim.sh build/obuspa plugins/tr181_vendor/libtr181_vendor.so`
  Expected: 插件测试全部 PASS；集成测试（不设 `SKIP_PLUGIN_CHECKS`）全部 PASS
- [ ] **Step 5: 提交** — `git commit -m "feat(tr181_vendor): per-CPE identity seeds and sqlite persistence"`

---

### Task 7: 文档与规模验证

**Files:**
- Create: `docs/sim/README.md`
- Modify: `test/sim/it_sim.sh`（新增可选用例 `it_mqtt_connect`）

- [ ] **Step 1: 编写 `docs/sim/README.md`**：命令行参数表（与 spec 5.1 一致，加上 `--sim-reset`、`--sim-log`）；目录布局；占位符说明；MQTT 示例模板（ClientID `${SN}`、ResponseTopicConfigured `usp/agent/${EID}`）；多机部署时如何划分 `--sim-base`；`--sim-reidentify` 的语义（会对所有实例恢复出厂）；系统调优（`ulimit -u/-n`、`kernel.pid_max`、`fs.inotify`）；1000 台运行手册（含如何测内存：`smem -c "name pss" -P obuspa` 或对 `/proc/*/smaps_rollup` 的 Pss 求和）。
- [ ] **Step 2: 新增 `it_mqtt_connect`**：仅在设置了 `MQTT_BROKER=host:port` 时运行，使用 MQTT 模板，2×5 个 CPE，60 秒内每个 idx 的 `Device.MQTT.Client.1.Status` 都应为 `Connected`。
- [ ] **Step 3: 规模冒烟（Linux，手工执行，记录结果）**
  Run: `obuspa --sim-procs 10 --sim-per-proc 100 --sim-ramp 50 --sim-reset mqtt_tmpl.txt -x libtr181_vendor.so`
  Expected: 约 20 秒后 summary 显示 `running=1000/1000`；把总 PSS 和 broker 侧的连接数记入 `docs/sim/README.md` 的"基线"小节（作为阶段 2 的对照）。
- [ ] **Step 4: 提交** — `git commit -m "docs(sim): usage, tuning and 1000-CPE baseline"`

---

## 执行前置条件

1. **一台 Linux 构建机**：具备 obuspa 依赖（sqlite3、OpenSSL、curl、zlib、libmosquitto、libwebsockets ≥ 4.1）以及 cmake 或 autotools。Task 4–7 的验证都要在这台机器上执行；Task 1–3 在任何机器上都能做。
2. **`plugins/tr181_vendor/Makefile`**：README 中引用了它，但它不在仓库里。Task 6 之前需要把它提交进来。
