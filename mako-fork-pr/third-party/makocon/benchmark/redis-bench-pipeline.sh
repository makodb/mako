#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <port> <-p|-g|-s>"
  echo "  -p: Preload keys"
  echo "  -g: GET benchmark"
  echo "  -s: SET benchmark"
  exit 1
}

if [ $# -ne 2 ]; then
  usage
fi

PORT="$1"
MODE="$2"
HOST="127.0.0.1"

KEYS=20000000      # 10M keyspace
VAL_SIZE=8         # 8-byte values
CONCURRENCY=128     # parallel clients
PIPELINE=20000        # pipeline depth
REQUESTS=20000000  # 10M ops per workload

case "$MODE" in
  -p)
    echo "== Preload: ${KEYS} keys with ${VAL_SIZE}-byte values on ${HOST}:${PORT} (pipeline=${PIPELINE}) =="
    redis-benchmark \
      -h "$HOST" -p "$PORT" \
      -t set \
      -n "$KEYS" \
      -c "$CONCURRENCY" \
      -P "$PIPELINE" \
      -d "$VAL_SIZE" \
      -r "$KEYS" \
    | grep "summary"
    ;;
  -g)
    echo "== GET workload: ${REQUESTS} uniform GETs over ${KEYS}-key space (pipeline=${PIPELINE}) =="
    redis-benchmark \
      -h "$HOST" -p "$PORT" \
      -t get \
      -n "$REQUESTS" \
      -c "$CONCURRENCY" \
      -P "$PIPELINE" \
      -d "$VAL_SIZE" \
      -r "$KEYS" \
    | grep "summary"
    ;;
  -s)
    echo "== SET workload: ${REQUESTS} uniform SETs over ${KEYS}-key space (pipeline=${PIPELINE}) =="
    redis-benchmark \
      -h "$HOST" -p "$PORT" \
      -t set \
      -n "$REQUESTS" \
      -c "$CONCURRENCY" \
      -P "$PIPELINE" \
      -d "$VAL_SIZE" \
      -r "$KEYS" \
    | grep "summary"
    ;;
  *)
    usage
    ;;
esac