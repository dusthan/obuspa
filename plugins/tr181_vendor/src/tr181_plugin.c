/*
 * tr181_plugin.c
 *
 * OBUSPA vendor plug-in (loaded with 'obuspa -x libtr181_vendor.so') which registers
 * the TR-181 data model described in data/tr181_params.csv.
 *
 * The data model description (tr181_dm_gen.c) is generated from the CSV by tools/gen_tr181_dm.py.
 * All values are read/written via the backend interface (tr181_backend.h).
 *
 * Paths which OBUSPA core already implements (e.g. Device.DeviceInfo.SoftwareVersion,
 * Device.IP.Diagnostics.IPLayerCapacitySupported) are skipped, so core keeps ownership of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "usp_err_codes.h"
#include "vendor_defs.h"
#include "vendor_api.h"
#include "usp_api.h"

#include "tr181_dm.h"
#include "tr181_backend.h"

//------------------------------------------------------------------------------
// How long (in seconds) OBUSPA may cache table instances before calling the refresh callback again
#ifndef TR181_REFRESH_PERIOD
#define TR181_REFRESH_PERIOD 5
#endif

// Maximum number of instances in any one table
#define TR181_MAX_INSTANCES 256

// The plug-in is built with -fvisibility=hidden; only the entry points looked up by OBUSPA's
// PLUGIN_Load() (via dlsym) are exported
#define TR181_EXPORT __attribute__((visibility("default")))

//------------------------------------------------------------------------------
// Set for each entry in tr181_tables[] that this plug-in registered (rather than skipped)
static bool *table_registered = NULL;

//------------------------------------------------------------------------------
// Forward declarations
static int RegisterTables(void);
static int RegisterParams(void);
static int RegisterNumEntries(void);
static int Tr181_GetValue(dm_req_t *req, char *buf, int len);
static int Tr181_SetValue(dm_req_t *req, char *buf);
static int Tr181_AddInstance(dm_req_t *req);
static int Tr181_DeleteInstance(dm_req_t *req);
static int Tr181_RefreshInstances(int group_id, char *path, int *expiry_period);
static int RefreshTable(int table_index, const char *table_prefix);
static int FormInstantiatedPath(const char *schema, dm_req_instances_t *inst, char *buf, int len);
static const tr181_param_t *FindParam(const char *schema);
static const char *TypeDefault(unsigned type);
static bool IsSecret(const char *schema);

/*********************************************************************//**
**
** VENDOR_Init
**
** Registers the data model provided by this plug-in
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
TR181_EXPORT int VENDOR_Init(void)
{
    int err;

    if (TR181_BE_Init() != TR181_BE_OK)
    {
        USP_LOG_Error("%s: TR-181 backend failed to initialise", __FUNCTION__);
        return USP_ERR_INTERNAL_ERROR;
    }

    table_registered = calloc(tr181_num_tables, sizeof(bool));
    if (table_registered == NULL)
    {
        return USP_ERR_RESOURCES_EXCEEDED;
    }

    // NOTE: Tables must be registered before the parameters and NumberOfEntries parameters within them
    err = RegisterTables();
    if (err == USP_ERR_OK)
    {
        err = RegisterParams();
    }
    if (err == USP_ERR_OK)
    {
        err = RegisterNumEntries();
    }

    return err;
}

/*********************************************************************//**
**
** VENDOR_Start
**
** Called after the data model has been registered and the USP DB has been read
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
TR181_EXPORT int VENDOR_Start(void)
{
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** VENDOR_Stop
**
** Called when OBUSPA is shutting down
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
TR181_EXPORT int VENDOR_Stop(void)
{
    TR181_BE_Stop();
    free(table_registered);
    table_registered = NULL;
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** RegisterTables
**
** Registers all multi-instance objects, parents first
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int RegisterTables(void)
{
    int i;
    int err;
    const tr181_table_t *t;

    for (i=0; i < tr181_num_tables; i++)
    {
        t = &tr181_tables[i];

        // Skip tables already provided by OBUSPA core, and tables inside a table we didn't register
        if ((USP_DM_IsRegistered((char *)t->schema)) || ((t->parent >= 0) && (table_registered[t->parent] == false)))
        {
            USP_LOG_Info("%s: Skipping %s (provided elsewhere)", __FUNCTION__, t->schema);
            continue;
        }

        if (t->writable)
        {
            err = USP_REGISTER_Object((char *)t->schema, NULL, Tr181_AddInstance, NULL,
                                      NULL, Tr181_DeleteInstance, NULL);
        }
        else
        {
            err = USP_REGISTER_Object((char *)t->schema, USP_HOOK_DenyAddInstance, NULL, NULL,
                                      USP_HOOK_DenyDeleteInstance, NULL, NULL);
        }

        // Instances of top-level tables (and all tables nested inside them) are obtained from the backend
        if ((err == USP_ERR_OK) && (t->parent < 0))
        {
            err = USP_REGISTER_Object_RefreshInstances((char *)t->schema, Tr181_RefreshInstances);
        }

        if (err != USP_ERR_OK)
        {
            USP_LOG_Error("%s: Failed to register %s", __FUNCTION__, t->schema);
            return err;
        }

        table_registered[i] = true;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** RegisterParams
**
** Registers all parameters
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int RegisterParams(void)
{
    int i;
    int err;
    int skipped = 0;
    const tr181_param_t *p;

    for (i=0; i < tr181_num_params; i++)
    {
        p = &tr181_params[i];

        if (USP_DM_IsRegistered((char *)p->schema))
        {
            USP_LOG_Info("%s: Skipping %s (provided by OBUSPA core)", __FUNCTION__, p->schema);
            skipped++;
            continue;
        }

        if (p->writable)
        {
            err = USP_REGISTER_VendorParam_ReadWrite((char *)p->schema, Tr181_GetValue, Tr181_SetValue, NULL, p->type);
        }
        else
        {
            err = USP_REGISTER_VendorParam_ReadOnly((char *)p->schema, Tr181_GetValue, p->type);
        }

        if (err != USP_ERR_OK)
        {
            USP_LOG_Error("%s: Failed to register %s", __FUNCTION__, p->schema);
            return err;
        }
    }

    USP_LOG_Info("%s: Registered %d parameters (%d skipped)", __FUNCTION__, tr181_num_params - skipped, skipped);
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** RegisterNumEntries
**
** Registers all NumberOfEntries parameters
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int RegisterNumEntries(void)
{
    int i;
    int err;
    const tr181_numentries_t *ne;

    for (i=0; i < tr181_num_numentries; i++)
    {
        ne = &tr181_numentries[i];
        if (USP_DM_IsRegistered((char *)ne->schema))
        {
            USP_LOG_Info("%s: Skipping %s (provided by OBUSPA core)", __FUNCTION__, ne->schema);
            continue;
        }

        err = USP_REGISTER_Param_NumEntries((char *)ne->schema, (char *)ne->table);
        if (err != USP_ERR_OK)
        {
            USP_LOG_Error("%s: Failed to register %s", __FUNCTION__, ne->schema);
            return err;
        }
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Tr181_GetValue
**
** Get vendor hook for all parameters
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - pointer to buffer in which to return the value
** \param   len - length of return buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int Tr181_GetValue(dm_req_t *req, char *buf, int len)
{
    int err;
    const tr181_param_t *p;

    // TR-181: passwords and keys always read back as an empty string
    if (IsSecret(req->schema_path))
    {
        *buf = '\0';
        return USP_ERR_OK;
    }

    err = TR181_BE_Get(req->path, buf, len);
    if (err == TR181_BE_NOT_FOUND)
    {
        p = FindParam(req->schema_path);
        snprintf(buf, len, "%s", TypeDefault((p != NULL) ? p->type : DM_STRING));
        return USP_ERR_OK;
    }

    if (err != TR181_BE_OK)
    {
        USP_ERR_SetMessage("%s: Failed to get %s", __FUNCTION__, req->path);
        return USP_ERR_INTERNAL_ERROR;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Tr181_SetValue
**
** Set vendor hook for all writable parameters
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - new value (already validated against the parameter's type by OBUSPA core)
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int Tr181_SetValue(dm_req_t *req, char *buf)
{
    const tr181_param_t *p;
    const char *value = buf;

    // Store booleans in canonical form (controller may send "1"/"0")
    p = FindParam(req->schema_path);
    if ((p != NULL) && (p->type == DM_BOOL))
    {
        value = req->val_union.value_bool ? "true" : "false";
    }

    if (TR181_BE_Set(req->path, value) != TR181_BE_OK)
    {
        USP_ERR_SetMessage("%s: Failed to set %s", __FUNCTION__, req->path);
        return USP_ERR_INTERNAL_ERROR;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Tr181_AddInstance
**
** Add vendor hook for writable tables
**
** \param   req - pointer to structure identifying the new object instance
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int Tr181_AddInstance(dm_req_t *req)
{
    char path[MAX_DM_PATH];

    if (FormInstantiatedPath(req->schema_path, req->inst, path, sizeof(path)) != USP_ERR_OK)
    {
        return USP_ERR_INTERNAL_ERROR;
    }

    if (TR181_BE_AddInstance(path) != TR181_BE_OK)
    {
        USP_ERR_SetMessage("%s: Backend failed to add %s", __FUNCTION__, path);
        return USP_ERR_INTERNAL_ERROR;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Tr181_DeleteInstance
**
** Delete vendor hook for writable tables
**
** \param   req - pointer to structure identifying the object instance to delete
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int Tr181_DeleteInstance(dm_req_t *req)
{
    char path[MAX_DM_PATH];

    if (FormInstantiatedPath(req->schema_path, req->inst, path, sizeof(path)) != USP_ERR_OK)
    {
        return USP_ERR_INTERNAL_ERROR;
    }

    if (TR181_BE_DeleteInstance(path) != TR181_BE_OK)
    {
        USP_ERR_SetMessage("%s: Backend failed to delete %s", __FUNCTION__, path);
        return USP_ERR_INTERNAL_ERROR;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Tr181_RefreshInstances
**
** Refresh instances vendor hook for top-level tables. Reports the instances of the
** table and all tables nested within it
**
** \param   group_id - unused
** \param   path - schema path of the top-level table, without the trailing '{i}', e.g. "Device.WiFi.SSID."
** \param   expiry_period - pointer to variable in which to return the cache period (seconds)
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int Tr181_RefreshInstances(int group_id, char *path, int *expiry_period)
{
    int i;
    size_t len;
    const char *schema;

    (void)group_id;
    *expiry_period = TR181_REFRESH_PERIOD;

    len = strlen(path);
    for (i=0; i < tr181_num_tables; i++)
    {
        schema = tr181_tables[i].schema;
        if ((tr181_tables[i].parent < 0) && (strncmp(schema, path, len) == 0) && (strcmp(&schema[len], "{i}") == 0))
        {
            return RefreshTable(i, path);
        }
    }

    USP_ERR_SetMessage("%s: Unknown table %s", __FUNCTION__, path);
    return USP_ERR_INTERNAL_ERROR;
}

/*********************************************************************//**
**
** RefreshTable
**
** Reports the instances of the specified table (and its child tables) to OBUSPA
**
** \param   table_index - index of the table in tr181_tables[]
** \param   table_prefix - concrete path of the table with trailing '.', e.g. "Device.IP.Interface.2.IPv4Address."
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int RefreshTable(int table_index, const char *table_prefix)
{
    int insts[TR181_MAX_INSTANCES];
    int num_insts = 0;
    int i;
    int j;
    int err;
    char obj_path[MAX_DM_PATH];
    char child_prefix[MAX_DM_PATH];
    const char *parent_schema;
    const char *rel;
    size_t rel_len;

    if (TR181_BE_GetInstances(table_prefix, insts, TR181_MAX_INSTANCES, &num_insts) != TR181_BE_OK)
    {
        USP_ERR_SetMessage("%s: Backend failed to get instances of %s", __FUNCTION__, table_prefix);
        return USP_ERR_INTERNAL_ERROR;
    }

    parent_schema = tr181_tables[table_index].schema;
    for (i=0; i < num_insts; i++)
    {
        if (snprintf(obj_path, sizeof(obj_path), "%s%d", table_prefix, insts[i]) >= (int)sizeof(obj_path))
        {
            USP_ERR_SetMessage("%s: Path too long for %s%d", __FUNCTION__, table_prefix, insts[i]);
            return USP_ERR_INTERNAL_ERROR;
        }
        err = USP_DM_RefreshInstance(obj_path);
        if (err != USP_ERR_OK)
        {
            return err;
        }

        // Recurse into child tables. Child schema = parent schema + rel, where rel is e.g. ".IPv4Address.{i}"
        for (j=0; j < tr181_num_tables; j++)
        {
            if ((tr181_tables[j].parent != table_index) || (table_registered[j] == false))
            {
                continue;
            }

            rel = &tr181_tables[j].schema[strlen(parent_schema)];
            rel_len = strlen(rel) - (sizeof("{i}") - 1);
            if (snprintf(child_prefix, sizeof(child_prefix), "%s%.*s", obj_path, (int)rel_len, rel) >= (int)sizeof(child_prefix))
            {
                USP_ERR_SetMessage("%s: Path too long below %s", __FUNCTION__, obj_path);
                return USP_ERR_INTERNAL_ERROR;
            }
            err = RefreshTable(j, child_prefix);
            if (err != USP_ERR_OK)
            {
                return err;
            }
        }
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** FormInstantiatedPath
**
** Replaces each '{i}' in the schema path with the corresponding instance number
**
** \param   schema - schema path, e.g. "Device.Hosts.AccessControl.{i}"
** \param   inst - instance numbers
** \param   buf - buffer in which to return the instantiated path
** \param   len - length of buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
static int FormInstantiatedPath(const char *schema, dm_req_instances_t *inst, char *buf, int len)
{
    int n = 0;
    int k = 0;
    int written;
    const char *p = schema;
    const char *brace;

    while (*p != '\0')
    {
        brace = strstr(p, "{i}");
        if (brace == NULL)
        {
            written = snprintf(&buf[n], len - n, "%s", p);
            n += written;
            break;
        }

        if (k >= inst->order)
        {
            USP_ERR_SetMessage("%s: Not enough instance numbers for %s", __FUNCTION__, schema);
            return USP_ERR_INTERNAL_ERROR;
        }

        written = snprintf(&buf[n], len - n, "%.*s%d", (int)(brace - p), p, inst->instances[k++]);
        n += written;
        if (n >= len)
        {
            break;
        }
        p = brace + sizeof("{i}") - 1;
    }

    if (n >= len)
    {
        USP_ERR_SetMessage("%s: Path too long for %s", __FUNCTION__, schema);
        return USP_ERR_INTERNAL_ERROR;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** FindParam
**
** Finds the description of the parameter with the specified schema path
**
** \param   schema - schema path of the parameter
**
** \return  pointer to parameter description, or NULL if not found
**
**************************************************************************/
static const tr181_param_t *FindParam(const char *schema)
{
    int i;

    for (i=0; i < tr181_num_params; i++)
    {
        if (strcmp(tr181_params[i].schema, schema) == 0)
        {
            return &tr181_params[i];
        }
    }

    return NULL;
}

/*********************************************************************//**
**
** TypeDefault
**
** Returns the value to report for a parameter which the backend has no value for
**
** \param   type - type of the parameter
**
** \return  default value as a string
**
**************************************************************************/
static const char *TypeDefault(unsigned type)
{
    switch (type)
    {
        case DM_BOOL:
            return "false";
        case DM_INT:
        case DM_UINT:
        case DM_LONG:
        case DM_ULONG:
            return "0";
        case DM_DATETIME:
            return "0001-01-01T00:00:00Z";      // TR-106 Unknown Time
        default:
            return "";
    }
}

/*********************************************************************//**
**
** IsSecret
**
** Determines whether the parameter holds a password or key, which TR-181 requires to read back as an empty string
** Define TR181_EXPOSE_SECRETS when building to disable this (e.g. for testing)
**
** \param   schema - schema path of the parameter
**
** \return  true if the parameter's value must not be returned
**
**************************************************************************/
static bool IsSecret(const char *schema)
{
#ifdef TR181_EXPOSE_SECRETS
    (void)schema;
    return false;
#else
    static const char *suffixes[] = { "Password", "Passphrase", "WEPKey", "PreSharedKey", "KeyPassphrase" };
    const char *name;
    size_t name_len;
    size_t suffix_len;
    int i;

    name = strrchr(schema, '.');
    name = (name != NULL) ? name + 1 : schema;
    name_len = strlen(name);

    for (i=0; i < (int)(sizeof(suffixes)/sizeof(suffixes[0])); i++)
    {
        suffix_len = strlen(suffixes[i]);
        if ((name_len >= suffix_len) && (strcmp(&name[name_len - suffix_len], suffixes[i]) == 0))
        {
            return true;
        }
    }

    return false;
#endif
}
