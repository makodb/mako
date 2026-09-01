#!/usr/bin/env bash
set -u

HOST="${MAKO_HOST:-127.0.0.1}"
PORT="${MAKO_PORT:-6380}"
DURATION="${SOAK_SECONDS:-10}"
DEADLINE=$((SECONDS + DURATION))
ops=0

if ! command -v redis-cli >/dev/null 2>&1; then
    echo "missing redis-cli"
    exit 78
fi

while [[ "${SECONDS}" -lt "${DEADLINE}" ]]; do
    key="soak:$ops"
    redis-cli -h "${HOST}" -p "${PORT}" SET "${key}" "${ops}" >/dev/null || {
        echo "soak SET failed at op=${ops}"
        exit 1
    }
    value="$(redis-cli -h "${HOST}" -p "${PORT}" GET "${key}")" || {
        echo "soak GET failed at op=${ops}"
        exit 1
    }
    if [[ "${value}" != "${ops}" ]]; then
        echo "soak mismatch key=${key} value=${value}"
        exit 1
    fi
    ops=$((ops + 1))
done

pid="$(pgrep -f "^.*makoCon$" | head -n 1 || true)"
if [[ -n "${pid}" && -r "/proc/${pid}/status" ]]; then
    rss="$(awk '/VmRSS:/ {print $2 $3}' "/proc/${pid}/status")"
    fds="$(find "/proc/${pid}/fd" -maxdepth 1 -type l 2>/dev/null | wc -l)"
    threads="$(awk '/Threads:/ {print $2}' "/proc/${pid}/status")"
    echo "soak completed ops=${ops} rss=${rss:-unknown} fds=${fds} threads=${threads:-unknown}"
else
    echo "soak completed ops=${ops}"
fi
