/*
 * CPE simulator: restart policy. Pure libc; no USP headers.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <sys/wait.h>
#include "sim_policy.h"

#define SIM_BACKOFF_MAX_SECONDS   60
#define SIM_CRASH_RESET_UPTIME    60.0

sim_action_t SIM_ClassifyExit(int wait_status)
{
    if (WIFEXITED(wait_status))
    {
        switch (WEXITSTATUS(wait_status))
        {
            case SIM_EXIT_STOP:          return SIM_ACT_STAY_DOWN;
            case SIM_EXIT_REBOOT:        return SIM_ACT_RESTART;
            case SIM_EXIT_FACTORY_RESET: return SIM_ACT_FACTORY_RESET;
            default:                     return SIM_ACT_CRASH;
        }
    }
    return SIM_ACT_CRASH;   // signalled, stopped or anything unexpected
}

unsigned SIM_BackoffSeconds(unsigned consecutive_crashes)
{
    if (consecutive_crashes == 0)
    {
        return 0;
    }
    if (consecutive_crashes > 6)    // 1<<6 == 64 > 60
    {
        return SIM_BACKOFF_MAX_SECONDS;
    }
    unsigned s = 1u << (consecutive_crashes - 1);
    return (s > SIM_BACKOFF_MAX_SECONDS) ? SIM_BACKOFF_MAX_SECONDS : s;
}

bool SIM_ShouldResetCrashCount(double uptime_seconds)
{
    return uptime_seconds >= SIM_CRASH_RESET_UPTIME;
}

void SIM_RampInit(sim_ramp_t *r, double rate, double now)
{
    r->rate = (rate > 0) ? rate : 0;
    r->burst = (r->rate > 1) ? r->rate : 1;
    r->tokens = r->burst;
    r->last = now;
}

bool SIM_RampTake(sim_ramp_t *r, double now)
{
    if (r->rate <= 0)
    {
        return true;
    }
    if (now > r->last)
    {
        r->tokens += (now - r->last) * r->rate;
        if (r->tokens > r->burst)
        {
            r->tokens = r->burst;
        }
        r->last = now;
    }
    if (r->tokens >= 1.0)
    {
        r->tokens -= 1.0;
        return true;
    }
    return false;
}
