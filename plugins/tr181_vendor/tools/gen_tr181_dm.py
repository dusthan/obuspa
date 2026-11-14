#!/usr/bin/env python3
"""
gen_tr181_dm.py

Converts a GenieACS-style parameter dump (CSV) into the C tables used by the
TR-181 vendor plug-in (tr181_dm_gen.c).

CSV columns: Parameter,Object,Writable,Value,Value type[,Notification,ValueChange]

Usage:
    python3 tools/gen_tr181_dm.py data/tr181_params.csv src/tr181_dm_gen.c

What it does:
  * keeps only 'Device.' paths (DeviceID./Events./Tags./VirtualParameters. are ACS-internal)
  * converts instance numbers to '{i}' to derive the supported data model (schema)
  * detects multi-instance tables (and their writability) and NumberOfEntries params
  * normalises values (booleans -> true/false, 'YYYY-MM-DD hh:mm:ssZ' -> ISO-8601)
  * applies the fix-ups listed below for known errors in the source CSV
  * emits the concrete paths/values found in the CSV as the mock backend's seed data
"""

import csv
import re
import sys
from collections import OrderedDict

# ---------------------------------------------------------------------------
# Fix-ups for known issues in the source CSV
# ---------------------------------------------------------------------------

# Tables which the CSV lists without an instance number (TR-181 defines them as tables).
# Any concrete path below these (that lacks an instance number) gets instance '1' inserted.
IMPLICIT_TABLES = [
    "Device.WiFi.AccessPoint.{i}.AssociatedDevice",
]

# Schema path -> TR-181 type, where the CSV type is wrong
TYPE_OVERRIDES = {
    "Device.Ethernet.Interface.{i}.DuplexMode": "string",   # CSV says unsignedInt, value 'Full'
    "Device.WiFi.Radio.{i}.Name":              "string",   # CSV says boolean, value ''
    "Device.DeviceInfo.UpTime":                "unsignedInt",  # CSV says string
}

# Concrete path -> value, where the CSV value is corrupt
VALUE_OVERRIDES = {
    # Excel turned the IMSI into scientific notation ('4.6E+14'). Placeholder value.
    "Device.Cellular.Interface.1.USIM.IMSI": "460110000000000",
}

TYPE_MAP = {
    "string": "DM_STRING",
    "boolean": "DM_BOOL",
    "unsignedInt": "DM_UINT",
    "int": "DM_INT",
    "unsignedLong": "DM_ULONG",
    "long": "DM_LONG",
    "dateTime": "DM_DATETIME",
    "base64": "DM_BASE64",
    "hexBinary": "DM_HEXBIN",
}

warnings = []


def warn(msg):
    warnings.append(msg)


def is_true(s):
    return s.strip().lower() == "true"


def split_concrete(path):
    """Split a concrete path into segments, inserting instance '1' below implicit tables."""
    segs = path.split(".")
    result = []
    for i, seg in enumerate(segs):
        result.append(seg)
        if to_schema(result) in IMPLICIT_TABLES and i + 1 < len(segs) and not segs[i + 1].isdigit():
            result.append("1")
    return result


def to_schema(segs):
    return ".".join("{i}" if s.isdigit() else s for s in segs)


def normalise_value(value, ctype, path):
    v = value.strip()
    if ctype == "boolean":
        lv = v.lower()
        if lv in ("true", "1"):
            return "true"
        if lv in ("false", "0", ""):
            return "false"
        warn(f"{path}: boolean with value '{value}', using 'false'")
        return "false"
    if ctype in ("unsignedInt", "unsignedLong"):
        if v == "":
            return "0"
        if not v.isdigit():
            warn(f"{path}: {ctype} with value '{value}', using '0'")
            return "0"
    if ctype in ("int", "long"):
        if v == "":
            return "0"
        if not re.fullmatch(r"-?\d+", v):
            warn(f"{path}: {ctype} with value '{value}', using '0'")
            return "0"
    if ctype == "dateTime":
        if v == "":
            return "0001-01-01T00:00:00Z"
        m = re.fullmatch(r"(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2}(?:\.\d+)?)(Z?)", v)
        if m:
            fixed = f"{m.group(1)}T{m.group(2)}Z"
            if fixed != v:
                warn(f"{path}: dateTime '{v}' normalised to '{fixed}'")
            return fixed
        warn(f"{path}: unparseable dateTime '{v}'")
    if re.fullmatch(r"\d+(\.\d+)?E\+\d+", v):
        warn(f"{path}: value '{v}' looks like Excel scientific notation")
    return v


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main(csv_path, out_path):
    rows = []
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        reader = csv.reader(f)
        next(reader)  # header
        for lineno, r in enumerate(reader, start=2):
            if not r or not r[0].strip():
                continue
            r = (r + [""] * 7)[:7]
            path = r[0].strip()
            if path != "Device" and not path.startswith("Device."):
                continue
            rows.append(dict(lineno=lineno, path=path, obj=is_true(r[1]), wr=is_true(r[2]),
                             value=r[3], type=r[4].strip()))

    # Concrete segments for each row
    for row in rows:
        row["segs"] = split_concrete(row["path"])
        row["cpath"] = ".".join(row["segs"])
        row["schema"] = to_schema(row["segs"])

    # Every proper prefix of a row is an object; rows flagged Object are objects
    all_paths = set(r["cpath"] for r in rows)
    prefixes = set()
    for r in rows:
        segs = r["segs"]
        for n in range(1, len(segs)):
            prefixes.add(".".join(segs[:n]))
    for r in rows:
        if r["obj"] and r["cpath"] in all_paths:
            prefixes.add(r["cpath"])
        if not r["obj"] and r["cpath"] in prefixes and r["cpath"] != "Device":
            warn(f"line {r['lineno']}: {r['path']} marked Object=FALSE but has children; treated as object")

    # Multi-instance tables: any object whose last segment is an instance number
    tables = OrderedDict()   # schema -> writable
    instances = []           # concrete instance object paths, in CSV order
    for p in sorted(prefixes, key=lambda s: (s.count("."), s)):
        segs = p.split(".")
        if segs[-1].isdigit():
            schema = to_schema(segs)
            tables.setdefault(schema, False)
            if p not in instances:
                instances.append(p)

    # Table writability: from the table row itself (e.g. 'Device.Hosts.AccessControl', Object=TRUE, Writable=TRUE)
    for r in rows:
        if r["obj"]:
            cand = r["schema"] + ".{i}"
            if cand in tables and r["wr"]:
                tables[cand] = True

    # Parameters
    params = OrderedDict()   # schema -> dict(type, wr)
    numentries = OrderedDict()  # schema -> table schema
    defaults = OrderedDict()    # concrete path -> value
    for r in rows:
        if r["cpath"] in prefixes or r["path"] == "Device":
            continue
        schema = r["schema"]
        name = r["segs"][-1]

        # NumberOfEntries parameters are computed by OBUSPA from the table
        if name.endswith("NumberOfEntries"):
            parent = schema.rsplit(".", 1)[0]
            tbl = parent + "." + name[: -len("NumberOfEntries")] + ".{i}"
            obj_schema = tbl[: -len(".{i}")]
            if tbl not in tables:
                # Object exists in CSV but has no instances (e.g. IPv6Address): still a table
                if any(to_schema(p.split(".")) == obj_schema for p in prefixes):
                    tables[tbl] = False
                    warn(f"{schema}: table {tbl} has no instances in CSV; registered empty")
            if tbl in tables:
                numentries[schema] = tbl
                continue
            warn(f"{schema}: no matching table {tbl}; registered as plain parameter")

        ctype = r["type"].split(":")[-1] if r["type"] else ""
        if schema in TYPE_OVERRIDES:
            if ctype != TYPE_OVERRIDES[schema]:
                warn(f"{schema}: type '{ctype}' overridden to '{TYPE_OVERRIDES[schema]}'")
            ctype = TYPE_OVERRIDES[schema]
        if ctype == "":
            warn(f"{schema}: no type given, using string")
            ctype = "string"
        if ctype not in TYPE_MAP:
            warn(f"{schema}: unknown type '{ctype}', using string")
            ctype = "string"

        if schema in params:
            prev = params[schema]
            if prev["type"] != ctype:
                warn(f"{r['path']}: type '{ctype}' conflicts with earlier '{prev['type']}' for {schema}; keeping earlier")
            if prev["wr"] != r["wr"]:
                warn(f"{r['path']}: writable flag conflicts for {schema}; using writable")
                prev["wr"] = True
            ctype = prev["type"]
        else:
            params[schema] = dict(type=ctype, wr=r["wr"])

        value = VALUE_OVERRIDES.get(r["cpath"], r["value"])
        if r["cpath"] in VALUE_OVERRIDES:
            warn(f"{r['cpath']}: value '{r['value']}' overridden to '{value}'")
        value = normalise_value(value, ctype, r["cpath"])
        if r["cpath"] in defaults and defaults[r["cpath"]] != value:
            warn(f"line {r['lineno']}: duplicate {r['cpath']} ('{defaults[r['cpath']]}' -> '{value}'); last one wins")
        defaults[r["cpath"]] = value

    # Order tables parent-first and compute parent index
    table_list = sorted(tables.keys(), key=lambda s: (s.count("{i}"), s))
    parent_idx = []
    for t in table_list:
        base = t[: -len(".{i}")]
        best = -1
        for j, cand in enumerate(table_list):
            if cand != t and base.startswith(cand + "."):
                if best < 0 or len(cand) > len(table_list[best]):
                    best = j
        parent_idx.append(best)

    # Emit C
    out = []
    out.append("/*")
    out.append(" * AUTO-GENERATED by tools/gen_tr181_dm.py from " + csv_path.split("/")[-1] + " - DO NOT EDIT")
    out.append(" */")
    out.append('#include "tr181_dm.h"')
    out.append("")
    out.append("// Multi-instance objects (parents before children)")
    out.append("const tr181_table_t tr181_tables[] =")
    out.append("{")
    for t, p in zip(table_list, parent_idx):
        out.append(f"    {{ {c_str(t)}, {'true' if tables[t] else 'false'}, {p} }},")
    out.append("};")
    out.append("const int tr181_num_tables = sizeof(tr181_tables) / sizeof(tr181_tables[0]);")
    out.append("")
    out.append("// Parameters")
    out.append("const tr181_param_t tr181_params[] =")
    out.append("{")
    for s, info in params.items():
        out.append(f"    {{ {c_str(s)}, {TYPE_MAP[info['type']]}, {'true' if info['wr'] else 'false'} }},")
    out.append("};")
    out.append("const int tr181_num_params = sizeof(tr181_params) / sizeof(tr181_params[0]);")
    out.append("")
    out.append("// NumberOfEntries parameters (value computed by OBUSPA core from the table)")
    out.append("const tr181_numentries_t tr181_numentries[] =")
    out.append("{")
    for s, t in numentries.items():
        out.append(f"    {{ {c_str(s)}, {c_str(t)} }},")
    out.append("};")
    out.append("const int tr181_num_numentries = sizeof(tr181_numentries) / sizeof(tr181_numentries[0]);")
    out.append("")
    out.append("// Mock backend seed data: object instances")
    out.append("const char *tr181_seed_instances[] =")
    out.append("{")
    for i in instances:
        out.append(f"    {c_str(i)},")
    out.append("};")
    out.append("const int tr181_num_seed_instances = sizeof(tr181_seed_instances) / sizeof(tr181_seed_instances[0]);")
    out.append("")
    out.append("// Mock backend seed data: parameter values")
    out.append("const tr181_kv_t tr181_seed_values[] =")
    out.append("{")
    for k, v in defaults.items():
        out.append(f"    {{ {c_str(k)}, {c_str(v)} }},")
    out.append("};")
    out.append("const int tr181_num_seed_values = sizeof(tr181_seed_values) / sizeof(tr181_seed_values[0]);")
    out.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print(f"tables={len(table_list)} params={len(params)} numentries={len(numentries)} "
          f"instances={len(instances)} values={len(defaults)}")
    for w in warnings:
        print("WARNING: " + w, file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], sys.argv[2])
