/*
 * CPE simulator: restart policy (exit classification, crash backoff, launch-rate limiter).
 * Pure libc; no USP headers. Safe to include from obuspa core (for the SIM_EXIT_* codes).
 */
#ifndef SIM_POLICY_H
#define SIM_POLICY_H

#include <stdbool.h>

#define SIM_EXIT_STOP           0
#define SIM_EXIT_REBOOT         10
#define SIM_EXIT_FACTORY_RESET  11

typedef enum { SIM_ACT_STAY_DOWN, SIM_ACT_RESTART, SIM_ACT_FACTORY_RESET, SIM_ACT_CRASH } sim_action_t;

// Input is a waitpid() status
sim_action_t SIM_ClassifyExit(int wait_status);

// Nth consecutive crash (n>=1) waits min(1<<(n-1), 60) seconds; n==0 returns 0.
// Crash count resets once an instance has run >= 60 seconds.
unsigned SIM_BackoffSeconds(unsigned consecutive_crashes);
bool     SIM_ShouldResetCrashCount(double uptime_seconds);

// Token bucket. Times are seconds from a caller-supplied monotonic clock.
typedef struct { double rate, tokens, burst; double last; } sim_ramp_t;
void SIM_RampInit(sim_ramp_t *r, double rate, double now);   // burst = max(1, rate); rate 0 => unlimited
bool SIM_RampTake(sim_ramp_t *r, double now);                // takes one token if available

#endif
