#!/usr/bin/env bash
# chmod +x redis-bench.sh
# ./redis-bench.sh 6378
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <port>"
  exit 1
fi

PORT="$1"
HOST="127.0.0.1"

KEYS=100000      # 1M keyspace
VAL_SIZE=8         # 8-byte values (Masstree-style)
CONCURRENCY=8    # number of client connections
REQUESTS=1000000  # 1M ops per workload

echo "== Preload: ${KEYS} keys with ${VAL_SIZE}-byte values on ${HOST}:${PORT} =="
redis-benchmark \
  -h "$HOST" -p "$PORT" \
  -t set \
  -n "$KEYS" \
  -c "$CONCURRENCY" \
  -d "$VAL_SIZE" \
  -r "$KEYS" \
| grep "summary"

echo
echo "== GET workload: ${REQUESTS} uniform GETs over ${KEYS}-key space =="
redis-benchmark \
  -h "$HOST" -p "$PORT" \
  -t get \
  -n "$REQUESTS" \
  -c "$CONCURRENCY" \
  -d "$VAL_SIZE" \
  -r "$KEYS" \
| grep "summary"

# echo
# echo "== SET workload: ${REQUESTS} uniform SETs over ${KEYS}-key space =="
# redis-benchmark \
#   -h "$HOST" -p "$PORT" \
#   -t set \
#   -n "$REQUESTS" \
#   -c "$CONCURRENCY" \
#   -d "$VAL_SIZE" \
#   -r "$KEYS" \
# | grep "summary"
