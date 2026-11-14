/*
 * CPE simulator: identity, template and sim.conf logic.
 * Pure libc; no USP headers.
 */
#ifndef SIM_IDENTITY_H
#define SIM_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

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
void SIM_FormatMac(const unsigned char mac[6], bool colons, char *buf, size_t len);
void SIM_PercentEncode(const char *src, char *dst, size_t dst_len);
int  SIM_ExpandTemplate(const char *in, const sim_identity_t *id, char *out, size_t out_len, char *err, size_t err_len);
int  SIM_ConfWrite(const char *path, const sim_config_t *cfg);
int  SIM_ConfCompare(const char *path, const sim_config_t *cfg, char *diff, size_t diff_len); // 0 same, 1 differs, -1 missing
void SIM_MapWriteHeader(FILE *f);
void SIM_MapWriteRow(FILE *f, const sim_identity_t *id);

#endif
