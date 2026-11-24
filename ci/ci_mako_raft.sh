#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
exec bash "$root/ci/ci.sh" "${@:-shard2ReplicationRaft}"
