/*
 * CPE simulator: identity, template and sim.conf logic.
 * Pure libc; no USP headers.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_identity.h"

#define SIM_IDX_MAX 0xFFFFFFFFULL
#define SIM_SOCK_SUFFIX "/cpe-00000000/cli.sock"

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || err_len == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

void SIM_ConfigDefaults(sim_config_t *cfg, const char *default_root, const char *default_oui)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->procs = 1;
    cfg->per_proc = 1;
    cfg->base = 0;
    copy_str(cfg->root, sizeof(cfg->root), default_root);
    copy_str(cfg->oui, sizeof(cfg->oui), default_oui);
    copy_str(cfg->sn_fmt, sizeof(cfg->sn_fmt), "SIM%08u");
    cfg->eid_fmt[0] = '\0';
    cfg->ramp = 20.0;
    cfg->reidentify = false;
    cfg->reset_tmpl[0] = '\0';
    copy_str(cfg->log_mode, sizeof(cfg->log_mode), "stdout");
}

/* Returns true if fmt has exactly one conversion (after removing "%%"),
 * of the form %[flags 0-+ #][width](d|i|u|x|X). */
static bool sn_fmt_ok(const char *fmt, char *why, size_t why_len)
{
    int convs = 0;
    const char *p = fmt;
    while (*p != '\0')
    {
        if (*p != '%') { p++; continue; }
        p++;
        if (*p == '%') { p++; continue; }
        while (*p != '\0' && strchr("0-+ #", *p) != NULL) p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p != 'd' && *p != 'i' && *p != 'u' && *p != 'x' && *p != 'X')
        {
            set_err(why, why_len, "unsupported conversion (only %%[flags][width]d|i|u|x|X allowed)");
            return false;
        }
        p++;
        convs++;
    }
    if (convs != 1)
    {
        set_err(why, why_len, "must contain exactly one conversion, found %d", convs);
        return false;
    }
    return true;
}

/* fmt has been validated by sn_fmt_ok() */
static int format_sn(const char *fmt, unsigned long idx, char *out, size_t out_len, char *err, size_t err_len)
{
    int n;
    size_t i;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    n = snprintf(out, out_len, fmt, (unsigned)idx);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (n < 0 || (size_t)n >= out_len || n >= SIM_SN_MAX)
    {
        set_err(err, err_len, "sn-fmt: generated serial number is too long (max %d chars)", SIM_SN_MAX - 1);
        return -1;
    }
    if (n == 0)
    {
        set_err(err, err_len, "sn-fmt: generated serial number is empty");
        return -1;
    }
    for (i = 0; i < (size_t)n; i++)
    {
        if (isspace((unsigned char)out[i]))
        {
            set_err(err, err_len, "sn-fmt: generated serial number contains whitespace");
            return -1;
        }
    }
    return 0;
}

static void fill_mac(unsigned long idx, unsigned char mac[6])
{
    mac[0] = 0x02;
    mac[1] = (unsigned char)((idx >> 24) & 0xFF);
    mac[2] = (unsigned char)((idx >> 16) & 0xFF);
    mac[3] = (unsigned char)((idx >> 8) & 0xFF);
    mac[4] = (unsigned char)(idx & 0xFF);
    mac[5] = 0x00;
}

void SIM_FormatMac(const unsigned char mac[6], bool colons, char *buf, size_t len)
{
    if (colons)
        snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        snprintf(buf, len, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void SIM_PercentEncode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (dst_len == 0) return;
    for (; *src != '\0'; src++)
    {
        unsigned char c = (unsigned char)*src;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.' || c == '_')
        {
            if (o + 1 >= dst_len) break;
            dst[o++] = (char)c;
        }
        else
        {
            if (o + 3 >= dst_len) break;
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

static int expand(const char *in, const sim_identity_t *id, bool allow_eid,
                  char *out, size_t out_len, char *err, size_t err_len)
{
    size_t o = 0;
    const char *p = in;

    if (out_len == 0) { set_err(err, err_len, "template: output buffer is empty"); return -1; }

    while (*p != '\0')
    {
        char val[SIM_EID_MAX + 64];
        const char *v;
        if (!(p[0] == '$' && p[1] == '{'))
        {
            if (o + 1 >= out_len) goto overflow;
            out[o++] = *p++;
            continue;
        }
        const char *end = strchr(p + 2, '}');
        if (end == NULL)
        {
            set_err(err, err_len, "template: unterminated placeholder in \"%s\"", in);
            return -1;
        }
        size_t nlen = (size_t)(end - (p + 2));
        char name[16];
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;  /* truncated name can't match anything */
        memcpy(name, p + 2, nlen); name[nlen] = '\0';

        v = val;
        if (strcmp(name, "IDX") == 0) snprintf(val, sizeof(val), "%lu", id->idx);
        else if (strcmp(name, "PROC") == 0) snprintf(val, sizeof(val), "%u", id->proc);
        else if (strcmp(name, "INST") == 0) snprintf(val, sizeof(val), "%u", id->inst);
        else if (strcmp(name, "OUI") == 0) v = id->oui;
        else if (strcmp(name, "SN") == 0) v = id->sn;
        else if (strcmp(name, "EID") == 0 && allow_eid) v = id->eid;
        else if (strcmp(name, "MAC") == 0) SIM_FormatMac(id->mac, true, val, sizeof(val));
        else if (strcmp(name, "MACX") == 0) SIM_FormatMac(id->mac, false, val, sizeof(val));
        else
        {
            if (strcmp(name, "EID") == 0)
                set_err(err, err_len, "template: ${EID} cannot be used inside eid-fmt");
            else
                set_err(err, err_len, "template: unknown placeholder ${%.*s}", (int)(end - (p + 2)), p + 2);
            return -1;
        }
        size_t vl = strlen(v);
        if (o + vl + 1 > out_len) goto overflow;
        memcpy(out + o, v, vl);
        o += vl;
        p = end + 1;
    }
    out[o] = '\0';
    return 0;

overflow:
    out[0] = '\0';
    set_err(err, err_len, "template: expanded result exceeds %zu bytes", out_len);
    return -1;
}

int SIM_ExpandTemplate(const char *in, const sim_identity_t *id, char *out, size_t out_len, char *err, size_t err_len)
{
    return expand(in, id, true, out, out_len, err, err_len);
}

int SIM_MakeIdentity(const sim_config_t *cfg, unsigned proc, unsigned inst, sim_identity_t *out, char *err, size_t err_len)
{
    unsigned long long idx;
    char why[128];

    if (proc >= cfg->procs || inst >= cfg->per_proc)
    {
        set_err(err, err_len, "proc %u / inst %u out of range (procs=%u, per-proc=%u)", proc, inst, cfg->procs, cfg->per_proc);
        return -1;
    }
    idx = (unsigned long long)cfg->base + (unsigned long long)proc * cfg->per_proc + inst;
    if (idx > SIM_IDX_MAX)
    {
        set_err(err, err_len, "global index %llu exceeds %llu", idx, SIM_IDX_MAX);
        return -1;
    }
    if (!sn_fmt_ok(cfg->sn_fmt, why, sizeof(why)))
    {
        set_err(err, err_len, "sn-fmt \"%s\": %s", cfg->sn_fmt, why);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->idx = (unsigned long)idx;
    out->proc = proc;
    out->inst = inst;
    copy_str(out->oui, sizeof(out->oui), cfg->oui);
    if (format_sn(cfg->sn_fmt, out->idx, out->sn, sizeof(out->sn), err, err_len) != 0) return -1;
    fill_mac(out->idx, out->mac);

    if (cfg->eid_fmt[0] == '\0')
    {
        char oe[SIM_EID_MAX], se[SIM_EID_MAX];
        SIM_PercentEncode(out->oui, oe, sizeof(oe));
        SIM_PercentEncode(out->sn, se, sizeof(se));
        int n = snprintf(out->eid, sizeof(out->eid), "os::%s-%s", oe, se);
        if (n < 0 || (size_t)n >= sizeof(out->eid))
        {
            set_err(err, err_len, "endpoint id is too long (max %d chars)", SIM_EID_MAX - 1);
            return -1;
        }
    }
    else if (expand(cfg->eid_fmt, out, false, out->eid, sizeof(out->eid), err, err_len) != 0)
    {
        return -1;
    }
    if (out->eid[0] == '\0')
    {
        set_err(err, err_len, "eid-fmt expands to an empty endpoint id");
        return -1;
    }

    int n = snprintf(out->dir, sizeof(out->dir), "%s/cpe-%08lu", cfg->root, out->idx);
    if (n < 0 || (size_t)n >= sizeof(out->dir))
    {
        set_err(err, err_len, "instance directory path is too long");
        return -1;
    }
    return 0;
}

static bool eid_fmt_has_varying(const char *fmt)
{
    return strstr(fmt, "${SN}") || strstr(fmt, "${IDX}") || strstr(fmt, "${MAC}") || strstr(fmt, "${MACX}");
}

static bool has_ctrl(const char *s)
{
    for (; *s != '\0'; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F) return true;
    return false;
}

int SIM_ValidateConfig(const sim_config_t *cfg, char *err, size_t err_len)
{
    unsigned long long last;
    sim_config_t probe;
    sim_identity_t id;
    char why[128];

    if (cfg->procs < 1 || cfg->per_proc < 1)
    {
        set_err(err, err_len, "sim-procs and sim-per-proc must be >= 1");
        return -1;
    }
    last = (unsigned long long)cfg->base + (unsigned long long)cfg->procs * cfg->per_proc - 1;
    if (last > SIM_IDX_MAX)
    {
        set_err(err, err_len, "base + procs*per-proc - 1 = %llu exceeds %llu (32-bit index)", last, SIM_IDX_MAX);
        return -1;
    }
    if (cfg->root[0] == '\0')
    {
        set_err(err, err_len, "sim-root must not be empty");
        return -1;
    }
    if (strlen(cfg->root) + strlen(SIM_SOCK_SUFFIX) >= SIM_SUN_PATH_MAX)
    {
        set_err(err, err_len, "sim-root is too long: \"<root>%s\" must be shorter than %d bytes (unix socket path limit)",
                SIM_SOCK_SUFFIX, SIM_SUN_PATH_MAX);
        return -1;
    }
    if (has_ctrl(cfg->oui)) { set_err(err, err_len, "sim-oui contains control characters"); return -1; }
    if (has_ctrl(cfg->sn_fmt)) { set_err(err, err_len, "sn-fmt contains control characters"); return -1; }
    if (has_ctrl(cfg->eid_fmt)) { set_err(err, err_len, "eid-fmt contains control characters"); return -1; }
    if (cfg->oui[0] == '\0' || strlen(cfg->oui) >= sizeof(cfg->oui))
    {
        set_err(err, err_len, "sim-oui must be non-empty and shorter than %zu chars", sizeof(cfg->oui));
        return -1;
    }
    if (strlen(cfg->sn_fmt) >= sizeof(cfg->sn_fmt))
    {
        set_err(err, err_len, "sn-fmt is too long");
        return -1;
    }
    if (!sn_fmt_ok(cfg->sn_fmt, why, sizeof(why)))
    {
        set_err(err, err_len, "sn-fmt \"%s\": %s", cfg->sn_fmt, why);
        return -1;
    }
    if (cfg->eid_fmt[0] != '\0' && !eid_fmt_has_varying(cfg->eid_fmt))
    {
        set_err(err, err_len, "eid-fmt \"%s\" must contain at least one of ${SN} ${IDX} ${MAC} ${MACX}", cfg->eid_fmt);
        return -1;
    }
    if (cfg->ramp < 0)
    {
        set_err(err, err_len, "sim-ramp must be >= 0");
        return -1;
    }
    if (strcmp(cfg->log_mode, "stdout") != 0 && strcmp(cfg->log_mode, "syslog") != 0 && strcmp(cfg->log_mode, "file") != 0)
    {
        set_err(err, err_len, "sim-log must be stdout, syslog or file");
        return -1;
    }

    /* Trial identities for the first and last index: catches bad SN output, unknown placeholders, overflow */
    probe = *cfg;
    if (SIM_MakeIdentity(&probe, 0, 0, &id, err, err_len) != 0) return -1;
    if (SIM_MakeIdentity(&probe, cfg->procs - 1, cfg->per_proc - 1, &id, err, err_len) != 0) return -1;
    return 0;
}

int SIM_ConfWrite(const char *path, const sim_config_t *cfg)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) return -1;
    fprintf(f, "base=%lu\noui=%s\nsn_fmt=%s\neid_fmt=%s\n", cfg->base, cfg->oui, cfg->sn_fmt, cfg->eid_fmt);
    if (fclose(f) != 0) return -1;
    return 0;
}

static void append(char *diff, size_t diff_len, const char *fmt, ...)
{
    va_list ap;
    size_t cur;
    if (diff == NULL || diff_len == 0) return;
    cur = strlen(diff);
    if (cur >= diff_len - 1) return;
    va_start(ap, fmt);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    vsnprintf(diff + cur, diff_len - cur, fmt, ap);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    va_end(ap);
}

int SIM_ConfCompare(const char *path, const sim_config_t *cfg, char *diff, size_t diff_len)
{
    char line[SIM_EID_MAX + 64];
    char base[32] = "", oui[sizeof(cfg->oui) + 16] = "", sn_fmt[sizeof(cfg->sn_fmt) + 16] = "", eid_fmt[SIM_EID_MAX + 16] = "";
    char cur[32];
    bool have_base = false, have_oui = false, have_sn = false, have_eid = false;
    int differs = 0;
    FILE *f = fopen(path, "r");

    if (diff != NULL && diff_len > 0) diff[0] = '\0';
    if (f == NULL) return -1;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *eq = strchr(line, '=');
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] != '\n' && !feof(f))
        {
            /* over-long line: file is corrupt, report as differing */
            append(diff, diff_len, "sim.conf: line too long (corrupt); ");
            fclose(f);
            return 1;
        }
        if (eq == NULL) continue;
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        *eq++ = '\0';
        if (strcmp(line, "base") == 0) { copy_str(base, sizeof(base), eq); have_base = true; }
        else if (strcmp(line, "oui") == 0) { copy_str(oui, sizeof(oui), eq); have_oui = true; }
        else if (strcmp(line, "sn_fmt") == 0) { copy_str(sn_fmt, sizeof(sn_fmt), eq); have_sn = true; }
        else if (strcmp(line, "eid_fmt") == 0) { copy_str(eid_fmt, sizeof(eid_fmt), eq); have_eid = true; }
    }
    fclose(f);

    snprintf(cur, sizeof(cur), "%lu", cfg->base);
    if (!have_base || strcmp(base, cur) != 0)
    {
        append(diff, diff_len, "base: file=\"%s\" now=\"%s\"; ", base, cur);
        differs = 1;
    }
    if (!have_oui || strcmp(oui, cfg->oui) != 0)
    {
        append(diff, diff_len, "oui: file=\"%s\" now=\"%s\"; ", oui, cfg->oui);
        differs = 1;
    }
    if (!have_sn || strcmp(sn_fmt, cfg->sn_fmt) != 0)
    {
        append(diff, diff_len, "sn_fmt: file=\"%s\" now=\"%s\"; ", sn_fmt, cfg->sn_fmt);
        differs = 1;
    }
    if (!have_eid || strcmp(eid_fmt, cfg->eid_fmt) != 0)
    {
        append(diff, diff_len, "eid_fmt: file=\"%s\" now=\"%s\"; ", eid_fmt, cfg->eid_fmt);
        differs = 1;
    }
    return differs;
}

void SIM_MapWriteHeader(FILE *f)
{
    fputs("idx,proc,inst,sn,eid,mac,dir\n", f);
}

void SIM_MapWriteRow(FILE *f, const sim_identity_t *id)
{
    char mac[32];
    SIM_FormatMac(id->mac, true, mac, sizeof(mac));
    fprintf(f, "%lu,%u,%u,%s,%s,%s,%s\n", id->idx, id->proc, id->inst, id->sn, id->eid, mac, id->dir);
}
