#!/usr/bin/env bash
# Container-only, bounded correctness check. Plain compilation never calls it.
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run ./docker_build.sh ci upper' >&2
    exit 1
fi
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec python3 scripts/upper_check.py "$@"
