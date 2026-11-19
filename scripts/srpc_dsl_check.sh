#!/usr/bin/env bash
# Use the canonical-source census shipped with the SRPC subtree.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "${SCRIPT_DIR}/../src/srpc/scripts/srpc_dsl_check.sh" "$@"
