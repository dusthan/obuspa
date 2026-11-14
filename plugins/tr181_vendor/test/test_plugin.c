/*
 * test_plugin.c
 *
 * Host test of the TR-181 plug-in against the USP API stub (usp_stub.c).
 * Run with 'make test'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "usp_err_codes.h"
#include "vendor_api.h"
#include "usp_api.h"
#include "usp_stub.h"
#include "tr181_dm.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Builds a dm_req_t for a concrete path: schema path has instance numbers replaced by '{i}'
typedef struct
{
    dm_req_t req;
    dm_req_instances_t inst;
    char path[MAX_DM_PATH];
    char schema[MAX_DM_PATH];
} test_req_t;

static void MakeReq(test_req_t *t, const char *path)
{
    char tmp[MAX_DM_PATH];
    char *seg;
    char *save = NULL;

    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", path);
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (seg = strtok_r(tmp, ".", &save); seg != NULL; seg = strtok_r(NULL, ".", &save))
    {
        if (t->schema[0] != '\0')
        {
            strcat(t->schema, ".");
        }
        if (isdigit((unsigned char)seg[0]))
        {
            strcat(t->schema, "{i}");
            t->inst.instances[t->inst.order++] = atoi(seg);
        }
        else
        {
            strcat(t->schema, seg);
        }
    }

    t->req.path = t->path;
    t->req.schema_path = t->schema;
    t->req.inst = &t->inst;
}

static int Get(const char *path, char *buf, int len)
{
    test_req_t t;
    stub_reg_t *r;

    MakeReq(&t, path);
    r = STUB_Find(t.schema, kStub_Param);
    if ((r == NULL) || (r->get_cb == NULL))
    {
        snprintf(buf, len, "<not registered>");
        return USP_ERR_INVALID_PATH;
    }
    return r->get_cb(&t.req, buf, len);
}

static int Set(const char *path, char *value)
{
    test_req_t t;
    stub_reg_t *r;

    MakeReq(&t, path);
    r = STUB_Find(t.schema, kStub_Param);
    if ((r == NULL) || (r->set_cb == NULL))
    {
        return USP_ERR_PARAM_READ_ONLY;
    }
    if (r->type == DM_BOOL)
    {
        t.req.val_union.value_bool = ((strcmp(value, "true") == 0) || (strcmp(value, "1") == 0));
    }
    return r->set_cb(&t.req, value);
}

// obj_path is the new instance, e.g. "Device.Hosts.AccessControl.5"
static int Add(const char *obj_path)
{
    test_req_t t;
    stub_reg_t *r;

    MakeReq(&t, obj_path);
    r = STUB_Find(t.schema, kStub_Object);
    if (r == NULL)
    {
        return USP_ERR_INVALID_PATH;
    }
    if (r->validate_add_cb != NULL)
    {
        int err = r->validate_add_cb(&t.req);
        if (err != USP_ERR_OK)
        {
            return err;
        }
    }
    return (r->add_cb != NULL) ? r->add_cb(&t.req) : USP_ERR_OK;
}

static int Delete(const char *obj_path)
{
    test_req_t t;
    stub_reg_t *r;

    MakeReq(&t, obj_path);
    r = STUB_Find(t.schema, kStub_Object);
    if ((r == NULL) || (r->del_cb == NULL))
    {
        return USP_ERR_OBJECT_NOT_DELETABLE;
    }
    return r->del_cb(&t.req);
}

// Calls the refresh callback of top-level table 'schema' (e.g. "Device.WiFi.SSID.{i}")
static int Refresh(const char *schema)
{
    stub_reg_t *r;
    char path[MAX_DM_PATH];
    int expiry = -1;
    int err;

    r = STUB_Find(schema, kStub_Refresh);
    if (r == NULL)
    {
        return -1;
    }
    snprintf(path, sizeof(path), "%.*s", (int)(strlen(schema) - 3), schema);   // strip '{i}'
    stub_num_refreshed = 0;
    err = r->refresh_cb(0, path, &expiry);
    CHECK(err == USP_ERR_OK, "refresh %s returned %d", schema, err);
    CHECK(expiry > 0, "refresh %s expiry %d", schema, expiry);
    return stub_num_refreshed;
}

static bool Refreshed(const char *path)
{
    int i;
    for (i=0; i < stub_num_refreshed; i++)
    {
        if (strcmp(stub_refreshed[i], path) == 0)
        {
            return true;
        }
    }
    return false;
}

static void ExpectGet(const char *path, const char *expected)
{
    char buf[MAX_DM_VALUE_LEN];
    int err = Get(path, buf, sizeof(buf));
    CHECK((err == USP_ERR_OK) && (strcmp(buf, expected) == 0), "Get %s = '%s' (err %d), expected '%s'", path, buf, err, expected);
}

int main(void)
{
    int err;
    int n;
    int i;
    char buf[MAX_DM_VALUE_LEN];

    //--------------------------------------------------------------------------
    // Registration
    err = VENDOR_Init();
    CHECK(err == USP_ERR_OK, "VENDOR_Init returned %d", err);

    // 9 params are provided by OBUSPA core and must be skipped
    CHECK(STUB_Count(kStub_Param) == tr181_num_params - 9, "registered %d params, expected %d", STUB_Count(kStub_Param), tr181_num_params - 9);
    CHECK(STUB_Count(kStub_Object) == tr181_num_tables, "registered %d tables", STUB_Count(kStub_Object));
    CHECK(STUB_Count(kStub_NumEntries) == tr181_num_numentries, "registered %d numentries", STUB_Count(kStub_NumEntries));
    CHECK(STUB_Find("Device.DeviceInfo.SoftwareVersion", kStub_Param) == NULL, "core param SoftwareVersion was re-registered");
    CHECK(STUB_Find("Device.IP.Diagnostics.IPLayerCapacitySupported", kStub_Param) == NULL, "core param IPLayerCapacitySupported was re-registered");
    CHECK(STUB_Find("Device.Cellular.Interface.{i}.X_SURGE-CO-ID_NR.RSRP", kStub_Param) != NULL, "vendor extension param missing");

    // Writable tables allow add; read-only tables deny it
    CHECK(Add("Device.WiFi.SSID.9") == USP_ERR_OBJECT_NOT_CREATABLE, "Add to read-only table was allowed");
    CHECK(Delete("Device.WiFi.SSID.1") == USP_ERR_OBJECT_NOT_DELETABLE, "Delete from read-only table was allowed");

    //--------------------------------------------------------------------------
    // Instance refresh, including nested tables
    n = Refresh("Device.Cellular.Interface.{i}");
    CHECK(n == 8, "Cellular.Interface refreshed %d instances, expected 8", n);
    CHECK(Refreshed("Device.Cellular.Interface.1"), "missing Cellular.Interface.1");
    CHECK(Refreshed("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellMeasured.3"), "missing NRCellMeasured.3");
    CHECK(Refreshed("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.6"), "missing NRCellLocked.6");
    CHECK(!Refreshed("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.4"), "unexpected NRCellLocked.4");

    n = Refresh("Device.WiFi.AccessPoint.{i}");
    CHECK(n == 8, "WiFi.AccessPoint refreshed %d instances, expected 8", n);
    CHECK(Refreshed("Device.WiFi.AccessPoint.3.AssociatedDevice.1"), "missing AssociatedDevice.1");

    n = Refresh("Device.IP.Interface.{i}");
    CHECK(n == 4, "IP.Interface refreshed %d instances, expected 4 (2 interfaces + 2 IPv4Address)", n);

    n = Refresh("Device.WiFi.NeighboringWiFiDiagnostic.Result.{i}");
    CHECK(n == 26, "NeighboringWiFiDiagnostic.Result refreshed %d, expected 26", n);

    // Every top-level table must have a refresh callback
    for (i=0; i < tr181_num_tables; i++)
    {
        if (tr181_tables[i].parent < 0)
        {
            CHECK(STUB_Find(tr181_tables[i].schema, kStub_Refresh) != NULL, "no refresh for %s", tr181_tables[i].schema);
        }
    }

    //--------------------------------------------------------------------------
    // Get
    ExpectGet("Device.WiFi.SSID.1.SSID", "CPE-C201-TEST4-5G");
    ExpectGet("Device.CaptivePortal.Enable", "false");
    ExpectGet("Device.Cellular.RoamingEnabled", "true");
    ExpectGet("Device.Cellular.Interface.1.X_SURGE-CO-ID_NR.RSRP", "-71");
    ExpectGet("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.1.DownlinkNRARFCN", "504990");
    ExpectGet("Device.Cellular.Interface.1.USIM.IMSI", "460110000000000");
    ExpectGet("Device.DeviceInfo.VendorConfigFile.1.Date", "2025-01-01T12:41:46Z");
    ExpectGet("Device.WiFi.AccessPoint.2.AssociatedDevice.1.MACAddress", "7C:45:D0:33:55:02");
    ExpectGet("Device.WiFi.NeighboringWiFiDiagnostic.Result.7.SSID", "!Wewins");
    ExpectGet("Device.Ethernet.Interface.1.DuplexMode", "Full");
    ExpectGet("Device.X_SURGE-CO-ID_RootDataModelVersion", "surge-fwa.6.2");

    // Passwords / keys read back empty (TR-181)
    ExpectGet("Device.Users.User.1.Password", "");
    ExpectGet("Device.ManagementServer.ConnectionRequestPassword", "");
    ExpectGet("Device.WiFi.AccessPoint.1.Security.WEPKey", "");
    ExpectGet("Device.ManagementServer.ConnectionRequestUsername", "7C45D0-TEST_MOCK-C100023710001066");

    // Read-only parameter has no set hook
    CHECK(Set("Device.WiFi.SSID.1.BSSID", "00:00:00:00:00:00") == USP_ERR_PARAM_READ_ONLY, "set on read-only param allowed");

    //--------------------------------------------------------------------------
    // Set
    CHECK(Set("Device.WiFi.SSID.2.SSID", "NewSSID") == USP_ERR_OK, "set SSID failed");
    ExpectGet("Device.WiFi.SSID.2.SSID", "NewSSID");
    CHECK(Set("Device.WiFi.Radio.1.AutoChannelEnable", "0") == USP_ERR_OK, "set bool failed");
    ExpectGet("Device.WiFi.Radio.1.AutoChannelEnable", "false");

    //--------------------------------------------------------------------------
    // Add / Delete on writable top-level table
    CHECK(Add("Device.Hosts.AccessControl.5") == USP_ERR_OK, "add AccessControl.5 failed");
    n = Refresh("Device.Hosts.AccessControl.{i}");
    CHECK(n == 5, "AccessControl has %d instances after add, expected 5", n);
    ExpectGet("Device.Hosts.AccessControl.5.HostName", "");            // not yet set -> type default
    CHECK(Set("Device.Hosts.AccessControl.5.HostName", "new-pc") == USP_ERR_OK, "set on new instance failed");
    ExpectGet("Device.Hosts.AccessControl.5.HostName", "new-pc");

    CHECK(Delete("Device.Hosts.AccessControl.2") == USP_ERR_OK, "delete AccessControl.2 failed");
    n = Refresh("Device.Hosts.AccessControl.{i}");
    CHECK(n == 4, "AccessControl has %d instances after delete, expected 4", n);
    CHECK(!Refreshed("Device.Hosts.AccessControl.2"), "AccessControl.2 still present");
    ExpectGet("Device.Hosts.AccessControl.2.HostName", "");            // value removed with the instance

    //--------------------------------------------------------------------------
    // Add / Delete on writable nested table
    CHECK(Add("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.7") == USP_ERR_OK, "add NRCellLocked.7 failed");
    n = Refresh("Device.Cellular.Interface.{i}");
    CHECK(n == 9, "Cellular.Interface refreshed %d after add, expected 9", n);
    CHECK(Refreshed("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.7"), "missing NRCellLocked.7");
    ExpectGet("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.7.DownlinkNRARFCN", "0");
    CHECK(Delete("Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked.1") == USP_ERR_OK, "delete NRCellLocked.1 failed");
    n = Refresh("Device.Cellular.Interface.{i}");
    CHECK(n == 8, "Cellular.Interface refreshed %d after delete, expected 8", n);
    CHECK(Refreshed("Device.Cellular.Interface.1"), "parent Interface.1 deleted along with child");

    //--------------------------------------------------------------------------
    // Every seed value can be read back through a registered parameter (or belongs to a core param)
    for (i=0; i < tr181_num_seed_values; i++)
    {
        const char *p = tr181_seed_values[i].path;
        if (strstr(p, "AccessControl.2.") || strstr(p, "NRCellLocked.1.") ||
            strcmp(p, "Device.WiFi.SSID.2.SSID") == 0 || strcmp(p, "Device.WiFi.Radio.1.AutoChannelEnable") == 0)
        {
            continue;   // modified above
        }
        err = Get(p, buf, sizeof(buf));
        if (err == USP_ERR_INVALID_PATH)
        {
            test_req_t t;
            MakeReq(&t, p);
            CHECK(USP_DM_IsRegistered(t.schema), "seed value %s has no registered parameter", p);
            continue;
        }
        CHECK(err == USP_ERR_OK, "Get %s failed", p);
    }

    VENDOR_Stop();

    if (failures == 0)
    {
        printf("All tests passed (%d tables, %d params, %d NumberOfEntries, %d seed values)\n",
               tr181_num_tables, tr181_num_params, tr181_num_numentries, tr181_num_seed_values);
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
