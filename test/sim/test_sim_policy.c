#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <sys/wait.h>
#include "test_util.h"
#include "sim_policy.h"

static void test_classify(void)
{
    CHECK(SIM_ClassifyExit(W_EXITCODE(0, 0)) == SIM_ACT_STAY_DOWN, "exit 0");
    CHECK(SIM_ClassifyExit(W_EXITCODE(10, 0)) == SIM_ACT_RESTART, "exit 10");
    CHECK(SIM_ClassifyExit(W_EXITCODE(11, 0)) == SIM_ACT_FACTORY_RESET, "exit 11");
    CHECK(SIM_ClassifyExit(W_EXITCODE(1, 0)) == SIM_ACT_CRASH, "exit 1");
    CHECK(SIM_ClassifyExit(W_EXITCODE(255, 0)) == SIM_ACT_CRASH, "exit 255");
    CHECK(SIM_ClassifyExit(W_EXITCODE(0, SIGKILL)) == SIM_ACT_CRASH, "SIGKILL");
    CHECK(SIM_ClassifyExit(W_EXITCODE(0, SIGSEGV)) == SIM_ACT_CRASH, "SIGSEGV");
    CHECK(SIM_ClassifyExit(0x137f) == SIM_ACT_CRASH, "stopped");
}

static void test_backoff_sequence(void)
{
    unsigned exp[] = {1, 2, 4, 8, 16, 32, 60, 60};
    unsigned n;
    CHECK(SIM_BackoffSeconds(0) == 0, "n=0");
    for (n = 1; n <= 8; n++)
        CHECK(SIM_BackoffSeconds(n) == exp[n - 1], "n=%u got %u", n, SIM_BackoffSeconds(n));
    CHECK(SIM_BackoffSeconds(1000) == 60, "n=1000");
    CHECK(SIM_BackoffSeconds(0xFFFFFFFFu) == 60, "n=max");
}

static void test_backoff_reset(void)
{
    CHECK(!SIM_ShouldResetCrashCount(59.9), "59.9");
    CHECK(SIM_ShouldResetCrashCount(60.0), "60.0");
}

static void test_ramp_limits(void)
{
    sim_ramp_t r;
    int i;
    SIM_RampInit(&r, 20, 0);
    for (i = 0; i < 20; i++)
        CHECK(SIM_RampTake(&r, 0), "take %d", i);
    CHECK(!SIM_RampTake(&r, 0), "21st");
    for (i = 0; i < 10; i++)
        CHECK(SIM_RampTake(&r, 0.5), "refill %d", i);
    CHECK(!SIM_RampTake(&r, 0.5), "after refill");
    // clock going backwards adds nothing
    CHECK(!SIM_RampTake(&r, 0.1), "backwards");
    // burst cap
    SIM_RampInit(&r, 2, 0);
    CHECK(SIM_RampTake(&r, 100) && SIM_RampTake(&r, 100) && !SIM_RampTake(&r, 100), "cap");
    // rate < 1 => burst 1
    SIM_RampInit(&r, 0.5, 0);
    CHECK(SIM_RampTake(&r, 0) && !SIM_RampTake(&r, 0) && !SIM_RampTake(&r, 1) && SIM_RampTake(&r, 2), "slow");
}

static void test_ramp_unlimited(void)
{
    sim_ramp_t r;
    int i, ok = 1;
    SIM_RampInit(&r, 0, 0);
    for (i = 0; i < 10000; i++)
        ok &= SIM_RampTake(&r, 0);
    CHECK(ok, "unlimited");
}

int main(void)
{
    test_classify();
    test_backoff_sequence();
    test_backoff_reset();
    test_ramp_limits();
    test_ramp_unlimited();
    if (g_test_failures) { fprintf(stderr, "%d failure(s)\n", g_test_failures); return 1; }
    printf("test_sim_policy: OK\n");
    return 0;
}
