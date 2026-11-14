/*
 * tr181_dm.h
 *
 * Types for the TR-181 data model tables generated from data/tr181_params.csv
 * by tools/gen_tr181_dm.py (see tr181_dm_gen.c)
 */
#ifndef TR181_DM_H
#define TR181_DM_H

#include <stdbool.h>
#include "usp_api.h"

// Multi-instance object, e.g. "Device.WiFi.SSID.{i}"
typedef struct
{
    const char *schema;     // Schema path, ending in ".{i}"
    bool writable;          // Whether a controller may Add/Delete instances
    int parent;             // Index (in tr181_tables[]) of the closest enclosing table, or -1 for top-level tables
} tr181_table_t;

// Parameter, e.g. "Device.WiFi.SSID.{i}.SSID"
typedef struct
{
    const char *schema;     // Schema path
    unsigned type;          // DM_STRING, DM_BOOL, DM_UINT, ...
    bool writable;
} tr181_param_t;

// NumberOfEntries parameter, value maintained by OBUSPA core from the table instances
typedef struct
{
    const char *schema;     // e.g. "Device.WiFi.SSIDNumberOfEntries"
    const char *table;      // e.g. "Device.WiFi.SSID.{i}"
} tr181_numentries_t;

// Concrete path/value pair (seed data for the mock backend)
typedef struct
{
    const char *path;
    const char *value;
} tr181_kv_t;

extern const tr181_table_t tr181_tables[];
extern const int tr181_num_tables;
extern const tr181_param_t tr181_params[];
extern const int tr181_num_params;
extern const tr181_numentries_t tr181_numentries[];
extern const int tr181_num_numentries;
extern const char *tr181_seed_instances[];
extern const int tr181_num_seed_instances;
extern const tr181_kv_t tr181_seed_values[];
extern const int tr181_num_seed_values;

#endif
