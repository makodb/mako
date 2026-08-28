#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${MAKO_SCALING_BUILD_DIR:-${ROOT_DIR}/build-pr72-upstream-cmake43-clang22}"
STAMP="$(date -u +%Y%m%d_%H%M%S)"
OUT_DIR="${MAKO_PAPER_SOAK_OUT_DIR:-${ROOT_DIR}/third-party/redis/compat/benchmark_logs/paper_soak_${STAMP}}"
SAMPLE_SECONDS="${MAKO_PAPER_SOAK_SAMPLE_SECONDS:-5}"
RESOURCE_CSV="${OUT_DIR}/resource_timeseries.csv"
RUN_LOG="${OUT_DIR}/runner.log"

mkdir -p "${OUT_DIR}"
printf 'timestamp_utc,elapsed_sec,pid,rss_kb,file_descriptors,threads,process_cpu_pct\n' >"${RESOURCE_CSV}"

env \
    MAKO_SCALING_PROFILE=paper-soak \
    MAKO_SCALING_BUILD_DIR="${BUILD_DIR}" \
    MAKO_SCALING_TARGETS=redis \
    MAKO_SCALING_WORKERS="${MAKO_PAPER_SOAK_WORKERS:-32}" \
    MAKO_SCALING_WORKLOADS=mixed \
    MAKO_SCALING_KEYS="${MAKO_PAPER_SOAK_KEYS:-1000000}" \
    MAKO_SCALING_VALUE_SIZE="${MAKO_PAPER_SOAK_VALUE_SIZE:-8}" \
    MAKO_SCALING_READ_PERCENT="${MAKO_PAPER_SOAK_READ_PERCENT:-80}" \
    MAKO_SCALING_CLIENTS_PER_WORKER="${MAKO_PAPER_SOAK_CLIENTS_PER_WORKER:-2}" \
    MAKO_SCALING_PIPELINE_DEPTH="${MAKO_PAPER_SOAK_PIPELINE_DEPTH:-64}" \
    MAKO_SCALING_OUT_DIR="${OUT_DIR}" \
    "${PYTHON_BIN:-python3}" "${ROOT_DIR}/third-party/redis/compat/run_scalability_benchmark.py" \
    >"${RUN_LOG}" 2>&1 &
runner_pid=$!

cleanup() {
    if kill -0 "${runner_pid}" >/dev/null 2>&1; then
        kill "${runner_pid}" >/dev/null 2>&1 || true
        wait "${runner_pid}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

start_epoch="$(date +%s)"
server_pid=""
while true; do
    runner_state="$(ps -p "${runner_pid}" -o stat= 2>/dev/null | tr -d ' ' || true)"
    if [[ -z "${runner_state}" || "${runner_state}" == Z* ]]; then
        break
    fi
    if [[ -z "${server_pid}" ]] || ! kill -0 "${server_pid}" >/dev/null 2>&1; then
        server_pid="$(pgrep -f "^${BUILD_DIR}/makoCon$" | head -n 1 || true)"
    fi
    if [[ -n "${server_pid}" && -r "/proc/${server_pid}/status" ]]; then
        now_epoch="$(date +%s)"
        rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${server_pid}/status")"
        threads="$(awk '/Threads:/ {print $2}' "/proc/${server_pid}/status")"
        file_descriptors="$(find "/proc/${server_pid}/fd" -maxdepth 1 -type l 2>/dev/null | wc -l)"
        process_cpu_pct="$(ps -p "${server_pid}" -o %cpu= | tr -d ' ')"
        printf '%s,%s,%s,%s,%s,%s,%s\n' \
            "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            "$((now_epoch - start_epoch))" \
            "${server_pid}" \
            "${rss_kb:-0}" \
            "${file_descriptors}" \
            "${threads:-0}" \
            "${process_cpu_pct:-0}" >>"${RESOURCE_CSV}"
    fi
    sleep "${SAMPLE_SECONDS}"
done

wait "${runner_pid}"
trap - EXIT INT TERM

"${PYTHON_BIN:-python3}" - "${RESOURCE_CSV}" "${OUT_DIR}/resource_summary.json" <<'PY'
import csv
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
target = Path(sys.argv[2])
with source.open(newline="") as handle:
    rows = list(csv.DictReader(handle))
if not rows:
    raise SystemExit("paper soak produced no resource samples")

def values(name):
    return [float(row[name]) for row in rows]

rss = values("rss_kb")
fds = values("file_descriptors")
threads = values("threads")
elapsed = values("elapsed_sec")
hours = max((elapsed[-1] - elapsed[0]) / 3600.0, 1e-9)
summary = {
    "samples": len(rows),
    "elapsed_seconds": elapsed[-1] - elapsed[0],
    "rss_kb": {
        "start": rss[0],
        "end": rss[-1],
        "min": min(rss),
        "max": max(rss),
        "end_minus_start": rss[-1] - rss[0],
        "endpoint_change_kb_per_hour": (rss[-1] - rss[0]) / hours,
    },
    "file_descriptors": {
        "start": fds[0],
        "end": fds[-1],
        "min": min(fds),
        "max": max(fds),
    },
    "threads": {
        "start": threads[0],
        "end": threads[-1],
        "min": min(threads),
        "max": max(threads),
    },
}
target.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
print(target)
PY

echo "Paper soak results: ${OUT_DIR}"
