#!/usr/bin/env bash
set -euo pipefail
readonly cross_repo=/home/users/shuai/mako/.claude/worktrees/masstree-rocks
readonly cross_out=/var/tmp/mako-final-integration.EqWvlp
readonly cross_launcher=/var/tmp/mako-sto-integration.UdvOQE/cargo-native
env PYTHONPATH="${cross_repo}/scripts" python3 -m unittest test_mako_cache_cross_engine_reopen
for cross_engine in c-abi sto; do
    cross_target="${cross_repo}/build_c22/cargo-target-mako-cache-bench-${cross_engine}"
    bash "${cross_launcher}" --run env CARGO_TARGET_DIR="${cross_target}" \
        cargo build --locked -p mako-cache-bench --bin mako-cache-bench \
        --no-default-features --features "engine-${cross_engine}"
    bash "${cross_launcher}" --run env CARGO_TARGET_DIR="${cross_target}" \
        cargo test --locked -p mako-cache-bench --bin mako-cache-bench \
        --no-default-features --features "engine-${cross_engine}" -- --test-threads=1
done
bash "${cross_launcher}" --run python3 "${cross_repo}/scripts/mako_cache_cross_engine_reopen.py" \
    --binary "c-abi=${cross_repo}/build_c22/cargo-target-mako-cache-bench-c-abi/debug/mako-cache-bench" \
    --binary "sto=${cross_repo}/build_c22/cargo-target-mako-cache-bench-sto/debug/mako-cache-bench" \
    --data-root "${cross_out}/cross-data" \
    --report "${cross_out}/qualified-cross-report.json"
