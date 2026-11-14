/*
 * tr181_backend_mock.c
 *
 * In-memory implementation of tr181_backend.h, seeded from the values in data/tr181_params.csv.
 * Writes are kept in RAM only (lost when obuspa restarts), unless the environment variable
 * TR181_MOCK_DB names a sqlite file, in which case every Set/Add/Delete is also persisted there
 * and replayed over the seeds at the next TR181_BE_Init().
 *
 * Per-CPE identity (set by the simulator supervisor) is applied to the seeds:
 *   USP_BOARD_SERIAL -> DeviceInfo.SerialNumber / DeviceID.SerialNumber
 *   USP_BOARD_OUI    -> DeviceInfo.ManufacturerOUI
 *   USP_SIM_MAC      -> every *MACAddress seed: first 5 bytes of USP_SIM_MAC + the seed's last byte
 *
 * Replace this file with a platform implementation to talk to the real device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <sqlite3.h>

#include "vendor_api.h"
#include "usp_api.h"
#include "tr181_backend.h"
#include "tr181_dm.h"

//------------------------------------------------------------------------------
// Parameter value store: chained hash table keyed by concrete path
#define NUM_BUCKETS 1024

typedef struct kv_entry
{
    struct kv_entry *next;
    char *path;
    char *value;
} kv_entry_t;

static kv_entry_t *buckets[NUM_BUCKETS];

//------------------------------------------------------------------------------
// Object instance store: concrete instance paths, e.g. "Device.WiFi.SSID.1"
static char **instances = NULL;
static int num_instances = 0;
static int max_instances = 0;

//------------------------------------------------------------------------------
// Optional sqlite persistence (only when TR181_MOCK_DB is set)
static sqlite3 *db = NULL;
static sqlite3_stmt *stmt_set = NULL;
static sqlite3_stmt *stmt_inst = NULL;
static sqlite3_stmt *stmt_del_kv = NULL;
static sqlite3_stmt *stmt_del_inst = NULL;
static sqlite3_stmt *stmt_get_inst = NULL;

//------------------------------------------------------------------------------
// Forward declarations
static void ApplyIdentity(void);
static bool ParseMac(const char *s, unsigned char mac[6]);
static int OpenDb(const char *file);
static void CloseDb(void);
static int ReplayDb(void);
static int DbExec(const char *sql);
static int DbStep(sqlite3_stmt *st, const char *what);
static int DbSetValue(const char *path, const char *value);
static int DbAddInstance(const char *obj_path);
static int DbDeleteInstance(const char *obj_path);
static int RemoveInstance(const char *obj_path);
static bool EndsWith(const char *s, const char *suffix);
static unsigned Hash(const char *s);
static kv_entry_t *FindEntry(const char *path);
static int StoreValue(const char *path, const char *value);
static int AddInstancePath(const char *obj_path);
static bool StartsWith(const char *s, const char *prefix);
static int CompareInt(const void *a, const void *b);

int TR181_BE_Init(void)
{
    int i;
    const char *db_file;

    for (i=0; i < tr181_num_seed_instances; i++)
    {
        if (AddInstancePath(tr181_seed_instances[i]) != TR181_BE_OK)
        {
            return TR181_BE_ERROR;
        }
    }

    for (i=0; i < tr181_num_seed_values; i++)
    {
        if (StoreValue(tr181_seed_values[i].path, tr181_seed_values[i].value) != TR181_BE_OK)
        {
            return TR181_BE_ERROR;
        }
    }

    ApplyIdentity();

    db_file = getenv("TR181_MOCK_DB");
    if ((db_file != NULL) && (db_file[0] != '\0'))
    {
        if ((OpenDb(db_file) != TR181_BE_OK) || (ReplayDb() != TR181_BE_OK))
        {
            CloseDb();
            return TR181_BE_ERROR;
        }
    }

    return TR181_BE_OK;
}

void TR181_BE_Stop(void)
{
    int i;
    kv_entry_t *e;
    kv_entry_t *next;

    CloseDb();

    for (i=0; i < NUM_BUCKETS; i++)
    {
        for (e = buckets[i]; e != NULL; e = next)
        {
            next = e->next;
            free(e->path);
            free(e->value);
            free(e);
        }
        buckets[i] = NULL;
    }

    for (i=0; i < num_instances; i++)
    {
        free(instances[i]);
    }
    free(instances);
    instances = NULL;
    num_instances = 0;
    max_instances = 0;
}

int TR181_BE_Get(const char *path, char *buf, int len)
{
    kv_entry_t *e;

    e = FindEntry(path);
    if (e == NULL)
    {
        return TR181_BE_NOT_FOUND;
    }

    snprintf(buf, len, "%s", e->value);
    return TR181_BE_OK;
}

int TR181_BE_Set(const char *path, const char *value)
{
    // Persist first: memory only changes if the db write succeeded
    if (DbSetValue(path, value) != TR181_BE_OK)
    {
        return TR181_BE_ERROR;
    }
    return (StoreValue(path, value) == TR181_BE_OK) ? TR181_BE_OK : TR181_BE_ERROR;
}

int TR181_BE_GetInstances(const char *table_prefix, int *insts, int max, int *num)
{
    int i;
    int n = 0;
    size_t prefix_len;
    const char *p;
    const char *q;

    prefix_len = strlen(table_prefix);
    for (i=0; i < num_instances; i++)
    {
        if (StartsWith(instances[i], table_prefix) == false)
        {
            continue;
        }

        // Remainder must be just an instance number (not a deeper object)
        p = &instances[i][prefix_len];
        for (q = p; isdigit((unsigned char)*q); q++)
        {
            /* skip digits */
        }
        if ((q == p) || (*q != '\0'))
        {
            continue;
        }

        if (n >= max)
        {
            break;
        }
        insts[n++] = atoi(p);
    }

    qsort(insts, n, sizeof(int), CompareInt);
    *num = n;
    return TR181_BE_OK;
}

int TR181_BE_AddInstance(const char *obj_path)
{
    if (DbAddInstance(obj_path) != TR181_BE_OK)
    {
        return TR181_BE_ERROR;
    }
    return (AddInstancePath(obj_path) == TR181_BE_OK) ? TR181_BE_OK : TR181_BE_ERROR;
}

int TR181_BE_DeleteInstance(const char *obj_path)
{
    if (strlen(obj_path) + 2 > 512)     // RemoveInstance() child prefix buffer
    {
        return TR181_BE_ERROR;
    }
    if (DbDeleteInstance(obj_path) != TR181_BE_OK)
    {
        return TR181_BE_ERROR;
    }
    return RemoveInstance(obj_path);
}

//------------------------------------------------------------------------------
// Removes an instance, its child instances and all parameter values below it (memory only)
static int RemoveInstance(const char *obj_path)
{
    int i;
    int j;
    char child_prefix[512];
    kv_entry_t **pe;
    kv_entry_t *e;

    if (snprintf(child_prefix, sizeof(child_prefix), "%s.", obj_path) >= (int)sizeof(child_prefix))
    {
        return TR181_BE_ERROR;
    }

    // Remove the instance and all child instances
    j = 0;
    for (i=0; i < num_instances; i++)
    {
        if ((strcmp(instances[i], obj_path) == 0) || StartsWith(instances[i], child_prefix))
        {
            free(instances[i]);
            continue;
        }
        instances[j++] = instances[i];
    }
    num_instances = j;

    // Remove all parameter values below the instance
    for (i=0; i < NUM_BUCKETS; i++)
    {
        pe = &buckets[i];
        while (*pe != NULL)
        {
            e = *pe;
            if (StartsWith(e->path, child_prefix))
            {
                *pe = e->next;
                free(e->path);
                free(e->value);
                free(e);
            }
            else
            {
                pe = &e->next;
            }
        }
    }

    return TR181_BE_OK;
}

//------------------------------------------------------------------------------
// Identity rewrite of the seeds

static bool ParseMac(const char *s, unsigned char mac[6])
{
    int i;
    unsigned v;

    if (strlen(s) != 17)
    {
        return false;
    }
    for (i=0; i < 6; i++)
    {
        if (!isxdigit((unsigned char)s[i*3]) || !isxdigit((unsigned char)s[i*3+1]))
        {
            return false;
        }
        if ((i < 5) && (s[i*3+2] != ':'))
        {
            return false;
        }
        if (sscanf(&s[i*3], "%2x", &v) != 1)
        {
            return false;
        }
        mac[i] = (unsigned char)v;
    }
    return true;
}

static void ApplyIdentity(void)
{
    const char *serial = getenv("USP_BOARD_SERIAL");
    const char *oui = getenv("USP_BOARD_OUI");
    const char *sim_mac = getenv("USP_SIM_MAC");
    unsigned char base[6];
    unsigned char orig[6];
    bool have_mac = false;
    char buf[32];
    int i;

    if ((serial != NULL) && (serial[0] != '\0'))
    {
        if (FindEntry("Device.DeviceInfo.SerialNumber") != NULL)
        {
            StoreValue("Device.DeviceInfo.SerialNumber", serial);
        }
        if (FindEntry("DeviceID.SerialNumber") != NULL)
        {
            StoreValue("DeviceID.SerialNumber", serial);
        }
    }
    if ((oui != NULL) && (oui[0] != '\0') && (FindEntry("Device.DeviceInfo.ManufacturerOUI") != NULL))
    {
        StoreValue("Device.DeviceInfo.ManufacturerOUI", oui);
    }

    if ((sim_mac != NULL) && ParseMac(sim_mac, base))
    {
        have_mac = true;
    }
    else if ((sim_mac != NULL) && (sim_mac[0] != '\0'))
    {
        USP_LOG_Warning("%s: ignoring malformed USP_SIM_MAC '%s'", __func__, sim_mac);
    }
    if (!have_mac)
    {
        return;
    }

    for (i=0; i < tr181_num_seed_values; i++)
    {
        if (EndsWith(tr181_seed_values[i].path, "MACAddress") && ParseMac(tr181_seed_values[i].value, orig))
        {
            snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     base[0], base[1], base[2], base[3], base[4], orig[5]);
            StoreValue(tr181_seed_values[i].path, buf);
        }
    }
}

static bool EndsWith(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);
    return (ls >= lx) && (strcmp(&s[ls - lx], suffix) == 0);
}

//------------------------------------------------------------------------------
// sqlite persistence

static int DbExec(const char *sql)
{
    char *msg = NULL;

    if (sqlite3_exec(db, sql, NULL, NULL, &msg) != SQLITE_OK)
    {
        USP_LOG_Error("%s: sqlite error executing '%s': %s", __func__, sql, (msg != NULL) ? msg : "?");
        sqlite3_free(msg);
        return TR181_BE_ERROR;
    }
    return TR181_BE_OK;
}

static int Prepare(sqlite3_stmt **st, const char *sql)
{
    if (sqlite3_prepare_v2(db, sql, -1, st, NULL) != SQLITE_OK)
    {
        USP_LOG_Error("%s: sqlite prepare failed (%s): %s", __func__, sql, sqlite3_errmsg(db));
        return TR181_BE_ERROR;
    }
    return TR181_BE_OK;
}

static int OpenDb(const char *file)
{
    if (sqlite3_open(file, &db) != SQLITE_OK)
    {
        USP_LOG_Error("%s: cannot open TR181_MOCK_DB '%s': %s", __func__, file, (db != NULL) ? sqlite3_errmsg(db) : "out of memory");
        return TR181_BE_ERROR;
    }
    sqlite3_busy_timeout(db, 5000);

    if ((DbExec("PRAGMA journal_mode=WAL") != TR181_BE_OK) ||
        (DbExec("PRAGMA synchronous=NORMAL") != TR181_BE_OK) ||
        (DbExec("CREATE TABLE IF NOT EXISTS kv(path TEXT PRIMARY KEY, value TEXT)") != TR181_BE_OK) ||
        (DbExec("CREATE TABLE IF NOT EXISTS inst(path TEXT PRIMARY KEY, present INTEGER)") != TR181_BE_OK))
    {
        return TR181_BE_ERROR;
    }

    if ((Prepare(&stmt_set, "INSERT OR REPLACE INTO kv(path, value) VALUES(?1, ?2)") != TR181_BE_OK) ||
        (Prepare(&stmt_inst, "INSERT OR REPLACE INTO inst(path, present) VALUES(?1, ?2)") != TR181_BE_OK) ||
        (Prepare(&stmt_del_kv, "DELETE FROM kv WHERE path = ?1 OR path LIKE ?2 ESCAPE '\\'") != TR181_BE_OK) ||
        (Prepare(&stmt_del_inst, "DELETE FROM inst WHERE path LIKE ?1 ESCAPE '\\'") != TR181_BE_OK) ||
        (Prepare(&stmt_get_inst, "SELECT present FROM inst WHERE path = ?1") != TR181_BE_OK))
    {
        return TR181_BE_ERROR;
    }
    return TR181_BE_OK;
}

static void CloseDb(void)
{
    sqlite3_finalize(stmt_set);
    sqlite3_finalize(stmt_inst);
    sqlite3_finalize(stmt_del_kv);
    sqlite3_finalize(stmt_del_inst);
    sqlite3_finalize(stmt_get_inst);
    stmt_set = stmt_inst = stmt_del_kv = stmt_del_inst = stmt_get_inst = NULL;
    if (db != NULL)
    {
        sqlite3_close(db);
        db = NULL;
    }
}

static int ReplayDb(void)
{
    sqlite3_stmt *st = NULL;
    int rc;
    int err = TR181_BE_OK;

    // Instances first, parents before children (a delete also removes the values and sub-instances below it)
    if (Prepare(&st, "SELECT path, present FROM inst "
                "ORDER BY length(path) - length(replace(path, '.', '')), rowid") != TR181_BE_OK)
    {
        return TR181_BE_ERROR;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
    {
        const char *path = (const char *)sqlite3_column_text(st, 0);
        int present = sqlite3_column_int(st, 1);

        if (path == NULL)
        {
            continue;
        }
        // present: 0 = deleted, 1 = added, 2 = re-added after delete (clears seed values below it first)
        if ((present == 0) || (present == 2))
        {
            err = RemoveInstance(path);
        }
        if ((err == TR181_BE_OK) && (present != 0))
        {
            err = AddInstancePath(path);
        }
        if (err != TR181_BE_OK)
        {
            break;
        }
    }
    if ((err == TR181_BE_OK) && (rc != SQLITE_DONE))
    {
        USP_LOG_Error("%s: sqlite error reading inst table: %s", __func__, sqlite3_errmsg(db));
        err = TR181_BE_ERROR;
    }
    sqlite3_finalize(st);
    if (err != TR181_BE_OK)
    {
        return TR181_BE_ERROR;
    }

    // Then the value overrides
    st = NULL;
    if (Prepare(&st, "SELECT path, value FROM kv") != TR181_BE_OK)
    {
        return TR181_BE_ERROR;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
    {
        const char *path = (const char *)sqlite3_column_text(st, 0);
        const char *value = (const char *)sqlite3_column_text(st, 1);

        if (path == NULL)
        {
            continue;
        }
        err = StoreValue(path, (value != NULL) ? value : "");
        if (err != TR181_BE_OK)
        {
            break;
        }
    }
    if ((err == TR181_BE_OK) && (rc != SQLITE_DONE))
    {
        USP_LOG_Error("%s: sqlite error reading kv table: %s", __func__, sqlite3_errmsg(db));
        err = TR181_BE_ERROR;
    }
    sqlite3_finalize(st);
    return err;
}

static int DbStep(sqlite3_stmt *st, const char *what)
{
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (rc != SQLITE_DONE)
    {
        USP_LOG_Error("%s: sqlite %s failed: %s", __func__, what, sqlite3_errmsg(db));
        return TR181_BE_ERROR;
    }
    return TR181_BE_OK;
}

static int DbSetValue(const char *path, const char *value)
{
    if (db == NULL)
    {
        return TR181_BE_OK;
    }
    sqlite3_bind_text(stmt_set, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_set, 2, value, -1, SQLITE_STATIC);
    return DbStep(stmt_set, "kv write");
}

static int DbAddInstance(const char *obj_path)
{
    int present = 1;
    int rc;

    if (db == NULL)
    {
        return TR181_BE_OK;
    }

    // A tombstone means the instance existed before (e.g. a seed) and was deleted: mark it re-added
    sqlite3_bind_text(stmt_get_inst, 1, obj_path, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt_get_inst);
    if (rc == SQLITE_ROW)
    {
        present = (sqlite3_column_int(stmt_get_inst, 0) == 1) ? 1 : 2;
    }
    sqlite3_reset(stmt_get_inst);
    sqlite3_clear_bindings(stmt_get_inst);
    if ((rc != SQLITE_ROW) && (rc != SQLITE_DONE))
    {
        USP_LOG_Error("%s: sqlite inst read failed: %s", __func__, sqlite3_errmsg(db));
        return TR181_BE_ERROR;
    }

    sqlite3_bind_text(stmt_inst, 1, obj_path, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_inst, 2, present);
    return DbStep(stmt_inst, "inst write");
}

// Deleting records a tombstone and drops all persisted rows (instances and values) below the path,
// atomically, so the database never disagrees with what a restart would rebuild
static int DbDeleteInstance(const char *obj_path)
{
    char *pattern;
    size_t i;
    size_t n = 0;
    int err;

    if (db == NULL)
    {
        return TR181_BE_OK;
    }

    // LIKE pattern for "<obj_path>.%" with wildcards in the path escaped
    pattern = malloc(strlen(obj_path) * 2 + 3);
    if (pattern == NULL)
    {
        return TR181_BE_ERROR;
    }
    for (i=0; obj_path[i] != '\0'; i++)
    {
        if ((obj_path[i] == '%') || (obj_path[i] == '_') || (obj_path[i] == '\\'))
        {
            pattern[n++] = '\\';
        }
        pattern[n++] = obj_path[i];
    }
    pattern[n++] = '.';
    pattern[n++] = '%';
    pattern[n] = '\0';

    err = DbExec("BEGIN");
    if (err == TR181_BE_OK)
    {
        sqlite3_bind_text(stmt_del_inst, 1, pattern, -1, SQLITE_STATIC);
        err = DbStep(stmt_del_inst, "inst delete");
    }
    if (err == TR181_BE_OK)
    {
        sqlite3_bind_text(stmt_del_kv, 1, obj_path, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_del_kv, 2, pattern, -1, SQLITE_STATIC);
        err = DbStep(stmt_del_kv, "kv delete");
    }
    if (err == TR181_BE_OK)
    {
        sqlite3_bind_text(stmt_inst, 1, obj_path, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt_inst, 2, 0);
        err = DbStep(stmt_inst, "inst tombstone");
    }
    if (err == TR181_BE_OK)
    {
        err = DbExec("COMMIT");
    }
    if (err != TR181_BE_OK)
    {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);     // no-op if no transaction is open
    }
    free(pattern);
    return err;
}

//------------------------------------------------------------------------------

static unsigned Hash(const char *s)
{
    unsigned h = 5381;
    while (*s != '\0')
    {
        h = (h * 33) ^ (unsigned char)*s++;
    }
    return h % NUM_BUCKETS;
}

static kv_entry_t *FindEntry(const char *path)
{
    kv_entry_t *e;

    for (e = buckets[Hash(path)]; e != NULL; e = e->next)
    {
        if (strcmp(e->path, path) == 0)
        {
            return e;
        }
    }
    return NULL;
}

static int StoreValue(const char *path, const char *value)
{
    kv_entry_t *e;
    char *new_value;
    unsigned h;

    new_value = strdup(value);
    if (new_value == NULL)
    {
        return TR181_BE_ERROR;
    }

    e = FindEntry(path);
    if (e != NULL)
    {
        free(e->value);
        e->value = new_value;
        return TR181_BE_OK;
    }

    e = malloc(sizeof(kv_entry_t));
    if (e == NULL)
    {
        free(new_value);
        return TR181_BE_ERROR;
    }
    e->path = strdup(path);
    if (e->path == NULL)
    {
        free(new_value);
        free(e);
        return TR181_BE_ERROR;
    }
    e->value = new_value;

    h = Hash(path);
    e->next = buckets[h];
    buckets[h] = e;
    return TR181_BE_OK;
}

static int AddInstancePath(const char *obj_path)
{
    int i;
    char **new_vec;

    for (i=0; i < num_instances; i++)
    {
        if (strcmp(instances[i], obj_path) == 0)
        {
            return TR181_BE_OK;
        }
    }

    if (num_instances == max_instances)
    {
        int new_max = (max_instances == 0) ? 64 : max_instances * 2;
        new_vec = realloc(instances, new_max * sizeof(char *));
        if (new_vec == NULL)
        {
            return TR181_BE_ERROR;
        }
        instances = new_vec;
        max_instances = new_max;
    }

    instances[num_instances] = strdup(obj_path);
    if (instances[num_instances] == NULL)
    {
        return TR181_BE_ERROR;
    }
    num_instances++;
    return TR181_BE_OK;
}

static bool StartsWith(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int CompareInt(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}
