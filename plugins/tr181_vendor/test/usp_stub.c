/*
 * usp_stub.c
 *
 * Minimal stand-in for the parts of the OBUSPA USP API used by the TR-181 plug-in,
 * so that the plug-in logic can be tested on the host without a full OBUSPA build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "usp_err_codes.h"
#include "vendor_defs.h"
#include "usp_api.h"
#include "usp_stub.h"

bool enable_callstack_debug = false;

stub_reg_t stub_regs[STUB_MAX_REGS];
int stub_num_regs = 0;

char stub_refreshed[STUB_MAX_REFRESH][MAX_DM_PATH];
int stub_num_refreshed = 0;

// Paths registered by OBUSPA core (default build, i.e. REMOVE_DEVICE_INFO not defined, IPLayerCapacity enabled)
static const char *core_paths[] =
{
    "Device.DeviceInfo.SoftwareVersion",
    "Device.DeviceInfo.ProductClass",
    "Device.DeviceInfo.Manufacturer",
    "Device.DeviceInfo.ModelName",
    "Device.DeviceInfo.HardwareVersion",
    "Device.DeviceInfo.UpTime",
    "Device.DeviceInfo.ManufacturerOUI",
    "Device.DeviceInfo.SerialNumber",
    "Device.IP.Diagnostics.IPLayerCapacitySupported",
};

static stub_reg_t *AddReg(char *path, stub_kind_t kind)
{
    stub_reg_t *r;
    int i;

    for (i=0; i < stub_num_regs; i++)
    {
        if ((strcmp(stub_regs[i].path, path) == 0) && (kind != kStub_Refresh))
        {
            fprintf(stderr, "STUB: duplicate registration of %s\n", path);
            return NULL;
        }
    }

    if (stub_num_regs >= STUB_MAX_REGS)
    {
        return NULL;
    }

    r = &stub_regs[stub_num_regs++];
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->kind = kind;
    return r;
}

stub_reg_t *STUB_Find(const char *path, stub_kind_t kind)
{
    int i;
    for (i=0; i < stub_num_regs; i++)
    {
        if ((strcmp(stub_regs[i].path, path) == 0) && (stub_regs[i].kind == kind))
        {
            return &stub_regs[i];
        }
    }
    return NULL;
}

int STUB_Count(stub_kind_t kind)
{
    int i;
    int n = 0;
    for (i=0; i < stub_num_regs; i++)
    {
        if (stub_regs[i].kind == kind)
        {
            n++;
        }
    }
    return n;
}

int USP_REGISTER_Object(char *path, dm_validate_add_cb_t validate_add_cb, dm_add_cb_t add_cb, dm_notify_add_cb_t notify_add_cb,
                        dm_validate_del_cb_t validate_del_cb, dm_del_cb_t del_cb, dm_notify_del_cb_t notify_del_cb)
{
    stub_reg_t *r = AddReg(path, kStub_Object);
    if (r == NULL)
    {
        return USP_ERR_INTERNAL_ERROR;
    }
    r->validate_add_cb = validate_add_cb;
    r->add_cb = add_cb;
    r->del_cb = del_cb;
    return USP_ERR_OK;
}

int USP_REGISTER_Object_RefreshInstances(char *path, dm_refresh_instances_cb_t refresh_instances_cb)
{
    stub_reg_t *r;

    // Must be a top-level table, i.e. exactly one '{i}'
    char *first = strstr(path, "{i}");
    if ((first == NULL) || (strstr(first + 3, "{i}") != NULL))
    {
        fprintf(stderr, "STUB: %s is not a top-level multi-instance object\n", path);
        return USP_ERR_INTERNAL_ERROR;
    }

    r = AddReg(path, kStub_Refresh);
    if (r == NULL)
    {
        return USP_ERR_INTERNAL_ERROR;
    }
    r->refresh_cb = refresh_instances_cb;
    return USP_ERR_OK;
}

int USP_REGISTER_VendorParam_ReadOnly(char *path, dm_get_value_cb_t get_cb, unsigned type_flags)
{
    stub_reg_t *r = AddReg(path, kStub_Param);
    if (r == NULL)
    {
        return USP_ERR_INTERNAL_ERROR;
    }
    r->get_cb = get_cb;
    r->type = type_flags;
    return USP_ERR_OK;
}

int USP_REGISTER_VendorParam_ReadWrite(char *path, dm_get_value_cb_t get_cb, dm_set_value_cb_t set_cb, dm_notify_set_cb_t notify_set_cb, unsigned type_flags)
{
    stub_reg_t *r = AddReg(path, kStub_Param);
    if (r == NULL)
    {
        return USP_ERR_INTERNAL_ERROR;
    }
    r->get_cb = get_cb;
    r->set_cb = set_cb;
    r->type = type_flags;
    return USP_ERR_OK;
}

int USP_REGISTER_Param_NumEntries(char *path, char *table_path)
{
    stub_reg_t *r;

    // Table must already be registered
    if (STUB_Find(table_path, kStub_Object) == NULL)
    {
        fprintf(stderr, "STUB: NumEntries %s refers to unregistered table %s\n", path, table_path);
        return USP_ERR_INTERNAL_ERROR;
    }

    r = AddReg(path, kStub_NumEntries);
    return (r == NULL) ? USP_ERR_INTERNAL_ERROR : USP_ERR_OK;
}

bool USP_DM_IsRegistered(char *path)
{
    int i;
    for (i=0; i < (int)(sizeof(core_paths)/sizeof(core_paths[0])); i++)
    {
        if (strcmp(core_paths[i], path) == 0)
        {
            return true;
        }
    }
    for (i=0; i < stub_num_regs; i++)
    {
        if (strcmp(stub_regs[i].path, path) == 0)
        {
            return true;
        }
    }
    return false;
}

int USP_DM_RefreshInstance(char *path)
{
    if (stub_num_refreshed >= STUB_MAX_REFRESH)
    {
        return USP_ERR_INTERNAL_ERROR;
    }
    snprintf(stub_refreshed[stub_num_refreshed++], MAX_DM_PATH, "%s", path);
    return USP_ERR_OK;
}

int USP_HOOK_DenyAddInstance(dm_req_t *req)
{
    return USP_ERR_OBJECT_NOT_CREATABLE;
}

int USP_HOOK_DenyDeleteInstance(dm_req_t *req)
{
    return USP_ERR_OBJECT_NOT_DELETABLE;
}

void USP_ERR_SetMessage(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ERR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

log_level_t USP_LOG_GetLogLevel()
{
    return kLogLevel_Warning;
}

void USP_LOG_Printf(log_level_t log_level, log_type_t log_type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void USP_LOG_Callstack(void)
{
}
