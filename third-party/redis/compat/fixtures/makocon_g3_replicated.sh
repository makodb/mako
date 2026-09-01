#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
MAKOCON_BIN="${MAKOCON_BIN:-/tmp/mako-fork-pr-build/makoCon}"
STATE_DIR="${MAKO_G3_STATE_DIR:-/tmp/makoCon-g3-replicated}"
RUNTIME_LIBS="${MAKOCON_LD_LIBRARY_PATH:-/tmp/llvm-21.1.6/lib/x86_64-unknown-linux-gnu:/tmp/mako-fork-pr-build:/tmp/mako-runtime-libs}"
SHARD_CONFIG="${MAKO_SHARD_CONFIG:-${ROOT_DIR}/src/mako/config/local-shards1-warehouses1.yml}"
REPLICATION_CONFIG="${MAKO_REPLICATION_CONFIG:-${ROOT_DIR}/third-party/redis/compat/fixtures/makocon_g3_paxos.yml}"
OCC_CONFIG="${MAKO_OCC_CONFIG:-${ROOT_DIR}/config/occ_paxos.yml}"

mkdir -p "${STATE_DIR}"

role_port() {
    case "$1" in
        localhost) echo 6390 ;;
        p1) echo 6391 ;;
        p2) echo 6392 ;;
        learner) echo 6393 ;;
    esac
}

is_running() {
    local pidfile="${STATE_DIR}/$1.pid"
    [[ -f "${pidfile}" ]] && kill -0 "$(cat "${pidfile}")" >/dev/null 2>&1
}

start_role() {
    local role="$1"
    local port
    port="$(role_port "${role}")"
    if is_running "${role}"; then
        return
    fi
    (
        cd "${ROOT_DIR}"
        nohup setsid env \
            LD_LIBRARY_PATH="${RUNTIME_LIBS}:${LD_LIBRARY_PATH:-}" \
            MAKO_HOST=127.0.0.1 \
            MAKO_PORT="${port}" \
            MAKO_REDIS_THREADS=1 \
            MAKO_SHARD_CONFIG="${SHARD_CONFIG}" \
            MAKO_REPLICATION_ENABLED=1 \
            MAKO_REPLICATION_CONFIG="${REPLICATION_CONFIG}" \
            MAKO_OCC_CONFIG="${OCC_CONFIG}" \
            MAKO_PAXOS_PROC_NAME="${role}" \
            "${MAKOCON_BIN}" </dev/null >"${STATE_DIR}/${role}.log" 2>&1 &
        echo "$!" >"${STATE_DIR}/${role}.pid"
    )
}

wait_processes() {
    local deadline=$((SECONDS + ${MAKO_G3_START_TIMEOUT_S:-30}))
    while (( SECONDS < deadline )); do
        local ready=1
        for role in localhost p1 p2 learner; do
            if ! is_running "${role}"; then
                ready=0
            fi
        done
        if [[ "${ready}" -eq 1 ]]; then
            return
        fi
        sleep 0.2
    done
    echo "replicated makoCon process exited during startup" >&2
    return 1
}

wait_redis() {
    local role="$1"
    local deadline=$((SECONDS + ${MAKO_G3_START_TIMEOUT_S:-30}))
    local port
    port="$(role_port "${role}")"
    while (( SECONDS < deadline )); do
        if is_running "${role}" && redis-cli -h 127.0.0.1 -p "${port}" PING >/dev/null 2>&1; then
            return
        fi
        sleep 0.2
    done
    echo "${role} did not become ready on port ${port}" >&2
    return 1
}

wait_promoted_learner() {
    local deadline=$((SECONDS + ${MAKO_G3_RECOVER_TIMEOUT_S:-15}))
    while (( SECONDS < deadline )); do
        local response
        response="$(redis-cli -h 127.0.0.1 -p 6393 --raw SET g3:promotion:probe ready 2>/dev/null || true)"
        if [[ "${response}" == "OK" ]] &&
            [[ "$(redis-cli -h 127.0.0.1 -p 6393 --raw GET g3:promotion:probe 2>/dev/null)" == "ready" ]]; then
            redis-cli -h 127.0.0.1 -p 6393 DEL g3:promotion:probe >/dev/null 2>&1 || true
            return
        fi
        sleep 0.2
    done
    echo "replicated makoCon learner was not promoted" >&2
    return 1
}

stop_role() {
    local role="$1"
    local pidfile="${STATE_DIR}/${role}.pid"
    if is_running "${role}"; then
        kill -9 "$(cat "${pidfile}")" >/dev/null 2>&1 || true
    fi
    rm -f "${pidfile}"
}

case "${1:-}" in
    start)
        if [[ ! -x "${MAKOCON_BIN}" ]]; then
            echo "missing executable ${MAKOCON_BIN}" >&2
            exit 78
        fi
        start_role localhost
        start_role learner
        start_role p2
        start_role p1
        wait_processes
        wait_redis localhost
        ;;
    kill)
        stop_role localhost
        ;;
    recover)
        wait_promoted_learner
        ;;
    stop)
        stop_role localhost
        stop_role p1
        stop_role p2
        stop_role learner
        ;;
    status)
        for role in localhost p1 p2 learner; do
            if is_running "${role}"; then
                echo "${role} running pid=$(cat "${STATE_DIR}/${role}.pid") port=$(role_port "${role}")"
            else
                echo "${role} stopped"
            fi
        done
        ;;
    *)
        echo "usage: $0 start|kill|recover|stop|status" >&2
        exit 2
        ;;
esac
