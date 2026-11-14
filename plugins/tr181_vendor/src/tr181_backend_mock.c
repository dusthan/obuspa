/*
 * tr181_backend_mock.c
 *
 * In-memory implementation of tr181_backend.h, seeded from the values in data/tr181_params.csv.
 * Writes are kept in RAM only (lost when obuspa restarts).
 *
 * Replace this file with a platform implementation to talk to the real device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
// Forward declarations
static unsigned Hash(const char *s);
static kv_entry_t *FindEntry(const char *path);
static int StoreValue(const char *path, const char *value);
static int AddInstancePath(const char *obj_path);
static bool StartsWith(const char *s, const char *prefix);
static int CompareInt(const void *a, const void *b);

int TR181_BE_Init(void)
{
    int i;

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

    return TR181_BE_OK;
}

void TR181_BE_Stop(void)
{
    int i;
    kv_entry_t *e;
    kv_entry_t *next;

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
    return StoreValue(path, value);
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
    return AddInstancePath(obj_path);
}

int TR181_BE_DeleteInstance(const char *obj_path)
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
