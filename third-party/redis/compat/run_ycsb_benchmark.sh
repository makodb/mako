#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
YCSB_HOME="${YCSB_HOME:-/tmp/ycsb-0.17.0}"
YCSB_BIN="${YCSB_BIN:-${YCSB_HOME}/bin/ycsb}"
YCSB_PYTHON="${YCSB_PYTHON:-}"
YCSB_WORKLOADS="${YCSB_WORKLOADS:-workloada workloadb workloadc workloadf}"
YCSB_RECORDCOUNT="${YCSB_RECORDCOUNT:-10000}"
YCSB_OPERATIONCOUNT="${YCSB_OPERATIONCOUNT:-10000}"
YCSB_THREADS="${YCSB_THREADS:-16}"
YCSB_FIELDCOUNT="${YCSB_FIELDCOUNT:-10}"
YCSB_FIELDLENGTH="${YCSB_FIELDLENGTH:-100}"
YCSB_TARGETS="${YCSB_TARGETS:-mako,redis}"
MAKO_HOST="${MAKO_HOST:-127.0.0.1}"
MAKO_PORT="${MAKO_PORT:-6380}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
OUT_DIR="${YCSB_OUT_DIR:-${ROOT_DIR}/third-party/redis/compat/benchmark_logs}"
STAMP="$(date -u +%Y%m%d_%H%M%S)"
CSV_FILE="${OUT_DIR}/ycsb_${STAMP}.csv"

mkdir -p "${OUT_DIR}"

if [[ ! -f "${YCSB_BIN}" ]]; then
    echo "YCSB benchmark N/A: missing launcher ${YCSB_BIN}" >&2
    echo "Set YCSB_HOME or YCSB_BIN to a YCSB distribution with the Redis binding." >&2
    exit 78
fi

if [[ -n "${YCSB_PYTHON}" ]]; then
    YCSB_CMD=("${YCSB_PYTHON}" "${YCSB_BIN}")
elif command -v python2 >/dev/null 2>&1 &&
    head -n 1 "${YCSB_BIN}" | grep -q 'python'; then
    YCSB_CMD=(python2 "${YCSB_BIN}")
else
    YCSB_CMD=("${YCSB_BIN}")
fi

if ! command -v redis-cli >/dev/null 2>&1; then
    echo "YCSB benchmark N/A: redis-cli not installed" >&2
    exit 78
fi

trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "${value}"
}

workload_path() {
    local workload="$1"
    if [[ -f "${YCSB_HOME}/workloads/${workload}" ]]; then
        printf '%s\n' "${YCSB_HOME}/workloads/${workload}"
        return 0
    fi
    if [[ -f "${workload}" ]]; then
        printf '%s\n' "${workload}"
        return 0
    fi
    echo "YCSB benchmark N/A: missing workload ${workload}" >&2
    exit 78
}

target_addr() {
    local target="$1"
    case "${target}" in
        mako)
            printf '%s %s\n' "${MAKO_HOST}" "${MAKO_PORT}"
            ;;
        redis)
            printf '%s %s\n' "${REDIS_HOST}" "${REDIS_PORT}"
            ;;
        *)
            echo "unknown YCSB target '${target}'" >&2
            return 1
            ;;
    esac
}

check_target() {
    local target="$1"
    local host="$2"
    local port="$3"
    if ! redis-cli -h "${host}" -p "${port}" PING >/dev/null 2>&1; then
        echo "YCSB benchmark FAIL: ${target} not reachable at ${host}:${port}" >&2
        return 1
    fi
}

extract_throughput() {
    local log_file="$1"
    awk -F, '
        /^\[OVERALL\], Throughput\(ops\/sec\),/ {
            gsub(/^[ \t]+|[ \t]+$/, "", $3);
            value=$3
        }
        END {
            if (value != "") print value
        }
    ' "${log_file}"
}

run_phase() {
    local phase="$1"
    local target="$2"
    local host="$3"
    local port="$4"
    local workload="$5"
    local workload_file="$6"
    local log_file="${OUT_DIR}/ycsb_${STAMP}_${target}_${workload}_${phase}.log"

    if "${YCSB_CMD[@]}" "${phase}" redis -s \
        -P "${workload_file}" \
        -p "redis.host=${host}" \
        -p "redis.port=${port}" \
        -p "recordcount=${YCSB_RECORDCOUNT}" \
        -p "operationcount=${YCSB_OPERATIONCOUNT}" \
        -p "threadcount=${YCSB_THREADS}" \
        -p "fieldcount=${YCSB_FIELDCOUNT}" \
        -p "fieldlength=${YCSB_FIELDLENGTH}" \
        >"${log_file}" 2>&1; then
        local throughput
        throughput="$(extract_throughput "${log_file}")"
        printf '%s,%s,%s,PASS,%s,%s\n' \
            "${target}" "${workload}" "${phase}" "${throughput:-}" "${log_file}" >>"${CSV_FILE}"
        echo "${target} ${workload} ${phase}: PASS throughput=${throughput:-n/a}"
    else
        printf '%s,%s,%s,FAIL,,%s\n' \
            "${target}" "${workload}" "${phase}" "${log_file}" >>"${CSV_FILE}"
        echo "${target} ${workload} ${phase}: FAIL, see ${log_file}" >&2
        return 1
    fi
}

printf 'target,workload,phase,status,throughput_ops_sec,log\n' >"${CSV_FILE}"

IFS=',' read -r -a targets <<<"${YCSB_TARGETS}"
for raw_target in "${targets[@]}"; do
    target="$(trim "${raw_target}")"
    [[ -z "${target}" ]] && continue
    read -r host port <<<"$(target_addr "${target}")"
    check_target "${target}" "${host}" "${port}"
    for workload in ${YCSB_WORKLOADS}; do
        workload_file="$(workload_path "${workload}")"
        redis-cli -h "${host}" -p "${port}" FLUSHALL >/dev/null
        run_phase load "${target}" "${host}" "${port}" "${workload}" "${workload_file}"
        run_phase run "${target}" "${host}" "${port}" "${workload}" "${workload_file}"
    done
done

echo "Saved YCSB benchmark CSV: ${CSV_FILE}"
