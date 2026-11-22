#!/usr/bin/env bash
# Each sample runs to its time limit, drains, then verifies in a new process.
# Scratch RocksDB directories are deleted only after successful verification.
# Example: bash scripts/run_mako_cache_gc_soak.sh /abs/gc-soak /var/tmp/results baseline
set -euo pipefail

if [[ $# != 3 ]]; then
    echo 'usage: run_mako_cache_gc_soak.sh ABSOLUTE_BINARY OUTPUT_ROOT ARM' >&2
    exit 2
fi
binary=$(realpath -- "$1")
output_root=$(realpath -- "$2")
arm=$3
if [[ ! -x "$binary" || ! -d "$output_root" || "$output_root" == / ||
      ! "$arm" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo 'invalid executable, output root, or arm' >&2
    exit 2
fi
seconds=${MAKO_GC_SOAK_SECONDS:-960}
workers=${MAKO_GC_SOAK_WORKERS:-'1 4 8 16 32'}
rate=${MAKO_GC_SOAK_RATE_PER_WORKER:-0}
run_timeout=${MAKO_GC_SOAK_RUN_TIMEOUT:-1800}
reopen_timeout=${MAKO_GC_SOAK_REOPEN_TIMEOUT:-3600}
for numeric in "$seconds" "$rate" "$run_timeout" "$reopen_timeout"; do
    [[ "$numeric" =~ ^[0-9]+$ ]] || { echo 'invalid numeric setting' >&2; exit 2; }
done

# All cooperating Mako perf jobs use this lock. Waiting does not alter another
# benchmark's process, affinity, boost, or governor settings.
if [[ -e /var/tmp/mako-zoo2-perf-shuai.lock ]]; then
    exec 9</var/tmp/mako-zoo2-perf-shuai.lock
else
    exec 9>/var/tmp/mako-zoo2-perf-shuai.lock
fi
flock -n 9 || { echo 'another Mako benchmark holds the machine lock' >&2; exit 1; }
[[ $(< /sys/devices/system/cpu/cpufreq/boost) == 0 ]] || {
    echo 'CPU boost must already be disabled' >&2; exit 1;
}

for worker in $workers; do
    [[ "$worker" =~ ^[0-9]+$ && "$worker" -ge 1 && "$worker" -le 32 ]] || {
        echo 'worker count must be between 1 and 32' >&2; exit 2;
    }
    run=$(mktemp -d "$output_root/$arm-w$worker.XXXXXX")
    database="$run/rocks"
    manifest="$run/expected.txt"
    {
        hostname
        date --iso-8601=seconds
        sha256sum "$binary"
        uname -a
        uptime
        free -b
        df -B1 "$run"
        lscpu
        cat /sys/devices/system/cpu/cpufreq/boost
        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
        ps -eo user,pid,pcpu,pmem,comm --sort=-pcpu | head -20 || true
    } > "$run/machine.txt"
    echo "starting $arm W$worker, ${seconds}s, results $run"
    timeout --signal=TERM --kill-after=30s "${run_timeout}s" \
        taskset -c 0-39 "$binary" run \
        --path "$database" --manifest "$manifest" \
        --workers "$worker" --seconds "$seconds" --sample-seconds 10 \
        --writeback-cpu 32 --rate-per-worker "$rate" \
        --disk-limit-bytes 103079215104 --minimum-free-bytes 85899345920 \
        > "$run/run.jsonl" 2> "$run/run.stderr"
    timeout --signal=TERM --kill-after=30s "${reopen_timeout}s" \
        taskset -c 0-39 "$binary" verify \
        --path "$database" --manifest "$manifest" --writeback-cpu 32 \
        > "$run/verify.jsonl" 2> "$run/verify.stderr"
    [[ $(< /sys/devices/system/cpu/cpufreq/boost) == 0 ]] || {
        echo 'CPU boost changed during the sample' >&2; exit 1;
    }
    # These are the exact generated child paths from this iteration. Preserve
    # all reports and the expected-state manifest. Failed runs retain their DB.
    [[ $(realpath -- "$database") == "$run/rocks" &&
       "$run" == "$output_root/$arm-w$worker."* && -f "$manifest" ]] || {
        echo 'refusing cleanup of an unexpected path' >&2; exit 1;
    }
    find "$database" -depth -delete
    printf '{"completed":true,"run_exit":0,"verify_exit":0,"final_boost":0,"scratch_cleaned":true,"retrospective":false}\n' \
        > "$run/completed.json"
    echo "verified $arm W$worker; removed its completed scratch RocksDB, retained $run"
done
