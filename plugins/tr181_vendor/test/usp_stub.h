/*
 * usp_stub.h - registration records captured by usp_stub.c
 */
#ifndef USP_STUB_H
#define USP_STUB_H

#include "usp_api.h"

typedef enum
{
    kStub_Object,
    kStub_Refresh,
    kStub_Param,
    kStub_NumEntries,
} stub_kind_t;

typedef struct
{
    char path[MAX_DM_PATH];
    stub_kind_t kind;
    unsigned type;
    dm_get_value_cb_t get_cb;
    dm_set_value_cb_t set_cb;
    dm_validate_add_cb_t validate_add_cb;
    dm_add_cb_t add_cb;
    dm_del_cb_t del_cb;
    dm_refresh_instances_cb_t refresh_cb;
} stub_reg_t;

#define STUB_MAX_REGS     1024
#define STUB_MAX_REFRESH  256

extern stub_reg_t stub_regs[STUB_MAX_REGS];
extern int stub_num_regs;
extern char stub_refreshed[STUB_MAX_REFRESH][MAX_DM_PATH];
extern int stub_num_refreshed;

stub_reg_t *STUB_Find(const char *path, stub_kind_t kind);
int STUB_Count(stub_kind_t kind);

#endif
