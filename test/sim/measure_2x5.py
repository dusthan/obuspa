#!/usr/bin/env python3
"""Linux 2x5 smoke measurement; keep logs and JSON evidence in a new directory."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("obuspa", type=Path)
    parser.add_argument("plugin", type=Path)
    parser.add_argument("output", type=Path, help="new directory (must not exist)")
    parser.add_argument("--mqtt-broker", help="optional hostname-or-IPv4:port, anonymous TCP MQTT 5")
    args = parser.parse_args()
    binary, plugin, output = (p.resolve() for p in (args.obuspa, args.plugin, args.output))
    if platform.system() != "Linux":
        parser.error("Linux /proc is required")
    if not binary.is_file() or not plugin.is_file():
        parser.error("obuspa and plugin must exist")
    if args.mqtt_broker:
        match = re.fullmatch(r"([A-Za-z0-9_.-]+):([0-9]{1,5})", args.mqtt_broker)
        if not match or not 1 <= int(match[2]) <= 65535:
            parser.error("broker must be hostname-or-IPv4:port (1..65535)")
    output.mkdir(parents=True, exist_ok=False)
    root = output / "r"
    here = Path(__file__).resolve().parent
    template = (here / ("mqtt_tmpl.txt" if args.mqtt_broker else "reset_tmpl.txt")).read_text()
    if args.mqtt_broker:
        for field, value in (("BrokerAddress", match[1]), ("BrokerPort", match[2])):
            template = re.sub(rf"(?m)^Device\.MQTT\.Client\.1\.{field} .*",
                              f'Device.MQTT.Client.1.{field} "{value}"', template)
    reset = output / "template.txt"
    reset.write_text(template)
    command = [str(binary), "--sim-root", str(root), "--sim-procs", "2",
               "--sim-per-proc", "5", "--sim-base", "100", "--sim-ramp", "20",
               "--sim-reset", str(reset), "-x", str(plugin)]
    report = {"started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "platform": platform.platform(), "cpu_count": os.cpu_count(),
              "mem_total_kib": int(re.search(r"MemTotal:\s+(\d+)", Path("/proc/meminfo").read_text())[1]),
              "command": command, "mqtt_broker": args.mqtt_broker, "passed": False}
    supervisor = None
    started = time.monotonic()

    def get(idx, path, timeout=2):
        directory = root / f"cpe-{idx:08d}"
        result = subprocess.run([str(binary), "-s", str(directory / "cli.sock"),
                                 "-f", str(directory / "usp.db"), "-c", "get", path],
                                capture_output=True, text=True, timeout=timeout, check=True)
        prefix = path + " => "
        return next((line[len(prefix):] for line in result.stdout.splitlines()
                     if line.startswith(prefix)), "")

    def resources(pid):
        proc = Path(f"/proc/{pid}")
        return {"pss_kib": int(re.search(r"(?m)^Pss:\s+(\d+)", (proc / "smaps_rollup").read_text())[1]),
                "rss_kib": int(re.search(r"(?m)^VmRSS:\s+(\d+)", (proc / "status").read_text())[1]),
                "threads": len(list((proc / "task").iterdir()))}

    def interrupt(signum, frame):
        raise RuntimeError(f"interrupted by signal {signum}")

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    try:
        with (output / "supervisor.log").open("w") as log:
            supervisor = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                          start_new_session=True)
        deadline = started + 60
        for idx in range(100, 110):
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or supervisor.poll() is not None:
                    raise RuntimeError(f"fleet not ready within 60s, idx={idx}")
                try:
                    ready = get(idx, "Device.DeviceInfo.SerialNumber", min(2, remaining)) == f"SIM{idx:08d}"
                    if ready and args.mqtt_broker:
                        remaining = deadline - time.monotonic()
                        ready = remaining > 0 and get(idx, "Device.MQTT.Client.1.Status", min(2, remaining)) == "Connected"
                    if ready:
                        break
                except (subprocess.SubprocessError, OSError):
                    pass
                time.sleep(0.2)
        report["fleet_ready_seconds"] = round(time.monotonic() - started, 3)
        # Sample after 30 seconds at idle, including at least two supervisor summaries.
        time.sleep(30)
        log_text = (output / "supervisor.log").read_text()
        launches = re.findall(r"(?m)^sim: launch idx=(\d+) pid=(\d+)$", log_text)
        if len(launches) != 10 or "running=10/10 stopped=0 pending=0 restarts=0 crashes=0" not in log_text:
            raise RuntimeError("expected ten launches and a healthy supervisor summary")
        rows = []
        for index, pid in launches:
            idx = int(index)
            row = {"idx": idx, "pid": int(pid), "sn": get(idx, "Device.DeviceInfo.SerialNumber"),
                   "eid": get(idx, "Device.LocalAgent.EndpointID"),
                   "mac": get(idx, "Device.Ethernet.Interface.1.MACAddress"), **resources(pid)}
            assert row["sn"] == f"SIM{idx:08d}", row
            assert row["eid"] == f"os::012345-SIM{idx:08d}", row
            assert row["mac"] == f"02:00:00:00:{idx:02X}:53", row
            for name in ("usp.db", "mock.db"):
                assert (root / f"cpe-{idx:08d}" / name).stat().st_size > 0
            if args.mqtt_broker:
                row["mqtt_status"] = get(idx, "Device.MQTT.Client.1.Status")
                row["mqtt_client_id"] = get(idx, "Device.MQTT.Client.1.ClientID")
                row["response_topic"] = get(idx, "Device.LocalAgent.MTP.1.MQTT.ResponseTopicConfigured")
                assert row["mqtt_status"] == "Connected", row
                assert row["mqtt_client_id"] == row["sn"], row
                assert row["response_topic"] == "usp/agent/" + row["eid"], row
            rows.append(row)
        assert len((root / "sim.map").read_text().splitlines()) == 11
        report.update(instances=rows, total_pss_kib=sum(r["pss_kib"] for r in rows),
                      total_rss_kib=sum(r["rss_kib"] for r in rows),
                      total_threads=sum(r["threads"] for r in rows), supervisor=resources(supervisor.pid))
        if args.mqtt_broker and match[1] == "127.0.0.1":
            sockets = Path("/proc/net/tcp").read_text().splitlines()[1:]
            report["local_broker_established"] = sum(
                line.split()[3] == "01" and int(line.split()[1].split(":")[1], 16) == int(match[2])
                for line in sockets)
            assert report["local_broker_established"] == 10
        report["passed"] = True
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        shutdown = time.monotonic()
        if supervisor:
            supervisor.terminate()
            try:
                report["supervisor_exit_code"] = supervisor.wait(timeout=12)
            except subprocess.TimeoutExpired:
                report["passed"] = False
                report["error"] = "supervisor shutdown timed out"
                os.killpg(supervisor.pid, signal.SIGKILL)
                report["supervisor_exit_code"] = supervisor.wait()
            report["shutdown_seconds"] = round(time.monotonic() - shutdown, 3)
            # Also detect descendants on failure before launches were collected.
            leftovers = []
            for proc in Path("/proc").glob("[0-9]*"):
                try:
                    if str(root).encode() + b"/cpe-" in (proc / "cmdline").read_bytes():
                        leftovers.append(int(proc.name))
                except (FileNotFoundError, PermissionError, ProcessLookupError):
                    pass
            report["leftover_pids"] = leftovers
            report["passed"] &= report["supervisor_exit_code"] == 0 and not leftovers
            for pid in leftovers:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        report["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
