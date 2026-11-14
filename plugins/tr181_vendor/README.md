# TR-181 Vendor Plug-in (`libtr181_vendor.so`)

OBUSPA vendor plug-in that registers the TR-181 data model listed in `data/tr181_params.csv`
(Device.Cellular / WiFi / IP / Hosts / DNS / DHCPv4 / DeviceInfo / ManagementServer ... including
the `X_SURGE-CO-ID_` vendor extensions). It is loaded at runtime via OBUSPA's existing plug-in mechanism (`-x`),
so the core code needs no changes.

## Layout

```
data/tr181_params.csv        Source data model (GenieACS-style export)
tools/gen_tr181_dm.py        CSV -> src/tr181_dm_gen.c generator
src/tr181_dm_gen.c           Generated: tables / parameters / NumberOfEntries / mock seed values (do not edit by hand)
src/tr181_plugin.c           VENDOR_Init/Start/Stop: data model registration + generic Get/Set/Add/Delete/Refresh callbacks
src/tr181_backend.h          Backend interface (the integration point with real hardware)
src/tr181_backend_mock.c     In-memory mock backend, seeded with the CSV values
test/                        Host test harness (stubs the USP API)
```

## Build (Linux)

Dependencies: `gcc`, `make`, OpenSSL headers (Debian/Ubuntu `libssl-dev`, RHEL/Fedora `openssl-devel`, Alpine `openssl-dev`).
Python 3 is only needed by `make gen`. The obuspa source tree must be at `../..`, otherwise set `OBUSPA_SRC=`.

```sh
cd plugins/tr181_vendor
make env            # check toolchain / OpenSSL headers / obuspa headers first
make                # -> libtr181_vendor.so
make check          # run the host tests + check exported symbols (only VENDOR_* allowed)
sudo make install   # -> /usr/lib/obuspa/libtr181_vendor.so (PREFIX / LIBDIR / DESTDIR can be overridden)
```

Cross-compile:

```sh
make CROSS_COMPILE=aarch64-linux-gnu- SYSROOT=/opt/sysroot
make CC=arm-openwrt-linux-muslgnueabi-gcc OPENSSL_CFLAGS=-I$STAGING_DIR/usr/include
make install-strip DESTDIR=$ROOTFS
```

| Variable | Default | Meaning |
|---|---|---|
| `OBUSPA_SRC` | `../..` | obuspa source tree (for `src/include`, `src/vendor`) |
| `CROSS_COMPILE` | empty | toolchain prefix; CC/NM/STRIP derived from it |
| `SYSROOT` | empty | passed as `--sysroot` |
| `OPENSSL_CFLAGS` | `pkg-config --cflags openssl` | OpenSSL include path |
| `PLUGIN_DEFS` | `-DENABLE_UDS` | **must match the feature defines obuspa was built with** (affects `vendor_defs.h` struct layout) |
| `BACKEND` | `mock` | compiles `src/tr181_backend_$(BACKEND).c` |
| `CFLAGS` / `LDFLAGS` | `-O2 -g` / empty | user flags; required flags such as `-fPIC` are kept separately and are not overridden |
| `V=1` / `WERROR=1` | | show full command lines / treat warnings as errors |

- The `.so` exports only `VENDOR_Init/Start/Stop` (`-fvisibility=hidden` + `exports.map`), so its internal symbols cannot clash with obuspa's.
- `USP_*` symbols are left undefined on purpose and resolved from obuspa when it is loaded, so do not add `-Wl,-z,defs` / `--no-undefined`.
- `make test` always uses `HOSTCC` (default `cc`), so it still runs on the build machine when cross-compiling.

## Run

```sh
obuspa -p -v 4 -r factory_reset.txt -i eth0 -x /usr/lib/obuspa/libtr181_vendor.so
```

The obuspa executable must export its symbols: the autotools build already passes `-rdynamic`;
for the CMake build, `ENABLE_EXPORTS ON` was added to the top-level `CMakeLists.txt`.

## Updating the data model

After editing or replacing `data/tr181_params.csv`:

```sh
make gen && make && make test
```

The generator prints a warning for every data problem it finds in the CSV. Fix-ups for known issues
are in `IMPLICIT_TABLES` / `TYPE_OVERRIDES` / `VALUE_OVERRIDES` at the top of `tools/gen_tr181_dm.py`.

## Integrating with real hardware

All parameter reads and writes go through the 7 functions in `src/tr181_backend.h`. Write
`src/tr181_backend_<name>.c` (e.g. calling UCI / ubus / AT commands) and build it with `make BACKEND=<name>`.
The plug-in itself needs no changes. Notes:

- `TR181_BE_GetInstances()` decides which instances are visible (e.g. the number of rows in Hosts.Host or NeighboringWiFiDiagnostic.Result).
  OBUSPA caches the result for `TR181_REFRESH_PERIOD` (default 5 s).
- Returning `TR181_BE_NOT_FOUND` from `TR181_BE_Get()` makes the plug-in report the type's default value (`""`, `0`, `false`, `0001-01-01T00:00:00Z`).
- Setting `DiagnosticsState=Requested` on the diagnostics objects (IPPing / Download / Upload / NeighboringWiFi)
  only stores the value; actually running the diagnostic is up to the backend.

## Behaviour notes

| Item | Handling |
|---|---|
| Paths already provided by OBUSPA core (DeviceInfo.SoftwareVersion/ProductClass/Manufacturer/ModelName/HardwareVersion/UpTime/ManufacturerOUI/SerialNumber, IP.Diagnostics.IPLayerCapacitySupported) | Detected at runtime with `USP_DM_IsRegistered()` and skipped; core implementation is kept |
| `DeviceID.*` / `Events.*` / `Tags.*` / `VirtualParameters.*` | ACS (GenieACS) internal fields, not TR-181; ignored |
| `*NumberOfEntries` | Registered with `USP_REGISTER_Param_NumEntries`, value computed automatically by core |
| Writable tables | Only `Device.Hosts.AccessControl.{i}` and `...X_SURGE-CO-ID_NRCellLocked.{i}` (per the CSV); all other tables are read-only |
| Password / Passphrase / WEPKey parameters | Read back as an empty string per TR-181; define `TR181_EXPOSE_SECRETS` at build time to disable |
| Mock backend | Writes are kept in memory only and are lost when obuspa restarts |

### CSV data problems fixed automatically

- `Device.WiFi.AccessPoint.N.AssociatedDevice.*` has no instance number; changed to `AssociatedDevice.1.*` (a table in TR-181)
- `PerConnectionResult` rows are marked `Object=FALSE` but have child nodes; treated as a table
- `Ethernet.Interface.DuplexMode` was typed unsignedInt -> string; `WiFi.Radio.Name` was typed boolean -> string; `DeviceInfo.UpTime` string -> unsignedInt
- `USIM.IMSI` value `4.6E+14` (corrupted by Excel) replaced with placeholder `460110000000000`
- dateTime values like `2025-01-01 12:41:46Z` normalised to `2025-01-01T12:41:46Z`
- `NeighboringWiFiDiagnostic.Result.7` appears twice; the later one wins
- `IP.Interface.N.IPv6Address` has no instances; registered as an empty table
