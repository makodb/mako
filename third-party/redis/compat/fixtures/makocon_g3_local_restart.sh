#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
LOCAL_FIXTURE="${ROOT_DIR}/third-party/redis/compat/fixtures/makocon_local.sh"

export MAKO_REDIS_THREADS="${MAKO_REDIS_THREADS:-1}"
export MAKOCON_LOG="${MAKOCON_LOG:-/tmp/makoCon-g3-restart.log}"
export MAKOCON_PIDFILE="${MAKOCON_PIDFILE:-/tmp/makoCon-g3-restart.pid}"
export MAKOCON_STDIN_FIFO="${MAKOCON_STDIN_FIFO:-/tmp/makoCon-g3-restart.stdin}"
export MAKOCON_STDIN_PIDFILE="${MAKOCON_STDIN_PIDFILE:-/tmp/makoCon-g3-restart.stdin.pid}"

case "${1:-}" in
    start)
        bash "${LOCAL_FIXTURE}" start
        ;;
    kill)
        if [[ -f "${MAKOCON_PIDFILE}" ]] && kill -0 "$(cat "${MAKOCON_PIDFILE}")" >/dev/null 2>&1; then
            kill -9 "$(cat "${MAKOCON_PIDFILE}")" >/dev/null 2>&1 || true
            rm -f "${MAKOCON_PIDFILE}"
        fi
        ;;
    recover)
        bash "${LOCAL_FIXTURE}" start
        ;;
    stop)
        bash "${LOCAL_FIXTURE}" stop
        ;;
    status)
        bash "${LOCAL_FIXTURE}" status
        ;;
    *)
        echo "usage: $0 start|kill|recover|stop|status"
        exit 2
        ;;
esac
