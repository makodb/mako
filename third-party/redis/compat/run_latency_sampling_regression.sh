#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${MAKO_SCALING_BUILD_DIR:-${ROOT_DIR}/build-pr72-upstream-cmake43-clang22}"
if [[ -n "${MAKO_LATENCY_REGRESSION_OUT_DIR:-}" ]]; then
    OUT_DIR="${MAKO_LATENCY_REGRESSION_OUT_DIR}"
    mkdir -p "${OUT_DIR}"
    KEEP_ARTIFACT=1
else
    OUT_DIR="$(mktemp -d /tmp/mako-latency-reservoir-regression.XXXXXX)"
    KEEP_ARTIFACT=0
fi
SAMPLE_LIMIT="${MAKO_LATENCY_REGRESSION_SAMPLE_LIMIT:-1024}"
CLIENTS=64

cleanup() {
    if [[ "${KEEP_ARTIFACT}" -eq 0 ]]; then
        rm -rf "${OUT_DIR}"
    fi
}
trap cleanup EXIT

env \
    MAKO_SCALING_BUILD_DIR="${BUILD_DIR}" \
    MAKO_SCALING_TARGETS=redis \
    MAKO_SCALING_WORKERS=32 \
    MAKO_SCALING_WORKLOADS=mixed \
    MAKO_SCALING_KEYS=100000 \
    MAKO_SCALING_DURATION=5 \
    MAKO_SCALING_WARMUP=1 \
    MAKO_SCALING_REPEATS=1 \
    MAKO_SCALING_CLIENTS_PER_WORKER=2 \
    MAKO_SCALING_PIPELINE_DEPTH=64 \
    MAKO_SCALING_MAX_LATENCY_SAMPLES_PER_THREAD="${SAMPLE_LIMIT}" \
    MAKO_SCALING_OUT_DIR="${OUT_DIR}" \
    "${PYTHON_BIN:-python3}" \
    "${ROOT_DIR}/third-party/redis/compat/run_scalability_benchmark.py"

"${PYTHON_BIN:-python3}" - \
    "${OUT_DIR}/scalability_raw.csv" \
    "${OUT_DIR}/regression_summary.json" \
    "${SAMPLE_LIMIT}" \
    "${CLIENTS}" <<'PY'
import csv
import json
import sys
from pathlib import Path

path, output_path, limit_text, clients_text = sys.argv[1:]
row = next(csv.DictReader(open(path, newline="")))
limit = int(limit_text)
clients = int(clients_text)
samples = int(row["latency_samples"])
observations = int(row["latency_observations"])
expected = limit * clients
if samples != expected:
    raise SystemExit(f"expected {expected} retained samples, found {samples}")
if observations <= samples * 100:
    raise SystemExit(
        f"expected observations to exceed samples by 100x: "
        f"observations={observations} samples={samples}"
    )
if row["latency_sampling"] != "deterministic-reservoir":
    raise SystemExit(f"unexpected latency sampler: {row['latency_sampling']}")
summary = {
    "status": "pass",
    "latency_sampling": row["latency_sampling"],
    "clients": clients,
    "sample_limit_per_client": limit,
    "expected_retained_samples": expected,
    "retained_samples": samples,
    "latency_observations": observations,
    "observations_per_retained_sample": observations / samples,
}
Path(output_path).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
print(
    f"latency reservoir regression passed: "
    f"observations={observations} retained={samples}"
)
PY
