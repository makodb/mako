#!/usr/bin/env bash
# Native Raft runtime regression companion to the Verus proof gates.
# tier1.sh PHASE [rust|rustlab|rustdisk]...
set -euo pipefail
phase="${1:?usage: tier1.sh PHASE [rust|rustlab|rustdisk]...}"
shift
: "${RESULTS:?RESULTS must name the evidence directory}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$root"
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run the native regression gates inside the build container.' >&2
    exit 1
fi
out="$RESULTS/$phase/tier1"
mkdir -p "$out"
lanes=("$@")
[[ ${#lanes[@]} -gt 0 ]] || lanes=(rust)
status=0
for lane in "${lanes[@]}"; do
    case "$lane" in
        rustlab)
            cargo test --locked --manifest-path crates/Cargo.toml -p raft-host \
                --test protocol -- --test-threads=1 >"$out/$lane.log" 2>&1 || status=1
            ;;
        rust|rustdisk)
            # Native runtime coverage includes both volatile transactions and
            # actual disk-backed replay/checkpoint/SIGKILL recovery. There is
            # no global C++ disk-mode build flag or process-name cleanup.
            (bash ci/native_rust.sh runtime && bash ci/native_rust.sh workloads) \
                >"$out/$lane.log" 2>&1 || status=1
            ;;
        *) echo "unknown lane: $lane" >&2; exit 2 ;;
    esac
done
echo "native Raft regression status=$status; evidence: $out"
exit "$status"
