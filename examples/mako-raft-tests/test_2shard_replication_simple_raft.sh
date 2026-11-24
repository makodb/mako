#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
exec python3 "$root/examples/run_native.py" transactions --shards 2 --replicas 3 "$@"
