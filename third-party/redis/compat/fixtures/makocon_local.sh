#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
MAKOCON_BIN="${MAKOCON_BIN:-/tmp/mako-fork-pr-build/makoCon}"
MAKO_HOST="${MAKO_HOST:-127.0.0.1}"
MAKO_PORT="${MAKO_PORT:-6380}"
MAKO_REDIS_THREADS="${MAKO_REDIS_THREADS:-1}"
MAKOCON_LOG="${MAKOCON_LOG:-/tmp/makoCon-redis-compat.log}"
MAKOCON_PIDFILE="${MAKOCON_PIDFILE:-/tmp/makoCon-redis-compat.pid}"
MAKOCON_STDIN_FIFO="${MAKOCON_STDIN_FIFO:-/tmp/makoCon-redis-compat.stdin}"
MAKOCON_STDIN_PIDFILE="${MAKOCON_STDIN_PIDFILE:-/tmp/makoCon-redis-compat.stdin.pid}"
RUNTIME_LIBS="${MAKOCON_LD_LIBRARY_PATH:-/tmp/llvm-21.1.6/lib/x86_64-unknown-linux-gnu:/tmp/mako-fork-pr-build:/tmp/mako-runtime-libs}"

is_running() {
    [[ -f "${MAKOCON_PIDFILE}" ]] && kill -0 "$(cat "${MAKOCON_PIDFILE}")" >/dev/null 2>&1
}

wait_ready() {
    local deadline=$((SECONDS + ${MAKOCON_START_TIMEOUT_S:-10}))
    while (( SECONDS < deadline )); do
        if ! is_running; then
            echo "makoCon exited before becoming ready"
            return 1
        fi
        if command -v redis-cli >/dev/null 2>&1 &&
            redis-cli -h "${MAKO_HOST}" -p "${MAKO_PORT}" PING >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    echo "makoCon did not become ready at ${MAKO_HOST}:${MAKO_PORT}"
    return 1
}

start_server() {
    if is_running; then
        echo "makoCon already running pid=$(cat "${MAKOCON_PIDFILE}")"
        exit 0
    fi
    if [[ ! -x "${MAKOCON_BIN}" ]]; then
        echo "missing executable ${MAKOCON_BIN}"
        exit 78
    fi
    mkdir -p "$(dirname "${MAKOCON_LOG}")"
    (
        cd "${ROOT_DIR}"
        rm -f "${MAKOCON_STDIN_FIFO}"
        mkfifo "${MAKOCON_STDIN_FIFO}"
        tail -f /dev/null >"${MAKOCON_STDIN_FIFO}" &
        echo "$!" >"${MAKOCON_STDIN_PIDFILE}"
        env MAKO_REDIS_THREADS="${MAKO_REDIS_THREADS}" MAKO_PORT="${MAKO_PORT}" \
            LD_LIBRARY_PATH="${RUNTIME_LIBS}:${LD_LIBRARY_PATH:-}" \
            "${MAKOCON_BIN}" <"${MAKOCON_STDIN_FIFO}" >"${MAKOCON_LOG}" 2>&1 &
        echo "$!" >"${MAKOCON_PIDFILE}"
    )
    wait_ready
    echo "makoCon started pid=$(cat "${MAKOCON_PIDFILE}") port=${MAKO_PORT}"
}

stop_server() {
    if ! is_running; then
        rm -f "${MAKOCON_PIDFILE}"
        echo "makoCon not running"
        return 0
    fi
    local pid
    pid="$(cat "${MAKOCON_PIDFILE}")"
    kill "${pid}" >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do
        if ! kill -0 "${pid}" >/dev/null 2>&1; then
            if [[ -f "${MAKOCON_STDIN_PIDFILE}" ]]; then
                kill "$(cat "${MAKOCON_STDIN_PIDFILE}")" >/dev/null 2>&1 || true
            fi
            rm -f "${MAKOCON_PIDFILE}" "${MAKOCON_STDIN_PIDFILE}" "${MAKOCON_STDIN_FIFO}"
            echo "makoCon stopped"
            return 0
        fi
        sleep 0.2
    done
    kill -9 "${pid}" >/dev/null 2>&1 || true
    if [[ -f "${MAKOCON_STDIN_PIDFILE}" ]]; then
        kill "$(cat "${MAKOCON_STDIN_PIDFILE}")" >/dev/null 2>&1 || true
    fi
    rm -f "${MAKOCON_PIDFILE}" "${MAKOCON_STDIN_PIDFILE}" "${MAKOCON_STDIN_FIFO}"
    echo "makoCon killed"
}

case "${1:-}" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    restart)
        stop_server
        start_server
        ;;
    status)
        if is_running; then
            echo "makoCon running pid=$(cat "${MAKOCON_PIDFILE}")"
        else
            echo "makoCon not running"
            exit 1
        fi
        ;;
    *)
        echo "usage: $0 start|stop|restart|status"
        exit 2
        ;;
esac
