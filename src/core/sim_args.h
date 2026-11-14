/*
 * CPE simulator: --sim-* argument parsing and child argv construction.
 * Pure libc; no USP headers.
 */
#ifndef SIM_ARGS_H
#define SIM_ARGS_H

#include <stdbool.h>
#include <stddef.h>
#include "sim_identity.h"

// True if any argument (after argv[0]) starts with "--sim-" or "-sim-"
bool SIM_IsRequested(int argc, char **argv);

// Parses --sim-X V and --sim-X=V (single dash accepted too). Only overrides options present;
// does per-option checks only (cross-field checks are SIM_ValidateConfig's job).
// Remaining args are copied in order to *fwd (malloc'd array of pointers into argv; caller frees the array only).
// Returns 0 ok, -1 error (err filled).
int  SIM_ParseArgs(int argc, char **argv, sim_config_t *cfg, char ***fwd, int *fwd_argc, char *err, size_t err_len);

// Child argv: [self, fwd..., "-f", dir/usp.db, "-s", dir/cli.sock, ("-r", dir/reset.txt)?, ("-l", dir/agent.log | "syslog")?, NULL]
// One malloc'd NULL-terminated array (strings may be malloc'd too; no free API). NULL on allocation failure.
char **SIM_BuildChildArgv(const char *self, char **fwd, int fwd_argc, const sim_config_t *cfg, const sim_identity_t *id);

#endif
