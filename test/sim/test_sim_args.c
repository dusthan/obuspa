#include <stdlib.h>
#include <string.h>
#include "test_util.h"
#include "sim_args.h"

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int parse(char **argv, int argc, sim_config_t *cfg, char ***fwd, int *nf, char *err, size_t el)
{
    SIM_ConfigDefaults(cfg, "/tmp/simroot", "00AABB");
    err[0] = '\0';
    return SIM_ParseArgs(argc, argv, cfg, fwd, nf, err, el);
}

static void test_is_requested(void)
{
    char *a[] = {"obuspa", "-p", "--sim-procs", "2"};
    char *b[] = {"obuspa", "-p"};
    char *c[] = {"obuspa", "-sim-base", "1"};
    CHECK(SIM_IsRequested(N(a), a), "a");
    CHECK(!SIM_IsRequested(N(b), b), "b");
    CHECK(SIM_IsRequested(N(c), c), "c");
}

static void test_parse_both_forms(void)
{
    char *a[] = {"obuspa", "--sim-procs", "10", "--sim-per-proc=100", "-sim-base", "5"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == 0, "%s", err);
    CHECK(cfg.procs == 10 && cfg.per_proc == 100 && cfg.base == 5, "values");
    CHECK(nf == 0, "nf");
    free(fwd);
}

static void test_parse_all_options(void)
{
    char *a[] = {"obuspa", "--sim-root", "/r", "--sim-oui=ABCDEF", "--sim-sn-fmt", "S%d", "--sim-eid-fmt", "os::x",
                 "--sim-ramp", "2.5", "--sim-reset", "/t/r.txt", "--sim-log", "file"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == 0, "%s", err);
    CHECK(strcmp(cfg.root, "/r") == 0 && strcmp(cfg.oui, "ABCDEF") == 0, "root/oui");
    CHECK(strcmp(cfg.sn_fmt, "S%d") == 0 && strcmp(cfg.eid_fmt, "os::x") == 0, "fmt");
    CHECK(cfg.ramp == 2.5 && strcmp(cfg.reset_tmpl, "/t/r.txt") == 0 && strcmp(cfg.log_mode, "file") == 0, "rest");
    free(fwd);
}

static void test_parse_forwards_rest(void)
{
    char *a[] = {"obuspa", "-v", "4", "--sim-procs", "2", "-x", "a.so", "-p"};
    char *exp[] = {"-v", "4", "-x", "a.so", "-p"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == 0, "%s", err);
    CHECK(nf == 5, "nf=%d", nf);
    for (int i = 0; i < nf && i < 5; i++) CHECK(strcmp(fwd[i], exp[i]) == 0, "fwd[%d]", i);
    free(fwd);
}

static void test_parse_reidentify_flag(void)
{
    char *a[] = {"obuspa", "--sim-reidentify"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == 0, "%s", err);
    CHECK(cfg.reidentify, "flag");
    free(fwd);
}

static void test_parse_bad_number(void)
{
    char *a[] = {"obuspa", "--sim-procs", "0"};
    char *b[] = {"obuspa", "--sim-procs", "abc"};
    char *c[] = {"obuspa", "--sim-ramp", "-1"};
    char *d[] = {"obuspa", "--sim-procs", "3x"};
    char *e[] = {"obuspa", "--sim-per-proc=-2"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "a");
    CHECK(parse(b, N(b), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "b");
    CHECK(parse(c, N(c), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "c");
    CHECK(parse(d, N(d), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "d");
    CHECK(parse(e, N(e), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "e");
}

static void test_parse_unknown_sim(void)
{
    char *a[] = {"obuspa", "--sim-proc", "3"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "rc");
    CHECK(strstr(err, "--sim-proc") != NULL, "err=%s", err);
}

static void test_parse_missing_value(void)
{
    char *a[] = {"obuspa", "--sim-procs"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "rc");
}

static void test_conflicting_options(void)
{
    const char *bad[] = {"-f", "--dbfile", "-dbfile=x", "-s", "--cli", "-r", "--resetfile", "-l", "--log", "-c", "--command"};
    for (int i = 0; i < N(bad); i++) {
        char *a[] = {"obuspa", (char *)bad[i], "x", "--sim-procs", "1"};
        sim_config_t cfg; char **fwd; int nf; char err[200];
        CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "%s accepted", bad[i]);
        if (strcmp(bad[i], "-r") == 0 || strcmp(bad[i], "--resetfile") == 0)
            CHECK(strstr(err, "--sim-reset") != NULL, "err=%s", err);
        if (strcmp(bad[i], "-l") == 0 || strcmp(bad[i], "--log") == 0)
            CHECK(strstr(err, "--sim-log") != NULL, "err=%s", err);
    }
}

static void test_parse_log_mode(void)
{
    char *a[] = {"obuspa", "--sim-log", "file"};
    char *b[] = {"obuspa", "--sim-log", "foo"};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == 0, "a");
    free(fwd);
    CHECK(parse(b, N(b), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "b");
}

static void test_value_too_long(void)
{
    char big[300];
    memset(big, 'a', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
    char *a[] = {"obuspa", "--sim-oui", big};
    sim_config_t cfg; char **fwd; int nf; char err[200];
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == -1, "long oui");
}

static void test_defaults_preserved(void)
{
    char *a[] = {"obuspa", "--sim-base", "7"};
    sim_config_t cfg, ref; char **fwd; int nf; char err[200];
    SIM_ConfigDefaults(&ref, "/tmp/simroot", "00AABB");
    CHECK(parse(a, N(a), &cfg, &fwd, &nf, err, sizeof(err)) == 0, "rc");
    CHECK(cfg.procs == ref.procs && strcmp(cfg.oui, ref.oui) == 0, "defaults kept");
    free(fwd);
}

static void test_child_argv(void)
{
    sim_config_t cfg; sim_identity_t id; char *fwd[] = {"-x", "a.so"};
    SIM_ConfigDefaults(&cfg, "/tmp/simroot", "00AABB");
    memset(&id, 0, sizeof(id));
    strcpy(id.dir, "/D");
    strcpy(cfg.reset_tmpl, "/t/r.txt");
    strcpy(cfg.log_mode, "file");
    const char *exp[] = {"self", "-x", "a.so", "-f", "/D/usp.db", "-s", "/D/cli.sock", "-r", "/D/reset.txt", "-l", "/D/agent.log", NULL};
    char **av = SIM_BuildChildArgv("self", fwd, 2, &cfg, &id);
    CHECK(av != NULL, "null");
    if (av) {
        int i = 0;
        for (; exp[i] != NULL; i++) CHECK(av[i] && strcmp(av[i], exp[i]) == 0, "av[%d]=%s", i, av[i] ? av[i] : "NULL");
        CHECK(av[i] == NULL, "terminator");
    }

    strcpy(cfg.log_mode, "stdout");
    av = SIM_BuildChildArgv("self", fwd, 2, &cfg, &id);
    CHECK(av && av[9] == NULL && strcmp(av[8], "/D/reset.txt") == 0, "stdout");

    strcpy(cfg.log_mode, "syslog");
    cfg.reset_tmpl[0] = '\0';
    av = SIM_BuildChildArgv("self", fwd, 2, &cfg, &id);
    CHECK(av && strcmp(av[7], "-l") == 0 && strcmp(av[8], "syslog") == 0 && av[9] == NULL, "syslog/no reset");
}

int main(void)
{
    test_is_requested();
    test_parse_both_forms();
    test_parse_all_options();
    test_parse_forwards_rest();
    test_parse_reidentify_flag();
    test_parse_bad_number();
    test_parse_unknown_sim();
    test_parse_missing_value();
    test_conflicting_options();
    test_parse_log_mode();
    test_value_too_long();
    test_defaults_preserved();
    test_child_argv();
    if (g_test_failures) { fprintf(stderr, "%d failure(s)\n", g_test_failures); return 1; }
    printf("test_sim_args: all passed\n");
    return 0;
}
