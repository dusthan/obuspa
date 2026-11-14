/*
 * tr181_backend.h
 *
 * Interface between the TR-181 plug-in (tr181_plugin.c) and the platform.
 *
 * The plug-in registers the data model with OBUSPA and forwards every Get/Set/Add/Delete
 * and instance enumeration to the functions below. All paths are concrete (instantiated)
 * paths without a trailing '.', e.g. "Device.WiFi.SSID.1.SSID" or "Device.Hosts.AccessControl.3".
 *
 * tr181_backend_mock.c is an in-memory implementation seeded from the CSV values.
 * To integrate with real hardware, provide another implementation of these functions
 * (e.g. using UCI / ubus / AT commands) and select it with 'make BACKEND=xxx'.
 *
 * All functions are called from the OBUSPA data model thread.
 */
#ifndef TR181_BACKEND_H
#define TR181_BACKEND_H

#define TR181_BE_OK          0
#define TR181_BE_NOT_FOUND   1      // Parameter has no value: plug-in returns the type's default value
#define TR181_BE_ERROR      -1      // Any other failure: reported to the controller as an internal error

// Called from VENDOR_Init() before the data model is registered
int  TR181_BE_Init(void);

// Called from VENDOR_Stop()
void TR181_BE_Stop(void);

// Gets the value of a parameter as a string
int  TR181_BE_Get(const char *path, char *buf, int len);

// Sets the value of a parameter. Value has already been validated against the parameter's type
int  TR181_BE_Set(const char *path, const char *value);

// Returns the instance numbers of a table, in ascending order.
// table_prefix is the concrete path of the table with a trailing '.', e.g. "Device.Cellular.Interface.1.X_SURGE-CO-ID_NRCellLocked."
int  TR181_BE_GetInstances(const char *table_prefix, int *instances, int max_instances, int *num_instances);

// Creates/deletes an object instance (only called for writable tables). obj_path e.g. "Device.Hosts.AccessControl.5"
int  TR181_BE_AddInstance(const char *obj_path);
int  TR181_BE_DeleteInstance(const char *obj_path);

#endif
