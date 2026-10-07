#!/usr/bin/env bash
set -euo pipefail

ROOT="${MAKO_CAPACITY_ROOT:-/home/users/ssoumojit/mako-pr}"
SERVER_BIN="${MAKO_CAPACITY_SERVER_BIN:-${ROOT}/build-pr72-o3-clang22/makoCon}"
CLIENT_BIN="${MAKO_CAPACITY_CLIENT_BIN:-/tmp/mako_bench_resp_scalability}"
HOST="${MAKO_CAPACITY_HOST:-127.0.0.1}"
PORT="${MAKO_CAPACITY_PORT:-6410}"
OUT="${MAKO_CAPACITY_OUT:-${ROOT}/third-party/redis/compat/benchmark_logs/capacity_sweep_$(date -u +%Y%m%d_%H%M%S)}"
CLIENTS="${MAKO_CAPACITY_CLIENTS:-1 4 16 32 64 128 256 512}"
WORKERS="${MAKO_CAPACITY_WORKERS:-32}"
SERVER_CPUS="${MAKO_CAPACITY_SERVER_CPUS:-0-$((WORKERS - 1))}"
CLIENT_CPUS="${MAKO_CAPACITY_CLIENT_CPUS:-32-63}"
RECOVERY_CLIENTS="${MAKO_CAPACITY_RECOVERY_CLIENTS:-$((WORKERS * 2))}"
WARMUP="${MAKO_CAPACITY_WARMUP:-3}"
DURATION="${MAKO_CAPACITY_DURATION:-8}"
REPEATS="${MAKO_CAPACITY_REPEATS:-2}"
PIPELINE="${MAKO_CAPACITY_PIPELINE:-64}"
KEYS="${MAKO_CAPACITY_KEYS:-1000000}"
VALUE_SIZE="${MAKO_CAPACITY_VALUE_SIZE:-8}"
READ_PERCENT="${MAKO_CAPACITY_READ_PERCENT:-80}"

mkdir -p "${OUT}"
SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -INT "${SERVER_PID}" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            kill -0 "${SERVER_PID}" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "${SERVER_PID}" 2>/dev/null; then
            kill -KILL "${SERVER_PID}" 2>/dev/null || true
        fi
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

cd "${ROOT}"
env \
    MAKO_HOST="${HOST}" \
    MAKO_PORT="${PORT}" \
    MAKO_REDIS_THREADS="${WORKERS}" \
    MAKO_REDIS_BACKEND=mako \
    MAKO_REPLICATION_ENABLED=0 \
    MAKO_PAXOS_PROC_NAME=localhost \
    MAKO_REDIS_BENCH_NO_DB=0 \
    MAKO_REDIS_BENCH_SKIP_TTL=0 \
    MAKO_REDIS_BENCH_SKIP_LOCKS=0 \
    MAKO_REDIS_BENCH_SKIP_MUTEX=0 \
    taskset -c "${SERVER_CPUS}" "${SERVER_BIN}" >"${OUT}/server.log" 2>&1 &
SERVER_PID="$!"
printf '%s\n' "${SERVER_PID}" >"${OUT}/server.pid"

ready=0
for _ in $(seq 1 200); do
    if redis-cli -h "${HOST}" -p "${PORT}" PING 2>/dev/null | grep -q PONG; then
        ready=1
        break
    fi
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        echo "server exited during startup" >&2
        exit 1
    fi
    sleep 0.1
done
if [[ "${ready}" -ne 1 ]]; then
    echo "server did not become ready" >&2
    exit 1
fi

sleep 5
preload_threads="${WORKERS}"
if [[ "${preload_threads}" -gt 8 ]]; then preload_threads=8; fi
taskset -c "${CLIENT_CPUS}" "${CLIENT_BIN}" \
    --host "${HOST}" --port "${PORT}" --keys "${KEYS}" \
    --value-size "${VALUE_SIZE}" --threads "${preload_threads}" --preload-threads "${preload_threads}" \
    --preload-max-retries 100 --preload-only >"${OUT}/preload.log" 2>&1

printf 'phase,clients,repeat,pipeline,duration_sec,total_ops,ops_per_sec,p50_us,p95_us,p99_us,server_cpu_cores,server_rss_kb,abort_delta,retry_delta,server_alive,ping_ok\n' >"${OUT}/capacity_summary.csv"

counter() {
    local name="$1"
    redis-cli -h "${HOST}" -p "${PORT}" INFO mako 2>/dev/null \
        | awk -F: -v name="${name}" '$1 == name { gsub(/\r/, "", $2); print $2; exit }'
}

run_one() {
    local phase="$1"
    local clients="$2"
    local repeat="$3"
    local duration="$4"
    local stem="${OUT}/${phase}_${clients}c_r${repeat}"
    local timeout_seconds=$((duration + 30))
    local clock_ticks
    local ticks_before
    local aborts_before
    local retries_before
    clock_ticks="$(getconf CLK_TCK)"
    ticks_before="$(awk '{print $14 + $15}' "/proc/${SERVER_PID}/stat")"
    aborts_before="$(counter mako_txn_aborts)"
    retries_before="$(counter mako_txn_retries)"

    if ! timeout "${timeout_seconds}s" taskset -c "${CLIENT_CPUS}" "${CLIENT_BIN}" \
        --name redis-over-mako --host "${HOST}" --port "${PORT}" \
        --keys "${KEYS}" --value-size "${VALUE_SIZE}" --threads "${clients}" \
        --duration "${duration}" --read-percent "${READ_PERCENT}" \
        --pipeline-depth "${PIPELINE}" --max-latency-samples-per-thread 8192 \
        --workloads mixed --skip-preload --out "${stem}.csv" >"${stem}.log" 2>&1; then
        printf '%s,%s,%s,%s,0,0,0,0,0,0,0,0,0,0,%s,%s\n' \
            "${phase}" "${clients}" "${repeat}" "${PIPELINE}" \
            "$(kill -0 "${SERVER_PID}" 2>/dev/null && echo 1 || echo 0)" \
            "$(redis-cli -h "${HOST}" -p "${PORT}" PING 2>/dev/null | grep -q PONG && echo 1 || echo 0)" \
            >>"${OUT}/capacity_summary.csv"
        return
    fi

    local alive=0
    local ping=0
    local ticks_after
    local aborts_after
    local retries_after
    local rss_kb
    kill -0 "${SERVER_PID}" 2>/dev/null && alive=1
    redis-cli -h "${HOST}" -p "${PORT}" PING 2>/dev/null | grep -q PONG && ping=1
    ticks_after="$(awk '{print $14 + $15}' "/proc/${SERVER_PID}/stat")"
    aborts_after="$(counter mako_txn_aborts)"
    retries_after="$(counter mako_txn_retries)"
    rss_kb="$(awk '/VmRSS:/ {print $2}' "/proc/${SERVER_PID}/status")"
    awk -F, -v phase="${phase}" -v clients="${clients}" -v repeat="${repeat}" \
        -v pipeline="${PIPELINE}" -v alive="${alive}" -v ping="${ping}" \
        -v ticks_before="${ticks_before}" -v ticks_after="${ticks_after}" \
        -v clock_ticks="${clock_ticks}" -v rss_kb="${rss_kb}" \
        -v abort_delta="$((aborts_after - aborts_before))" \
        -v retry_delta="$((retries_after - retries_before))" \
        'NR == 2 { cpu = (ticks_after - ticks_before) / (clock_ticks * $10); printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%.6f,%s,%s,%s,%s,%s\n", phase, clients, repeat, pipeline, $10, $11, $12, $14, $15, $16, cpu, rss_kb, abort_delta, retry_delta, alive, ping }' \
        "${stem}.csv" >>"${OUT}/capacity_summary.csv"
    redis-cli -h "${HOST}" -p "${PORT}" INFO mako >"${stem}.info" 2>&1 || true
}

for clients in ${CLIENTS}; do
    run_one warmup "${clients}" 0 "${WARMUP}"
    for repeat in $(seq 1 "${REPEATS}"); do
        run_one measure "${clients}" "${repeat}" "${DURATION}"
    done
done

# Recovery check after the maximum-concurrency point.
run_one recovery "${RECOVERY_CLIENTS}" 1 "${DURATION}"

sha256sum "${SERVER_BIN}" "${CLIENT_BIN}" >"${OUT}/binary_sha256.txt"
git -c "safe.directory=${ROOT}" rev-parse HEAD >"${OUT}/commit.txt"
git -C "${ROOT}" status --short >"${OUT}/git_status_short.txt"
lscpu >"${OUT}/lscpu.txt"
redis-cli -h "${HOST}" -p "${PORT}" INFO mako >"${OUT}/final_info.txt" 2>&1 || true
printf 'workers=%s\nserver_cpus=%s\nclient_cpus=%s\nclients=%s\npipeline=%s\nkeys=%s\nvalue_size=%s\nread_percent=%s\nwarmup=%s\nduration=%s\nrepeats=%s\nrecovery_clients=%s\n' \
    "${WORKERS}" "${SERVER_CPUS}" "${CLIENT_CPUS}" "${CLIENTS}" "${PIPELINE}" \
    "${KEYS}" "${VALUE_SIZE}" "${READ_PERCENT}" "${WARMUP}" "${DURATION}" \
    "${REPEATS}" "${RECOVERY_CLIENTS}" >"${OUT}/experiment_config.txt"

echo "${OUT}"
