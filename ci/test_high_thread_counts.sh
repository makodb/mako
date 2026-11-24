#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
for workers in 1 2 4 8 16 32 64; do
    python3 "$root/examples/run_native.py" transactions --shards 2 --workers "$workers" --count 10 "$@"
done
