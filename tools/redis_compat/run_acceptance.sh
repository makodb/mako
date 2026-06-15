#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST="${MAKO_HOST:-127.0.0.1}"
PORT="${MAKO_PORT:-6380}"
MEMTIER="${MEMTIER_BIN:-/tmp/memtier-local/usr/bin/memtier_benchmark}"
OUT_DIR="${ROOT_DIR}/tools/redis_compat/acceptance"
COMMIT="$(git -c safe.directory="${ROOT_DIR}" -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
STAMP="$(date -u +%Y%m%d_%H%M%S)"
OUT_FILE="${OUT_DIR}/ACCEPTANCE_${STAMP}_${COMMIT}.txt"

mkdir -p "${OUT_DIR}"

line() {
    local name="$1"
    local status="$2"
    local detail="$3"
    printf '%-34s %-4s %s\n' "${name}" "${status}" "${detail}"
}

redis_ready() {
    command -v redis-cli >/dev/null 2>&1 &&
        redis-cli -h "${HOST}" -p "${PORT}" PING >/dev/null 2>&1
}

run_pytest_g1() {
    if ! redis_ready; then
        line "G1 wire compatibility" "N/A" "makoCon not reachable at ${HOST}:${PORT}"
        return
    fi
    local output
    if output="$(cd "${ROOT_DIR}" && python3 -m pytest tools/redis_compat -q 2>&1)"; then
        line "G1 wire compatibility" "PASS" "$(echo "${output}" | tail -n 1)"
    else
        line "G1 wire compatibility" "FAIL" "$(echo "${output}" | tail -n 1)"
    fi
}

run_optional_script() {
    local name="$1"
    local path="$2"
    shift 2
    if [[ ! -x "${ROOT_DIR}/${path}" ]]; then
        line "${name}" "N/A" "missing ${path}"
        return
    fi
    local output
    if output="$(cd "${ROOT_DIR}" && "$@" 2>&1)"; then
        line "${name}" "PASS" "$(echo "${output}" | tail -n 1)"
    else
        local status=$?
        if [[ "${status}" -eq 78 ]]; then
            line "${name}" "N/A" "$(echo "${output}" | tail -n 1)"
        else
            line "${name}" "FAIL" "$(echo "${output}" | tail -n 1)"
        fi
    fi
}

run_throughput_guard() {
    if ! redis_ready; then
        line "Throughput guard" "N/A" "makoCon not reachable at ${HOST}:${PORT}"
        return
    fi
    if ! command -v redis-benchmark >/dev/null 2>&1; then
        line "Throughput guard" "N/A" "redis-benchmark not installed"
        return
    fi
    redis-cli -h "${HOST}" -p "${PORT}" SET acceptance:get value >/dev/null 2>&1 || true
    local output
    if output="$(redis-benchmark -h "${HOST}" -p "${PORT}" -n 1000 -c 10 -q GET acceptance:get 2>&1)"; then
        line "Throughput guard" "PASS" "$(echo "${output}" | tr '\r' '\n' | sed '/^$/d' | tail -n 1)"
    else
        line "Throughput guard" "FAIL" "$(echo "${output}" | tr '\r' '\n' | sed '/^$/d' | tail -n 1)"
    fi
}

run_memtier_guard() {
    if ! redis_ready; then
        line "Memtier p99 guard" "N/A" "makoCon not reachable at ${HOST}:${PORT}"
        return
    fi
    if [[ ! -x "${MEMTIER}" ]]; then
        line "Memtier p99 guard" "N/A" "missing ${MEMTIER}"
        return
    fi
    redis-cli -h "${HOST}" -p "${PORT}" SET acceptance:get value >/dev/null 2>&1 || true
    local output
    if output="$("${MEMTIER}" --server="${HOST}" --port="${PORT}" --protocol=redis \
        --clients=10 --threads=1 --requests=50 --command='GET acceptance:get' 2>&1)"; then
        local summary
        summary="$(echo "${output}" | awk '/Totals/ {print $2 " ops/sec p99=" $6 " ms"}' | tail -n 1)"
        line "Memtier p99 guard" "PASS" "${summary:-completed}"
    else
        line "Memtier p99 guard" "FAIL" "$(echo "${output}" | tail -n 1)"
    fi
}

run_info_guard() {
    if ! redis_ready; then
        line "INFO metrics guard" "N/A" "makoCon not reachable at ${HOST}:${PORT}"
        return
    fi
    local info
    if ! info="$(redis-cli -h "${HOST}" -p "${PORT}" INFO 2>&1)"; then
        line "INFO metrics guard" "FAIL" "$(echo "${info}" | tail -n 1)"
        return
    fi
    if grep -q "connected_clients:" <<<"${info}" &&
        grep -q "total_connections_received:" <<<"${info}" &&
        grep -q "mako_txn_commits:" <<<"${info}"; then
        line "INFO metrics guard" "PASS" "clients/server/mako metrics present"
    else
        line "INFO metrics guard" "FAIL" "required scoped metrics missing"
    fi
}

run_soak_guard() {
    if ! redis_ready; then
        line "Soak guard" "N/A" "makoCon not reachable at ${HOST}:${PORT}"
        return
    fi
    run_optional_script "Soak guard" "tools/redis_compat/run_soak.sh" bash tools/redis_compat/run_soak.sh
}

run_resp_fuzz_guard() {
    if ! redis_ready; then
        line "RESP fuzz guard" "N/A" "makoCon not reachable at ${HOST}:${PORT}"
        return
    fi
    run_optional_script "RESP fuzz guard" "tools/redis_compat/run_fuzz.sh" bash tools/redis_compat/run_fuzz.sh
}

{
    run_pytest_g1
    run_optional_script "G2 bank transfer" "tools/redis_compat/run_bank_transfer.py" python3 tools/redis_compat/run_bank_transfer.py
    run_optional_script "G2 cross-shard demo" "tools/redis_compat/run_cross_shard_demo.py" python3 tools/redis_compat/run_cross_shard_demo.py
    run_optional_script "G3 failover durability" "tools/redis_compat/run_failover_durability.py" python3 tools/redis_compat/run_failover_durability.py
    run_optional_script "G4 Elle isolation" "tools/redis_compat/run_elle_isolation.py" python3 tools/redis_compat/run_elle_isolation.py
    run_throughput_guard
    run_memtier_guard
    run_optional_script "TCL semantic guard" "tools/redis_compat/run_tcl_suite.sh" bash tools/redis_compat/run_tcl_suite.sh
    run_info_guard
    run_soak_guard
    run_optional_script "Restart durability guard" "tools/redis_compat/run_restart_durability.py" python3 tools/redis_compat/run_restart_durability.py
    run_optional_script "Client failover guard" "tools/redis_compat/run_client_failover.py" python3 tools/redis_compat/run_client_failover.py
    run_resp_fuzz_guard
} | tee "${OUT_FILE}"

echo "Saved acceptance artifact: ${OUT_FILE}" >&2
