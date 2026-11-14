/*
 * CPE simulator: --sim-* argument parsing and child argv construction.
 * Does not use getopt, so it cannot disturb main()'s getopt state.
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_args.h"

static void SetErr(char *err, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || len == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

static bool IsSimToken(const char *s)
{
    return strncmp(s, "--sim-", 6) == 0 || strncmp(s, "-sim-", 5) == 0;
}

bool SIM_IsRequested(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i] != NULL && IsSimToken(argv[i])) return true;
    }
    return false;
}

typedef struct { const char *name; const char *hint; } conflict_t;

static const conflict_t conflicts[] = {
    { "-f", NULL }, { "--dbfile", NULL }, { "-dbfile", NULL },
    { "-s", NULL }, { "--cli", NULL }, { "-cli", NULL },
    { "-r", "use --sim-reset instead" }, { "--resetfile", "use --sim-reset instead" }, { "-resetfile", "use --sim-reset instead" },
    { "-l", "use --sim-log instead" }, { "--log", "use --sim-log instead" }, { "-log", "use --sim-log instead" },
    { "-c", NULL }, { "--command", NULL }, { "-command", NULL },
};

// Returns the matching conflict entry, or NULL
static const conflict_t *FindConflict(const char *tok)
{
    for (size_t i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); i++) {
        size_t n = strlen(conflicts[i].name);
        if (strncmp(tok, conflicts[i].name, n) == 0 && (tok[n] == '\0' || tok[n] == '=')) return &conflicts[i];
    }
    return NULL;
}

static int ParseUnsignedLong(const char *s, unsigned long *out)
{
    char *end;
    if (s[0] == '\0' || s[0] == '-' || s[0] == '+' || s[0] == ' ') return -1;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *out = v;
    return 0;
}

static int CopyValue(char *dst, size_t dst_len, const char *val)
{
    if (strlen(val) >= dst_len) return -1;
    strcpy(dst, val);
    return 0;
}

enum { O_PROCS, O_PER_PROC, O_BASE, O_ROOT, O_OUI, O_SN_FMT, O_EID_FMT, O_RAMP, O_RESET, O_LOG, O_REIDENTIFY };

static const struct { const char *name; int id; bool has_value; } opts[] = {
    { "procs", O_PROCS, true }, { "per-proc", O_PER_PROC, true }, { "base", O_BASE, true },
    { "root", O_ROOT, true }, { "oui", O_OUI, true }, { "sn-fmt", O_SN_FMT, true },
    { "eid-fmt", O_EID_FMT, true }, { "ramp", O_RAMP, true }, { "reset", O_RESET, true },
    { "log", O_LOG, true }, { "reidentify", O_REIDENTIFY, false },
};

static int ApplyOption(int id, const char *name, const char *val, sim_config_t *cfg, char *err, size_t err_len)
{
    unsigned long n;
    switch (id) {
    case O_PROCS:
    case O_PER_PROC:
        if (ParseUnsignedLong(val, &n) != 0 || n < 1 || n > 0xFFFFFFFFul) {
            SetErr(err, err_len, "--sim-%s: '%s' is not an integer >= 1", name, val);
            return -1;
        }
        if (id == O_PROCS) cfg->procs = (unsigned)n; else cfg->per_proc = (unsigned)n;
        return 0;
    case O_BASE:
        if (ParseUnsignedLong(val, &n) != 0) {
            SetErr(err, err_len, "--sim-base: '%s' is not a non-negative integer", val);
            return -1;
        }
        cfg->base = n;
        return 0;
    case O_RAMP: {
        char *end;
        double d;
        errno = 0;
        d = strtod(val, &end);
        if (val[0] == '\0' || errno != 0 || *end != '\0' || !isfinite(d) || d < 0) {
            SetErr(err, err_len, "--sim-ramp: '%s' is not a number >= 0", val);
            return -1;
        }
        cfg->ramp = d;
        return 0;
    }
    case O_LOG:
        if (strcmp(val, "stdout") != 0 && strcmp(val, "syslog") != 0 && strcmp(val, "file") != 0) {
            SetErr(err, err_len, "--sim-log: '%s' must be stdout, syslog or file", val);
            return -1;
        }
        return CopyValue(cfg->log_mode, sizeof(cfg->log_mode), val);
    default:
        break;
    }

    char *dst; size_t dl;
    switch (id) {
    case O_ROOT: dst = cfg->root; dl = sizeof(cfg->root); break;
    case O_OUI: dst = cfg->oui; dl = sizeof(cfg->oui); break;
    case O_SN_FMT: dst = cfg->sn_fmt; dl = sizeof(cfg->sn_fmt); break;
    case O_EID_FMT: dst = cfg->eid_fmt; dl = sizeof(cfg->eid_fmt); break;
    default: dst = cfg->reset_tmpl; dl = sizeof(cfg->reset_tmpl); break;
    }
    if (CopyValue(dst, dl, val) != 0) {
        SetErr(err, err_len, "--sim-%s: value too long (max %zu characters)", name, dl - 1);
        return -1;
    }
    return 0;
}

int SIM_ParseArgs(int argc, char **argv, sim_config_t *cfg, char ***fwd, int *fwd_argc, char *err, size_t err_len)
{
    char **out = calloc((size_t)(argc > 0 ? argc : 1), sizeof(char *));
    int nout = 0;
    if (out == NULL) {
        SetErr(err, err_len, "out of memory");
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        const char *tok = argv[i];
        if (!IsSimToken(tok)) {
            const conflict_t *c = FindConflict(tok);
            if (c != NULL) {
                SetErr(err, err_len, "option '%s' conflicts with simulator mode%s%s",
                       tok, c->hint ? "; " : "", c->hint ? c->hint : "");
                goto fail;
            }
            out[nout++] = argv[i];
            continue;
        }

        const char *name = tok + (tok[1] == '-' ? 6 : 5);
        const char *eq = strchr(name, '=');
        size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
        int k, found = -1;
        for (k = 0; k < (int)(sizeof(opts) / sizeof(opts[0])); k++) {
            if (strlen(opts[k].name) == nlen && strncmp(opts[k].name, name, nlen) == 0) { found = k; break; }
        }
        if (found < 0) {
            SetErr(err, err_len, "unknown option '%s'", tok);
            goto fail;
        }

        if (!opts[found].has_value) {
            if (eq != NULL) {
                SetErr(err, err_len, "--sim-%s does not take a value", opts[found].name);
                goto fail;
            }
            cfg->reidentify = true;
            continue;
        }

        const char *val;
        if (eq != NULL) {
            val = eq + 1;
        } else if (i + 1 < argc) {
            val = argv[++i];
        } else {
            SetErr(err, err_len, "--sim-%s requires a value", opts[found].name);
            goto fail;
        }
        if (ApplyOption(opts[found].id, opts[found].name, val, cfg, err, err_len) != 0) {
            if (err != NULL && err_len > 0 && err[0] == '\0') SetErr(err, err_len, "--sim-%s: invalid value", opts[found].name);
            goto fail;
        }
    }

    *fwd = out;
    *fwd_argc = nout;
    return 0;

fail:
    free(out);
    *fwd = NULL;
    *fwd_argc = 0;
    return -1;
}

static char *JoinPath(const char *dir, const char *leaf)
{
    size_t n = strlen(dir) + 1 + strlen(leaf) + 1;
    char *p = malloc(n);
    if (p != NULL) snprintf(p, n, "%s/%s", dir, leaf);
    return p;
}

char **SIM_BuildChildArgv(const char *self, char **fwd, int fwd_argc, const sim_config_t *cfg, const sim_identity_t *id)
{
    size_t cap = 1 + (size_t)fwd_argc + 4 + 2 + 2 + 1;
    char **av = calloc(cap, sizeof(char *));
    size_t n = 0;
    if (av == NULL) return NULL;

    av[n++] = (char *)self;
    for (int i = 0; i < fwd_argc; i++) av[n++] = fwd[i];

    av[n++] = "-f";
    if ((av[n++] = JoinPath(id->dir, "usp.db")) == NULL) goto fail;
    av[n++] = "-s";
    if ((av[n++] = JoinPath(id->dir, "cli.sock")) == NULL) goto fail;
    if (cfg->reset_tmpl[0] != '\0') {
        av[n++] = "-r";
        if ((av[n++] = JoinPath(id->dir, "reset.txt")) == NULL) goto fail;
    }
    if (strcmp(cfg->log_mode, "file") == 0) {
        av[n++] = "-l";
        if ((av[n++] = JoinPath(id->dir, "agent.log")) == NULL) goto fail;
    } else if (strcmp(cfg->log_mode, "syslog") == 0) {
        av[n++] = "-l";
        av[n++] = "syslog";
    }
    av[n] = NULL;
    return av;

fail:
    free(av);   // already-allocated path strings are leaked by design (child execs right after); allocation failure only
    return NULL;
}
