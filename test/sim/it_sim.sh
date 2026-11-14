#!/usr/bin/env bash
# Integration tests for the CPE simulator supervisor (Linux only).
#   test/sim/it_sim.sh <obuspa> <plugin.so> [it_name...]
# SKIP_PLUGIN_CHECKS=1 skips the assertions that need tr181 mock identity/persistence
# (Ethernet MACAddress rewrite, WiFi SSID persistence/reset).
#
# Note: core forbids changing an Alias once assigned, so the "persisted value" used
# by these tests is Device.LocalAgent.Controller.1.EndpointID (seeded by reset_tmpl.txt).
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <obuspa> <plugin.so> [it_name...]" >&2
    exit 2
fi
OBUSPA=$(realpath "$1")
PLUGIN=$(realpath "$2")
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
TMPL="$HERE/reset_tmpl.txt"
SKIP_PLUGIN_CHECKS=${SKIP_PLUGIN_CHECKS:-0}
SSID_SEED="CPE-C201-TEST4-5G"
CTRL_EID="Device.LocalAgent.Controller.1.EndpointID"

ALL_CASES=(it_identities it_persist_reboot it_factory_reset it_stop_stays_down it_crash_restart
           it_restart_keeps_identity it_conf_mismatch it_second_supervisor_refused
           it_bad_template_no_damage it_no_orphans_after_sigterm it_crash_loop_backoff
           it_no_sim_unchanged it_mqtt_connect)

# ---------------------------------------------------------------- helpers
# Per-case globals: T (temp dir), SIMROOT, LOG (supervisor output), SUP (current supervisor pid)

fail() { echo "$*" > "$T/.reason"; exit 1; }

cpedir() { printf '%s/cpe-%08d' "$SIMROOT" "$1"; }

# cli <idx> <command...>
cli() {
    local dir
    dir=$(cpedir "$1")
    "$OBUSPA" -s "$dir/cli.sock" -f "$dir/usp.db" -c "${@:2}" 2>&1
}

# getv <idx> <path> -> prints the value (empty if absent)
getv() {
    cli "$1" get "$2" | sed -n "s/^$2 => //p" | head -1
}

# expect_eq <what> <actual> <expected>
expect_eq() {
    [ "$2" == "$3" ] || fail "$1: got '$2', expected '$3'"
}

# wait_for <description> <command...>: polls for up to 30s
wait_for() {
    local desc=$1; shift
    local i
    for ((i = 0; i < 150; i++)); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    fail "timeout waiting for: $desc"
}

# wait_for_n <polls (0.2s each)> <description> <command...>
wait_for_n() {
    local n=$1 desc=$2 i; shift 2
    for ((i = 0; i < n; i++)); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    fail "timeout waiting for: $desc"
}

start_sup() {
    "$OBUSPA" --sim-root "$SIMROOT" "$@" >>"$LOG" 2>&1 &
    SUP=$!
    echo "$SUP" >> "$T/.sups"
}

# Stops the supervisor gracefully and returns its exit code in SUP_RC
stop_sup() {
    kill -TERM "$SUP" 2>/dev/null || true
    local i
    for ((i = 0; i < 75; i++)); do
        kill -0 "$SUP" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$SUP" 2>/dev/null && fail "supervisor $SUP did not exit within 15s of SIGTERM"
    SUP_RC=0
    wait "$SUP" || SUP_RC=$?
}

# launches <idx> -> number of "sim: launch idx=<idx>" lines
launches() { grep -c "^sim: launch idx=$1 " "$LOG" || true; }

# child_pid <idx> -> pid of the latest launch of <idx>
child_pid() { grep "^sim: launch idx=$1 " "$LOG" | tail -1 | sed 's/.*pid=\([0-9]*\).*/\1/'; }

is_ready() { [ -n "$(getv "$1" Device.DeviceInfo.SerialNumber)" ]; }

new_pid_ready() { local p; p=$(child_pid "$1"); [ -n "$p" ] && [ "$p" != "$2" ] && is_ready "$1"; }

proc_gone() { ! kill -0 "$1" 2>/dev/null || [ "$(ps -o stat= -p "$1" 2>/dev/null | cut -c1)" == "Z" ]; }

no_cpe_procs() { ! pgrep -f "$SIMROOT/cpe-" >/dev/null; }

STD_ARGS() { echo --sim-base 100 --sim-ramp 0 --sim-reset "$TMPL" -x "$PLUGIN"; }

# ---------------------------------------------------------------- cases
it_identities() {
    start_sup --sim-procs 2 --sim-per-proc 3 $(STD_ARGS)
    local idx n hex
    for idx in 100 101 102 103 104 105; do
        wait_for "cpe $idx ready" is_ready $idx
    done
    n=$(ls -d "$SIMROOT"/cpe-0000010[0-5] | wc -l)
    expect_eq "instance dirs" "$n" 6
    n=$(ls -d "$SIMROOT"/cpe-* | wc -l)
    expect_eq "total cpe dirs" "$n" 6
    n=$(tail -n +2 "$SIMROOT/sim.map" | grep -c .)
    expect_eq "sim.map data rows" "$n" 6
    for idx in 100 101 102 103 104 105; do
        expect_eq "SN $idx" "$(getv $idx Device.DeviceInfo.SerialNumber)" "SIM00000$idx"
        expect_eq "EID $idx" "$(getv $idx Device.LocalAgent.EndpointID)" "os::012345-SIM00000$idx"
        expect_eq "Alias $idx" "$(getv $idx Device.LocalAgent.Controller.1.Alias)" "ctrl-$idx"
        if [ "$SKIP_PLUGIN_CHECKS" != 1 ]; then
            hex=$(printf %02X "$idx")
            expect_eq "MAC $idx" "$(getv $idx Device.Ethernet.Interface.1.MACAddress)" "02:00:00:00:$hex:53"
        fi
    done
}

it_persist_reboot() {
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    local pid
    pid=$(child_pid 100)
    cli 100 set $CTRL_EID foo >/dev/null
    cli 100 set Device.WiFi.SSID.1.SSID bar >/dev/null
    expect_eq "controller EID after set" "$(getv 100 $CTRL_EID)" foo
    cli 100 operate "Device.Reboot()" >/dev/null
    wait_for "cpe 100 relaunched after reboot" new_pid_ready 100 "$pid"
    grep -q "^sim: exit idx=100 pid=$pid .*action=restart" "$LOG" || fail "no restart exit line for pid $pid"
    expect_eq "controller EID after reboot" "$(getv 100 $CTRL_EID)" foo
    if [ "$SKIP_PLUGIN_CHECKS" != 1 ]; then
        expect_eq "SSID after reboot" "$(getv 100 Device.WiFi.SSID.1.SSID)" bar
    fi
}

it_factory_reset() {
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    local pid
    pid=$(child_pid 100)
    cli 100 set $CTRL_EID foo >/dev/null
    cli 100 set Device.WiFi.SSID.1.SSID bar >/dev/null
    expect_eq "controller EID after set" "$(getv 100 $CTRL_EID)" foo
    cli 100 operate "Device.FactoryReset()" >/dev/null
    wait_for "cpe 100 relaunched after factory reset" new_pid_ready 100 "$pid"
    grep -q "^sim: exit idx=100 pid=$pid .*action=factory-reset" "$LOG" || fail "no factory-reset exit line for pid $pid"
    expect_eq "Alias after factory reset" "$(getv 100 Device.LocalAgent.Controller.1.Alias)" ctrl-100
    expect_eq "controller EID after factory reset" "$(getv 100 $CTRL_EID)" self::ctrl-SIM00000100
    expect_eq "SN after factory reset" "$(getv 100 Device.DeviceInfo.SerialNumber)" SIM00000100
    if [ "$SKIP_PLUGIN_CHECKS" != 1 ]; then
        expect_eq "SSID after factory reset" "$(getv 100 Device.WiFi.SSID.1.SSID)" "$SSID_SEED"
    fi
}

it_stop_stays_down() {
    start_sup --sim-per-proc 2 $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    wait_for "cpe 101 ready" is_ready 101
    local pid
    pid=$(child_pid 100)
    cli 100 stop >/dev/null
    wait_for "cpe 100 exit" proc_gone "$pid"
    sleep 15
    expect_eq "launches of idx 100" "$(launches 100)" 1
    wait_for "summary line with running=1/2 stopped=1" grep -q "^sim: running=1/2 stopped=1 " "$LOG"
    is_ready 101 || fail "cpe 101 not running"
}

it_crash_restart() {
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    local pid t0 t1
    pid=$(child_pid 100)
    t0=$(date +%s.%N)
    kill -9 "$pid"
    wait_for "cpe 100 relaunched after kill -9" new_pid_ready 100 "$pid"
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN { exit !(b - a < 6) }' || fail "relaunch took $(awk -v a="$t0" -v b="$t1" 'BEGIN{print b-a}')s"
    grep -q "^sim: exit idx=100 pid=$pid .*action=crash" "$LOG" || fail "no crash exit line for pid $pid"
}

it_restart_keeps_identity() {
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    cli 100 set $CTRL_EID foo >/dev/null
    expect_eq "controller EID after set" "$(getv 100 $CTRL_EID)" foo
    stop_sup
    expect_eq "supervisor rc" "$SUP_RC" 0
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready after supervisor restart" is_ready 100
    expect_eq "SN" "$(getv 100 Device.DeviceInfo.SerialNumber)" SIM00000100
    expect_eq "EID" "$(getv 100 Device.LocalAgent.EndpointID)" os::012345-SIM00000100
    expect_eq "controller EID" "$(getv 100 $CTRL_EID)" foo
    expect_eq "Alias" "$(getv 100 Device.LocalAgent.Controller.1.Alias)" ctrl-100
}

it_conf_mismatch() {
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    cli 100 set $CTRL_EID foo >/dev/null
    stop_sup
    local rc=0
    timeout 20 "$OBUSPA" --sim-root "$SIMROOT" --sim-sn-fmt "X%06u" $(STD_ARGS) >"$T/out2" 2>"$T/err2" || rc=$?
    expect_eq "rc with changed sn-fmt" "$rc" 2
    grep -q sn_fmt "$T/err2" || fail "stderr lacks sn_fmt: $(cat "$T/err2")"
    [ ! -e "$SIMROOT/sim.pid" ] || fail "sim.pid left behind by refused supervisor"
    start_sup --sim-sn-fmt "X%06u" --sim-reidentify $(STD_ARGS)
    wait_for "cpe 100 ready after reidentify" is_ready 100
    expect_eq "SN" "$(getv 100 Device.DeviceInfo.SerialNumber)" X000100
    expect_eq "EID" "$(getv 100 Device.LocalAgent.EndpointID)" os::012345-X000100
    expect_eq "Alias" "$(getv 100 Device.LocalAgent.Controller.1.Alias)" ctrl-100
    expect_eq "controller EID" "$(getv 100 $CTRL_EID)" self::ctrl-X000100
    grep -q "^sn_fmt=X%06u$" "$SIMROOT/sim.conf" || fail "sim.conf not updated"
}

# snapshot -> sorted "md5 mtime path" of every file under SIMROOT (except the lock file)
snapshot() {
    find "$SIMROOT" -type f ! -name sim.pid -printf '%T@ %p\n' | sort -k2 > "$T/.mtimes"
    find "$SIMROOT" -type f ! -name sim.pid -exec md5sum {} + | sort -k2 | paste -d' ' - "$T/.mtimes"
}

it_bad_template_no_damage() {
    start_sup --sim-per-proc 2 $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    wait_for "cpe 101 ready" is_ready 101
    stop_sup
    [ -f "$(cpedir 100)/usp.db" ] && [ -f "$(cpedir 101)/usp.db" ] || fail "usp.db files missing after first run"
    snapshot > "$T/before"
    { head -1 "$TMPL"; echo 'Device.LocalAgent.Controller.1.EndpointID "self::${SNN}"'; } > "$T/bad_tmpl.txt"
    sleep 1   # make any rewrite visible in mtimes
    local rc=0
    timeout 20 "$OBUSPA" --sim-root "$SIMROOT" --sim-per-proc 2 --sim-base 100 --sim-ramp 0 \
        --sim-sn-fmt "X%06u" --sim-reidentify --sim-reset "$T/bad_tmpl.txt" -x "$PLUGIN" >"$T/out2" 2>"$T/err2" || rc=$?
    expect_eq "rc with bad template" "$rc" 2
    grep -q "bad_tmpl.txt:2 .*SNN" "$T/err2" || fail "stderr does not name line 2 / \${SNN}: $(cat "$T/err2")"
    snapshot > "$T/after"
    diff "$T/before" "$T/after" >"$T/snapdiff" || fail "files under sim-root changed: $(cat "$T/snapdiff")"
    grep -q "^sn_fmt=SIM%08u$" "$SIMROOT/sim.conf" || fail "sim.conf rewritten"
    if grep -q "^sim: launch" "$T/out2"; then fail "refused supervisor launched instances"; fi
}

it_second_supervisor_refused() {
    start_sup $(STD_ARGS)
    wait_for "cpe 100 ready" is_ready 100
    local rc=0
    timeout 20 "$OBUSPA" --sim-root "$SIMROOT" $(STD_ARGS) >"$T/out2" 2>"$T/err2" || rc=$?
    expect_eq "rc of second supervisor" "$rc" 2
    grep -q "another supervisor" "$T/err2" || fail "stderr lacks 'another supervisor': $(cat "$T/err2")"
    kill -0 "$SUP" 2>/dev/null || fail "first supervisor died"
    [ "$(cat "$SIMROOT/sim.pid")" == "$SUP" ] || fail "sim.pid does not hold first supervisor pid"
    expect_eq "launches of idx 100" "$(launches 100)" 1
}

it_no_orphans_after_sigterm() {
    start_sup --sim-procs 2 --sim-per-proc 2 $(STD_ARGS)
    local idx
    for idx in 100 101 102 103; do wait_for "cpe $idx ready" is_ready $idx; done
    kill -TERM "$SUP"
    local i
    for ((i = 0; i < 50; i++)); do
        no_cpe_procs && ! kill -0 "$SUP" 2>/dev/null && break
        sleep 0.2
    done
    no_cpe_procs || fail "children still alive 10s after SIGTERM: $(pgrep -af "$SIMROOT/cpe-")"
    kill -0 "$SUP" 2>/dev/null && fail "supervisor still alive 10s after SIGTERM"
    local rc=0
    wait "$SUP" || rc=$?
    expect_eq "supervisor rc" "$rc" 0
    [ ! -e "$SIMROOT/sim.pid" ] || fail "sim.pid not removed"
}

it_crash_loop_backoff() {
    # obuspa only logs a failed -x plugin load and keeps running, so an out-of-range
    # verbosity (-v 99, exit 255 right after start) is used as the instant-crash trigger
    start_sup --sim-procs 1 --sim-per-proc 2 --sim-base 100 -v 99
    sleep 8
    local idx n
    for idx in 100 101; do
        n=$(launches $idx)
        [ "$n" -ge 2 ] && [ "$n" -le 5 ] || fail "idx $idx launched $n times in 8s (want 2..5)"
    done
    stop_sup
    expect_eq "supervisor rc" "$SUP_RC" 0
    grep -q "^sim: running=.* crashes=[1-9]" "$LOG" || fail "no summary line with crashes>0"
}

it_no_sim_unchanged() {
    local d="$T/plain" rc=0 pid
    mkdir -p "$d"
    "$OBUSPA" -v 4 -f "$d/usp.db" -s "$d/cli.sock" >"$LOG" 2>&1 &
    pid=$!
    echo "$pid" >> "$T/.sups"
    plain_ready() { "$OBUSPA" -s "$d/cli.sock" -f "$d/usp.db" -c get Device.DeviceInfo.SerialNumber 2>&1 | grep -q " => "; }
    wait_for "plain agent ready" plain_ready
    "$OBUSPA" -s "$d/cli.sock" -f "$d/usp.db" -c stop >/dev/null 2>&1
    wait_for "plain agent exit" proc_gone "$pid"
    wait "$pid" || rc=$?
    expect_eq "plain agent rc" "$rc" 0
    [ -s "$LOG" ] || fail "plain agent produced no log output"
    if grep -q '^\[' "$LOG"; then fail "log line with [ prefix: $(grep -m1 '^\[' "$LOG")"; fi
    if grep -q '^sim:' "$LOG"; then fail "supervisor output in plain mode"; fi
}

# Needs a reachable MQTT broker: MQTT_BROKER=host:port (skipped otherwise)
it_mqtt_connect() {
    if [ -z "${MQTT_BROKER:-}" ]; then
        echo "SKIP it_mqtt_connect (MQTT_BROKER not set)" >&2
        return 0
    fi
    local host=${MQTT_BROKER%:*} port=${MQTT_BROKER##*:} idx
    [ -n "$host" ] && [ "$host" != "$MQTT_BROKER" ] || fail "MQTT_BROKER must be host:port, got '$MQTT_BROKER'"
    sed -e "s|^Device.MQTT.Client.1.BrokerAddress .*|Device.MQTT.Client.1.BrokerAddress \"$host\"|" \
        -e "s|^Device.MQTT.Client.1.BrokerPort .*|Device.MQTT.Client.1.BrokerPort \"$port\"|" \
        "$HERE/mqtt_tmpl.txt" > "$T/mqtt_tmpl.txt"
    grep -q "BrokerAddress \"$host\"" "$T/mqtt_tmpl.txt" || fail "broker substitution failed"
    start_sup --sim-procs 2 --sim-per-proc 5 --sim-base 100 --sim-ramp 0 --sim-reset "$T/mqtt_tmpl.txt" -x "$PLUGIN"
    mqtt_connected() { [ "$(getv "$1" Device.MQTT.Client.1.Status)" == "Connected" ]; }
    for ((idx = 100; idx < 110; idx++)); do
        wait_for_n 300 "cpe $idx MQTT Connected" mqtt_connected $idx
    done
}

# ---------------------------------------------------------------- runner
cleanup_case() {
    local p
    if [ -f "$T/.sups" ]; then
        while read -r p; do kill -TERM "$p" 2>/dev/null || true; done < "$T/.sups"
        for ((i = 0; i < 60; i++)); do
            local alive=0
            while read -r p; do kill -0 "$p" 2>/dev/null && alive=1; done < "$T/.sups"
            [ $alive == 0 ] && break
            sleep 0.2
        done
        while read -r p; do kill -KILL "$p" 2>/dev/null || true; done < "$T/.sups"
    fi
    pkill -KILL -f "$T/" 2>/dev/null || true
}

run_case() {
    local name=$1 rc
    T=$(mktemp -d /tmp/its.XXXXXX)
    SIMROOT="$T/r"
    LOG="$T/sup.log"
    : > "$LOG"
    set +e
    (
        set -eE
        trap 'echo "line $LINENO: command failed: $BASH_COMMAND" > "$T/.reason"' ERR
        trap cleanup_case EXIT
        "$name"
    )
    rc=$?
    set -e
    if [ $rc == 0 ]; then
        echo "PASS $name"
        rm -rf "$T"
        return 0
    fi
    echo "FAIL $name: $(cat "$T/.reason" 2>/dev/null || echo "exit $rc")"
    echo "---- supervisor log ($LOG, last 40 lines)"
    tail -n 40 "$LOG" | sed 's/^/    /'
    echo "----"
    return 1
}

CASES=("$@")
[ ${#CASES[@]} -gt 0 ] || CASES=("${ALL_CASES[@]}")
failed=0
for c in "${CASES[@]}"; do
    if ! declare -F "$c" >/dev/null; then echo "FAIL $c: no such test"; failed=$((failed + 1)); continue; fi
    run_case "$c" || failed=$((failed + 1))
done
echo "it_sim: ${#CASES[@]} run, $failed failed"
[ $failed == 0 ]
