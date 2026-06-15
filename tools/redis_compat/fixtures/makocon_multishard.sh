#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LOCAL_FIXTURE="${ROOT_DIR}/tools/redis_compat/fixtures/makocon_local.sh"

export MAKO_NUM_SHARDS="${MAKO_NUM_SHARDS:-3}"
export MAKO_LOCAL_SHARDS="${MAKO_LOCAL_SHARDS:-0,1,2}"
export MAKO_SHARD_INDEX="${MAKO_SHARD_INDEX:-0}"
export MAKO_REDIS_THREADS="${MAKO_REDIS_THREADS:-1}"
export MAKOCON_LOG="${MAKOCON_LOG:-/tmp/makoCon-g2-multishard.log}"
export MAKOCON_PIDFILE="${MAKOCON_PIDFILE:-/tmp/makoCon-g2-multishard.pid}"
export MAKOCON_STDIN_FIFO="${MAKOCON_STDIN_FIFO:-/tmp/makoCon-g2-multishard.stdin}"
export MAKOCON_STDIN_PIDFILE="${MAKOCON_STDIN_PIDFILE:-/tmp/makoCon-g2-multishard.stdin.pid}"

case "${1:-}" in
    start|stop|restart|status)
        bash "${LOCAL_FIXTURE}" "$1"
        ;;
    *)
        echo "usage: $0 start|stop|restart|status"
        exit 2
        ;;
esac
