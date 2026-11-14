#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h"
#include "sim_identity.h"

static void base_cfg(sim_config_t *c)
{
    SIM_ConfigDefaults(c, "/tmp/simroot", "012345");
    c->procs = 3;
    c->per_proc = 100;
}

static void test_identity_basic(void)
{
    sim_config_t c; sim_identity_t id; char err[256] = "";
    base_cfg(&c);
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == 0, "%s", err);
    CHECK(SIM_MakeIdentity(&c, 2, 5, &id, err, sizeof(err)) == 0, "%s", err);
    CHECK(id.idx == 205, "idx %lu", id.idx);
    CHECK(strcmp(id.sn, "SIM00000205") == 0, "sn %s", id.sn);
    CHECK(strcmp(id.eid, "os::012345-SIM00000205") == 0, "eid %s", id.eid);
    char mac[32]; SIM_FormatMac(id.mac, true, mac, sizeof(mac));
    CHECK(strcmp(mac, "02:00:00:00:CD:00") == 0, "mac %s", mac);
    CHECK(strcmp(id.dir, "/tmp/simroot/cpe-00000205") == 0, "dir %s", id.dir);
    CHECK(SIM_MakeIdentity(&c, 3, 0, &id, err, sizeof(err)) == -1, "proc out of range");
    CHECK(SIM_MakeIdentity(&c, 0, 100, &id, err, sizeof(err)) == -1, "inst out of range");
}

static void test_identity_base(void)
{
    sim_config_t c; sim_identity_t id; char err[256];
    base_cfg(&c); c.base = 1000000;
    CHECK(SIM_MakeIdentity(&c, 0, 0, &id, err, sizeof(err)) == 0, "%s", err);
    CHECK(id.idx == 1000000, "idx");
    CHECK(strcmp(id.sn, "SIM01000000") == 0, "sn %s", id.sn);
}

static void test_eid_percent_encode(void)
{
    sim_config_t c; sim_identity_t id; char err[256];
    base_cfg(&c);
    strcpy(c.oui, "01 23"); strcpy(c.sn_fmt, "A/B%u");
    CHECK(SIM_MakeIdentity(&c, 0, 7, &id, err, sizeof(err)) == 0, "%s", err);
    CHECK(strcmp(id.eid, "os::01%2023-A%2FB7") == 0, "eid %s", id.eid);
    char d[8]; SIM_PercentEncode("a-._~", d, sizeof(d));
    CHECK(strcmp(d, "a-._%7E") == 0, "enc %s", d);
}

static void test_eid_fmt_custom(void)
{
    sim_config_t c; sim_identity_t id; char err[256];
    base_cfg(&c);
    strcpy(c.eid_fmt, "proto::${OUI}.${SN}");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == 0, "%s", err);
    CHECK(SIM_MakeIdentity(&c, 0, 3, &id, err, sizeof(err)) == 0, "%s", err);
    CHECK(strcmp(id.eid, "proto::012345.SIM00000003") == 0, "eid %s", id.eid);
    strcpy(c.eid_fmt, "${EID}-${SN}");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "self reference");
    strcpy(c.eid_fmt, "${SN}${BAD}");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "unknown placeholder");
}

static void test_sn_fmt_requires_conversion(void)
{
    sim_config_t c; char err[256] = "";
    base_cfg(&c); strcpy(c.sn_fmt, "ABC");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "should fail");
    CHECK(strstr(err, "sn-fmt") != NULL, "err %s", err);
}

static void test_sn_fmt_rejects_two_conversions(void)
{
    sim_config_t c; char err[256];
    base_cfg(&c); strcpy(c.sn_fmt, "%u-%u");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "two conv");
    strcpy(c.sn_fmt, "%%%05x");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == 0, "%s", err);
    strcpy(c.sn_fmt, "%lu");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "length modifier");
    strcpy(c.sn_fmt, "%s");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "%%s");
    strcpy(c.sn_fmt, "A B%u");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "whitespace");
}

static void test_eid_fmt_requires_varying_placeholder(void)
{
    sim_config_t c; char err[256];
    base_cfg(&c);
    strcpy(c.eid_fmt, "os::x-${OUI}");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "constant");
    strcpy(c.eid_fmt, "os::${IDX}");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == 0, "%s", err);
    strcpy(c.eid_fmt, "${MACX}");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == 0, "%s", err);
}

static void test_idx_overflow(void)
{
    sim_config_t c; char err[256];
    base_cfg(&c); c.base = 0xFFFFFFFFUL; c.procs = 1; c.per_proc = 2;
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "overflow");
    c.per_proc = 1;
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == 0, "%s", err);
}

static void test_socket_path_too_long(void)
{
    sim_config_t c; char err[256] = "";
    base_cfg(&c); memset(c.root, 'a', 100); c.root[100] = 0;
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "too long");
    CHECK(strstr(err, "cli.sock") != NULL, "err %s", err);
}

static void test_template_all_placeholders(void)
{
    sim_config_t c; sim_identity_t id; char err[256], out[512];
    base_cfg(&c);
    SIM_MakeIdentity(&c, 2, 5, &id, err, sizeof(err));
    CHECK(SIM_ExpandTemplate("${IDX} ${PROC} ${INST} ${OUI} ${SN} ${EID} ${MAC} ${MACX}",
          &id, out, sizeof(out), err, sizeof(err)) == 0, "%s", err);
    CHECK(strcmp(out, "205 2 5 012345 SIM00000205 os::012345-SIM00000205 02:00:00:00:CD:00 02000000CD00") == 0,
          "out %s", out);
}

static void test_template_errors(void)
{
    sim_config_t c; sim_identity_t id; char err[256] = "", out[512];
    base_cfg(&c);
    SIM_MakeIdentity(&c, 2, 5, &id, err, sizeof(err));
    CHECK(SIM_ExpandTemplate("x ${SNN} y", &id, out, sizeof(out), err, sizeof(err)) == -1, "unknown");
    CHECK(strstr(err, "${SNN}") != NULL, "err %s", err);
    CHECK(SIM_ExpandTemplate("x ${SN", &id, out, sizeof(out), err, sizeof(err)) == -1, "unterminated");
    CHECK(SIM_ExpandTemplate("${SN} ${SN} ${SN}", &id, out, 8, err, sizeof(err)) == -1, "overflow");
    CHECK(SIM_ExpandTemplate("a$b $ c", &id, out, sizeof(out), err, sizeof(err)) == 0 && strcmp(out, "a$b $ c") == 0,
          "lone dollar");
}

static void test_conf_roundtrip(void)
{
    sim_config_t c, d; char diff[256] = "", path[64];
    snprintf(path, sizeof(path), "/tmp/sim_test_conf_%d", (int)getpid());
    base_cfg(&c);
    CHECK(SIM_ConfWrite(path, &c) == 0, "write");
    CHECK(SIM_ConfCompare(path, &c, diff, sizeof(diff)) == 0, "same");
    d = c; strcpy(d.sn_fmt, "X%u");
    CHECK(SIM_ConfCompare(path, &d, diff, sizeof(diff)) == 1, "differs");
    CHECK(strstr(diff, "sn_fmt") != NULL, "diff %s", diff);
    d = c; d.procs = 9; d.per_proc = 9;
    CHECK(SIM_ConfCompare(path, &d, diff, sizeof(diff)) == 0, "procs ignored");
    unlink(path);
    CHECK(SIM_ConfCompare(path, &c, diff, sizeof(diff)) == -1, "missing");
}

static void test_conf_corrupt_and_injection(void)
{
    sim_config_t c; char diff[256] = "", path[64], err[256] = "";
    snprintf(path, sizeof(path), "/tmp/sim_test_conf2_%d", (int)getpid());
    base_cfg(&c);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "open");
    if (f == NULL) return;
    fprintf(f, "base=0\noui=012345\nsn_fmt=SIM%%08u\neid_fmt=");
    for (int i = 0; i < 400; i++) fputc('x', f);
    fputs("\nsn_fmt=SIM%08u\n", f);
    fclose(f);
    CHECK(SIM_ConfCompare(path, &c, diff, sizeof(diff)) == 1, "long line differs");
    CHECK(strstr(diff, "too long") != NULL, "diff %s", diff);
    unlink(path);
    strcpy(c.eid_fmt, "${SN}\nbase=5");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "newline in eid_fmt");
    CHECK(strstr(err, "eid-fmt") != NULL, "err %s", err);
    base_cfg(&c); strcpy(c.sn_fmt, "%u\n");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "newline in sn_fmt");
    CHECK(strstr(err, "sn-fmt") != NULL, "err %s", err);
    base_cfg(&c); strcpy(c.oui, "01\n23");
    CHECK(SIM_ValidateConfig(&c, err, sizeof(err)) == -1, "newline in oui");
}

static void test_map_row(void)
{
    sim_config_t c; sim_identity_t id; char err[256], buf[1024] = "";
    base_cfg(&c);
    SIM_MakeIdentity(&c, 2, 5, &id, err, sizeof(err));
    FILE *f = tmpfile();
    CHECK(f != NULL, "tmpfile");
    if (f == NULL) return;
    SIM_MapWriteHeader(f);
    SIM_MapWriteRow(f, &id);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0;
    fclose(f);
    CHECK(strcmp(buf, "idx,proc,inst,sn,eid,mac,dir\n"
        "205,2,5,SIM00000205,os::012345-SIM00000205,02:00:00:00:CD:00,/tmp/simroot/cpe-00000205\n") == 0,
        "map %s", buf);
}

int main(void)
{
    test_identity_basic();
    test_identity_base();
    test_eid_percent_encode();
    test_eid_fmt_custom();
    test_sn_fmt_requires_conversion();
    test_sn_fmt_rejects_two_conversions();
    test_eid_fmt_requires_varying_placeholder();
    test_idx_overflow();
    test_socket_path_too_long();
    test_template_all_placeholders();
    test_template_errors();
    test_conf_roundtrip();
    test_conf_corrupt_and_injection();
    test_map_row();
    if (g_test_failures == 0) printf("PASS test_sim_identity\n");
    return g_test_failures;
}
