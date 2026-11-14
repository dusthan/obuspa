# Unit tests for the CPE simulator modules (libc only).
# Every test_sim_<x>.c is built against all ../../src/core/sim_*.c except sim_supervisor.c.
CC      ?= cc
CFLAGS  = -std=c99 -Wall -Wextra -Werror -I../../src/core
BUILD   = build
SRCS    = $(filter-out ../../src/core/sim_supervisor.c,$(wildcard ../../src/core/sim_*.c))

.PHONY: check
check:
	@mkdir -p $(BUILD)
	@fail=0; \
	for t in test_sim_*.c; do \
	    n=$${t%.c}; \
	    echo "== $$n"; \
	    $(CC) $(CFLAGS) -o $(BUILD)/$$n $$t $(SRCS) || exit 1; \
	    ./$(BUILD)/$$n || fail=1; \
	done; \
	exit $$fail
