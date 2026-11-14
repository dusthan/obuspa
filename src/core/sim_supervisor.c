/*
 * CPE simulator: supervisor process entry point.
 *
 * Runs before USP_LOG_Init()/USP_MEM_Init() (see main.c), so it uses plain stdio and malloc only.
 * Flow: parse/validate options -> lock <root>/sim.pid -> check sim.conf -> prepare instance
 * directories (reset.txt, sim.map) -> fork/exec/reap loop -> orderly shutdown on SIGINT/SIGTERM.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "sim_supervisor.h"
#include "sim_args.h"
#include "sim_identity.h"
#include "sim_policy.h"
#include "vendor_defs.h"   // VENDOR_OUI

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SIM_TICK_NS          (50 * 1000 * 1000)   // main loop period
#define SIM_SUMMARY_PERIOD   10.0                 // seconds between summary lines
#define SIM_SHUTDOWN_GRACE   10.0                 // seconds between SIGTERM and SIGKILL
#define SIM_LAUNCH_RETRY     1.0                  // seconds before retrying a failed fork

typedef enum { INST_PENDING, INST_RUNNING, INST_STOPPED } inst_state_t;

typedef struct
{
    sim_identity_t id;
    inst_state_t state;
    pid_t pid;
    double due;              // earliest launch time (when pending)
    double started;          // launch time (when running)
    unsigned crashes;        // consecutive crashes
} sim_inst_t;

typedef struct
{
    sim_config_t cfg;
    char **fwd;
    int fwd_argc;
    char self[PATH_MAX];
    char pid_path[PATH_MAX];
    int lock_fd;
    sim_inst_t *inst;
    size_t count;
    sim_ramp_t ramp;
    unsigned long restarts;
    unsigned long crash_total;
} sim_ctx_t;

static volatile sig_atomic_t stop_requested = 0;

static void OnStopSignal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static double Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void Out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void Err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Err(const char *fmt, ...)
{
    va_list ap;
    fputs("sim: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static int JoinPath(char *buf, size_t len, const char *dir, const char *leaf)
{
    int n = snprintf(buf, len, "%s/%s", dir, leaf);
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

// Deletes a file; a missing file is not an error. Returns 0 ok, -1 error (reported).
static int RemoveFile(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    Err("cannot delete %s: %s", path, strerror(errno));
    return -1;
}

/*********************************************************************//**
** Step 1: options
**************************************************************************/
static int LoadConfig(sim_ctx_t *ctx, int argc, char **argv)
{
    char err[512];

    SIM_ConfigDefaults(&ctx->cfg, OBUSPA_LOCAL_STATE_DIR "/sim", VENDOR_OUI);
    if (SIM_ParseArgs(argc, argv, &ctx->cfg, &ctx->fwd, &ctx->fwd_argc, err, sizeof(err)) != 0 ||
        SIM_ValidateConfig(&ctx->cfg, err, sizeof(err)) != 0)
    {
        Err("%s", err);
        return 2;
    }
    return 0;
}

/*********************************************************************//**
** Step 2: root directory and single-supervisor lock
**************************************************************************/
static int MkdirP(const char *path)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p != '\0'; p++)
    {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int AcquireLock(sim_ctx_t *ctx)
{
    char buf[32];
    ssize_t n;
    const char *root = ctx->cfg.root;

    if (MkdirP(root) != 0)
    {
        Err("cannot create %s: %s", root, strerror(errno));
        return 1;
    }
    if (JoinPath(ctx->pid_path, sizeof(ctx->pid_path), root, "sim.pid") != 0)
    {
        Err("sim-root path is too long");
        return 2;
    }
    ctx->lock_fd = open(ctx->pid_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (ctx->lock_fd < 0)
    {
        Err("cannot open %s: %s", ctx->pid_path, strerror(errno));
        return 1;
    }
    if (flock(ctx->lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        n = pread(ctx->lock_fd, buf, sizeof(buf) - 1, 0);
        buf[n > 0 ? n : 0] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        Err("another supervisor is running on %s (pid %s)", root, buf[0] != '\0' ? buf : "unknown");
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
        return 2;
    }
    n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    if (ftruncate(ctx->lock_fd, 0) != 0 || pwrite(ctx->lock_fd, buf, (size_t)n, 0) != n)
    {
        Err("cannot write %s: %s", ctx->pid_path, strerror(errno));
        return 1;
    }
    return 0;
}

static void ReleaseLock(sim_ctx_t *ctx)
{
    if (ctx->lock_fd < 0) return;
    unlink(ctx->pid_path);      // while still holding the lock
    close(ctx->lock_fd);
    ctx->lock_fd = -1;
}

/*********************************************************************//**
** Step 3: sim.conf (identity options must not change silently)
**************************************************************************/
// Deletes usp.db and mock.db of every cpe-* directory under root
static int FactoryResetAll(const char *root)
{
    DIR *d = opendir(root);
    struct dirent *e;
    char path[PATH_MAX];
    int rc = 0;

    if (d == NULL)
    {
        Err("cannot open %s: %s", root, strerror(errno));
        return -1;
    }
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, "cpe-", 4) != 0) continue;
        static const char *const leaves[] = { "usp.db", "mock.db" };
        for (size_t i = 0; i < 2; i++)
        {
            if (snprintf(path, sizeof(path), "%s/%s/%s", root, e->d_name, leaves[i]) >= (int)sizeof(path)) continue;
            if (RemoveFile(path) != 0) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

static int CheckSimConf(const sim_ctx_t *ctx)
{
    char path[PATH_MAX];
    char diff[2048];
    int cmp;

    if (JoinPath(path, sizeof(path), ctx->cfg.root, "sim.conf") != 0)
    {
        Err("sim-root path is too long");
        return 2;
    }
    cmp = SIM_ConfCompare(path, &ctx->cfg, diff, sizeof(diff));
    if (cmp == 0)
    {
        if (ctx->cfg.reidentify) Out("sim: identity options unchanged, --sim-reidentify has nothing to do\n");
        return 0;
    }
    if (cmp == 1 && !ctx->cfg.reidentify)
    {
        Err("identity options differ from %s: %s", path, diff);
        Err("instances keep their stored identities; use --sim-reidentify to factory-reset all instances with the new identities, or another --sim-root");
        return 2;
    }
    if (cmp == 1)
    {
        Out("sim: re-identifying (%s), deleting usp.db and mock.db of all instances\n", diff);
        if (FactoryResetAll(ctx->cfg.root) != 0) return 1;
    }
    if (SIM_ConfWrite(path, &ctx->cfg) != 0)
    {
        Err("cannot write %s: %s", path, strerror(errno));
        return 1;
    }
    return 0;
}

/*********************************************************************//**
** Step 4: per-instance directories, reset.txt and sim.map
**************************************************************************/
// Reads a whole text file into a malloc'd NUL-terminated buffer
static char *ReadFile(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    size_t len = 0, cap = 0, n;

    if (f == NULL) return NULL;
    do
    {
        if (cap - len < 4096)
        {
            char *nb = realloc(buf, cap + 65536);
            if (nb == NULL) { free(buf); fclose(f); errno = ENOMEM; return NULL; }
            buf = nb;
            cap += 65536;
        }
        n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
    } while (n > 0);
    if (ferror(f)) { free(buf); fclose(f); errno = EIO; return NULL; }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

// Expands the reset template line by line into <dir>/reset.txt. Returns 0, 1 (I/O) or 2 (template error).
static int WriteResetFile(const char *tmpl_path, const char *tmpl, const sim_identity_t *id)
{
    char path[PATH_MAX], err[256];
    char line[8192], out[16384];
    const char *p = tmpl;
    unsigned lineno = 0;
    int rc = 0;
    FILE *f;

    if (JoinPath(path, sizeof(path), id->dir, "reset.txt") != 0) { Err("%s/reset.txt: path too long", id->dir); return 2; }
    f = fopen(path, "w");
    if (f == NULL) { Err("cannot write %s: %s", path, strerror(errno)); return 1; }

    while (*p != '\0' && rc == 0)
    {
        size_t len = strcspn(p, "\n");
        lineno++;
        if (len >= sizeof(line))
        {
            Err("%s:%u: line too long (max %zu chars)", tmpl_path, lineno, sizeof(line) - 1);
            rc = 2;
            break;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p += len + (p[len] == '\n' ? 1 : 0);
        if (SIM_ExpandTemplate(line, id, out, sizeof(out), err, sizeof(err)) != 0)
        {
            Err("%s:%u (idx %lu): %s", tmpl_path, lineno, id->idx, err);
            rc = 2;
            break;
        }
        if (fprintf(f, "%s\n", out) < 0) rc = 1;
    }
    if (fclose(f) != 0 && rc == 0) rc = 1;
    if (rc == 1) Err("cannot write %s: %s", path, strerror(errno));
    return rc;
}

static int PrepareInstances(sim_ctx_t *ctx)
{
    char err[512], map_path[PATH_MAX], tmp_path[PATH_MAX];
    char *tmpl = NULL;
    FILE *map;
    int rc = 0;

    ctx->count = (size_t)ctx->cfg.procs * ctx->cfg.per_proc;
    ctx->inst = calloc(ctx->count, sizeof(sim_inst_t));
    if (ctx->inst == NULL) { Err("out of memory for %zu instances", ctx->count); return 1; }

    if (ctx->cfg.reset_tmpl[0] != '\0' && (tmpl = ReadFile(ctx->cfg.reset_tmpl)) == NULL)
    {
        Err("cannot read reset template %s: %s", ctx->cfg.reset_tmpl, strerror(errno));
        return 2;
    }
    if (JoinPath(map_path, sizeof(map_path), ctx->cfg.root, "sim.map") != 0 ||
        JoinPath(tmp_path, sizeof(tmp_path), ctx->cfg.root, "sim.map.tmp") != 0)
    {
        free(tmpl);
        Err("sim-root path is too long");
        return 2;
    }
    map = fopen(tmp_path, "w");
    if (map == NULL)
    {
        free(tmpl);
        Err("cannot write %s: %s", tmp_path, strerror(errno));
        return 1;
    }
    SIM_MapWriteHeader(map);

    for (size_t i = 0; i < ctx->count && rc == 0; i++)
    {
        sim_inst_t *in = &ctx->inst[i];
        unsigned proc = (unsigned)(i / ctx->cfg.per_proc), inst = (unsigned)(i % ctx->cfg.per_proc);
        if (SIM_MakeIdentity(&ctx->cfg, proc, inst, &in->id, err, sizeof(err)) != 0)
        {
            Err("instance proc=%u inst=%u: %s", proc, inst, err);
            rc = 2;
            break;
        }
        if (mkdir(in->id.dir, 0755) != 0 && errno != EEXIST)
        {
            Err("cannot create %s: %s", in->id.dir, strerror(errno));
            rc = 1;
            break;
        }
        if (tmpl != NULL) rc = WriteResetFile(ctx->cfg.reset_tmpl, tmpl, &in->id);
        SIM_MapWriteRow(map, &in->id);
        in->state = INST_PENDING;
        in->pid = -1;
    }
    free(tmpl);

    if (fclose(map) != 0 && rc == 0)
    {
        Err("cannot write %s: %s", tmp_path, strerror(errno));
        rc = 1;
    }
    if (rc != 0)
    {
        unlink(tmp_path);
        return rc;
    }
    if (rename(tmp_path, map_path) != 0)
    {
        Err("cannot rename %s to %s: %s", tmp_path, map_path, strerror(errno));
        return 1;
    }
    return 0;
}

/*********************************************************************//**
** Step 5 and 9: own executable path, resource limits
**************************************************************************/
static int ResolveSelf(sim_ctx_t *ctx, const char *argv0)
{
    ssize_t n = readlink("/proc/self/exe", ctx->self, sizeof(ctx->self) - 1);
    if (n > 0)
    {
        ctx->self[n] = '\0';
        return 0;
    }
    if (realpath(argv0, ctx->self) != NULL) return 0;
    Err("cannot determine own executable path (argv[0]=%s): %s", argv0, strerror(errno));
    return 1;
}

static void WarnLimit(int resource, const char *name, unsigned long long want)
{
    struct rlimit rl;
    if (getrlimit(resource, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) return;
    if ((unsigned long long)rl.rlim_cur < want)
        Err("warning: %s soft limit is %llu, below the recommended %llu for this run", name,
            (unsigned long long)rl.rlim_cur, want);
}

static void CheckLimits(const sim_ctx_t *ctx)
{
    unsigned long long t = ctx->count;
    WarnLimit(RLIMIT_NPROC, "RLIMIT_NPROC", t * 8);
    WarnLimit(RLIMIT_NOFILE, "RLIMIT_NOFILE", t + 64);
}

/*********************************************************************//**
** Signals
**************************************************************************/
static int InstallSignals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnStopSignal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
    {
        Err("cannot install signal handlers: %s", strerror(errno));
        return 1;
    }
    return 0;
}

// In the forked child: undo everything the supervisor changed about signals
static void ResetChildSignals(void)
{
    sigset_t all;
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
}

/*********************************************************************//**
** Step 6: launching
**************************************************************************/
static int SetChildEnv(const sim_identity_t *id)
{
    char idx[32], mac[32], mock[PATH_MAX];
    snprintf(idx, sizeof(idx), "%lu", id->idx);
    SIM_FormatMac(id->mac, true, mac, sizeof(mac));
    if (JoinPath(mock, sizeof(mock), id->dir, "mock.db") != 0) return -1;
    if (setenv("USP_BOARD_OUI", id->oui, 1) != 0 ||
        setenv("USP_BOARD_SERIAL", id->sn, 1) != 0 ||
        setenv("USP_ENDPOINT_ID", id->eid, 1) != 0 ||
        setenv("USP_SIM_IDX", idx, 1) != 0 ||
        setenv("USP_SIM_MAC", mac, 1) != 0 ||
        setenv("USP_SIM_TAG", id->sn, 1) != 0 ||
        setenv("TR181_MOCK_DB", mock, 1) != 0)
    {
        return -1;
    }
    return 0;
}

// Runs in the forked child; never returns
static void ExecChild(const sim_ctx_t *ctx, const sim_identity_t *id, pid_t supervisor)
{
    char **av;

#ifdef __linux__
    // Die with the supervisor even if it is SIGKILLed, so no orphan keeps the socket/DB busy
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != supervisor) _exit(127);
#else
    (void)supervisor;
#endif
    ResetChildSignals();
    if (SetChildEnv(id) != 0) _exit(127);
    av = SIM_BuildChildArgv(ctx->self, ctx->fwd, ctx->fwd_argc, &ctx->cfg, id);
    if (av == NULL) _exit(127);
    execv(ctx->self, av);
    fprintf(stderr, "sim: idx=%lu: execv(%s) failed: %s\n", id->idx, ctx->self, strerror(errno));
    _exit(127);
}

static void Launch(sim_ctx_t *ctx, sim_inst_t *in, double now)
{
    sigset_t block, old;
    pid_t me = getpid();
    pid_t pid;

    // Keep stop signals pending across fork, so the child only sees them after resetting its handlers
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &old);
    fflush(NULL);
    pid = fork();
    if (pid == 0) ExecChild(ctx, &in->id, me);
    sigprocmask(SIG_SETMASK, &old, NULL);

    if (pid < 0)
    {
        Out("sim: launch idx=%lu failed: fork: %s (retrying in %.0fs)\n", in->id.idx, strerror(errno), SIM_LAUNCH_RETRY);
        in->due = now + SIM_LAUNCH_RETRY;
        return;
    }
    in->pid = pid;
    in->state = INST_RUNNING;
    in->started = now;
    Out("sim: launch idx=%lu pid=%ld\n", in->id.idx, (long)pid);
}

// Launches due instances in idx order, as fast as the ramp allows
static void LaunchDue(sim_ctx_t *ctx, double now)
{
    for (size_t i = 0; i < ctx->count; i++)
    {
        sim_inst_t *in = &ctx->inst[i];
        if (stop_requested) return;
        if (in->state != INST_PENDING || in->due > now) continue;
        if (!SIM_RampTake(&ctx->ramp, now)) return;
        Launch(ctx, in, now);
    }
}

/*********************************************************************//**
** Reaping and restart policy
**************************************************************************/
static sim_inst_t *FindByPid(sim_ctx_t *ctx, pid_t pid)
{
    for (size_t i = 0; i < ctx->count; i++)
        if (ctx->inst[i].state == INST_RUNNING && ctx->inst[i].pid == pid) return &ctx->inst[i];
    return NULL;
}

static void DescribeStatus(int st, char *buf, size_t len)
{
    if (WIFEXITED(st)) snprintf(buf, len, "exit=%d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) snprintf(buf, len, "signal=%d", WTERMSIG(st));
    else snprintf(buf, len, "status=0x%x", (unsigned)st);
}

static const char *ActionName(sim_action_t a)
{
    switch (a)
    {
        case SIM_ACT_STAY_DOWN:     return "stay-down";
        case SIM_ACT_RESTART:       return "restart";
        case SIM_ACT_FACTORY_RESET: return "factory-reset";
        default:                    return "crash";
    }
}

static void ApplyExitPolicy(sim_ctx_t *ctx, sim_inst_t *in, pid_t pid, int st, double now)
{
    char how[32], mock[PATH_MAX];
    sim_action_t act = SIM_ClassifyExit(st);
    double uptime = now - in->started;

    DescribeStatus(st, how, sizeof(how));
    in->pid = -1;
    if (stop_requested)
    {
        in->state = INST_STOPPED;
        Out("sim: exit idx=%lu pid=%ld %s during shutdown\n", in->id.idx, (long)pid, how);
        return;
    }
    Out("sim: exit idx=%lu pid=%ld %s uptime=%.1fs action=%s\n", in->id.idx, (long)pid, how, uptime, ActionName(act));
    in->state = INST_PENDING;
    in->due = now;
    switch (act)
    {
        case SIM_ACT_STAY_DOWN:
            in->state = INST_STOPPED;
            break;
        case SIM_ACT_FACTORY_RESET:
            if (JoinPath(mock, sizeof(mock), in->id.dir, "mock.db") == 0) RemoveFile(mock);
            // fall through
        case SIM_ACT_RESTART:
            in->crashes = 0;
            ctx->restarts++;
            break;
        case SIM_ACT_CRASH:
        default:
            if (SIM_ShouldResetCrashCount(uptime)) in->crashes = 0;
            in->crashes++;
            ctx->crash_total++;
            in->due = now + SIM_BackoffSeconds(in->crashes);
            Out("sim: idx=%lu consecutive crashes=%u, relaunch in %us\n", in->id.idx, in->crashes, SIM_BackoffSeconds(in->crashes));
            break;
    }
}

// Reaps every exited child and applies the restart policy
static void ReapChildren(sim_ctx_t *ctx, double now)
{
    int st;
    pid_t pid;

    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
    {
        sim_inst_t *in = FindByPid(ctx, pid);
        if (in != NULL) ApplyExitPolicy(ctx, in, pid, st, now);
    }
}

/*********************************************************************//**
** Step 7: summary
**************************************************************************/
static void PrintSummary(const sim_ctx_t *ctx)
{
    size_t running = 0, stopped = 0, pending = 0;
    for (size_t i = 0; i < ctx->count; i++)
    {
        switch (ctx->inst[i].state)
        {
            case INST_RUNNING: running++; break;
            case INST_STOPPED: stopped++; break;
            default:           pending++; break;
        }
    }
    Out("sim: running=%zu/%zu stopped=%zu pending=%zu restarts=%lu crashes=%lu\n",
        running, ctx->count, stopped, pending, ctx->restarts, ctx->crash_total);
}

/*********************************************************************//**
** Step 8: shutdown
**************************************************************************/
static size_t CountRunning(const sim_ctx_t *ctx)
{
    size_t n = 0;
    for (size_t i = 0; i < ctx->count; i++)
        if (ctx->inst[i].state == INST_RUNNING) n++;
    return n;
}

static void SignalRunning(const sim_ctx_t *ctx, int sig)
{
    for (size_t i = 0; i < ctx->count; i++)
        if (ctx->inst[i].state == INST_RUNNING && ctx->inst[i].pid > 0) kill(ctx->inst[i].pid, sig);
}

static void Sleep50ms(void)
{
    struct timespec ts = { 0, SIM_TICK_NS };
    nanosleep(&ts, NULL);   // EINTR (signal) just ends the tick early
}

static void Shutdown(sim_ctx_t *ctx)
{
    double deadline = Now() + SIM_SHUTDOWN_GRACE;
    size_t n = CountRunning(ctx);

    Out("sim: stopping %zu instance(s)\n", n);
    SignalRunning(ctx, SIGTERM);
    while (CountRunning(ctx) > 0 && Now() < deadline)
    {
        ReapChildren(ctx, Now());
        if (CountRunning(ctx) > 0) Sleep50ms();
    }
    n = CountRunning(ctx);
    if (n > 0)
    {
        Out("sim: %zu instance(s) still running after %.0fs, sending SIGKILL\n", n, SIM_SHUTDOWN_GRACE);
        SignalRunning(ctx, SIGKILL);
        for (size_t i = 0; i < ctx->count; i++)
        {
            sim_inst_t *in = &ctx->inst[i];
            int st;
            if (in->state != INST_RUNNING) continue;
            while (waitpid(in->pid, &st, 0) < 0 && errno == EINTR) {}
            in->state = INST_STOPPED;
            in->pid = -1;
        }
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    PrintSummary(ctx);
}

/*********************************************************************//**
** Step 6: main loop
**************************************************************************/
static void RunLoop(sim_ctx_t *ctx)
{
    double now = Now();
    double next_summary = now + SIM_SUMMARY_PERIOD;

    SIM_RampInit(&ctx->ramp, ctx->cfg.ramp, now);
    for (size_t i = 0; i < ctx->count; i++) ctx->inst[i].due = now;
    Out("sim: supervising %zu instance(s) under %s (pid %ld)\n", ctx->count, ctx->cfg.root, (long)getpid());

    while (!stop_requested)
    {
        now = Now();
        ReapChildren(ctx, now);
        if (stop_requested) break;
        LaunchDue(ctx, now);
        if (now >= next_summary)
        {
            PrintSummary(ctx);
            next_summary = now + SIM_SUMMARY_PERIOD;
        }
        Sleep50ms();
    }
    Shutdown(ctx);
}

/*********************************************************************//**
**
** SIM_Main
**
** Entry point of the CPE simulator supervisor: forks/execs procs x per-proc obuspa
** instances, restarts them according to their exit codes, and stops them on SIGINT/SIGTERM.
**
** \param   argc - number of command line arguments
** \param   argv - array of command line arguments
**
** \return  0 after an orderly shutdown, 2 on configuration errors, 1 on runtime errors
**
**************************************************************************/
int SIM_Main(int argc, char **argv)
{
    sim_ctx_t *ctx = calloc(1, sizeof(sim_ctx_t));
    int rc;

    if (ctx == NULL)
    {
        Err("out of memory");
        return 1;
    }
    ctx->lock_fd = -1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    rc = LoadConfig(ctx, argc, argv);
    if (rc == 0) rc = AcquireLock(ctx);
    if (rc == 0) rc = CheckSimConf(ctx);
    if (rc == 0) rc = PrepareInstances(ctx);
    if (rc == 0) rc = ResolveSelf(ctx, argv[0]);
    if (rc == 0) rc = InstallSignals();
    if (rc == 0)
    {
        CheckLimits(ctx);
        RunLoop(ctx);
    }

    ReleaseLock(ctx);
    free(ctx->inst);
    free(ctx->fwd);
    free(ctx);
    return rc;
}
