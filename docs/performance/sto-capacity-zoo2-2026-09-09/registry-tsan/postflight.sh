#!/usr/bin/env bash
set -euxo pipefail
date -u
sha256sum /home/users/shuai/.cache/mako-registry-tsan-evidence-20260909.5zwu63/run.sh /home/users/shuai/.cache/mako-registry-tsan-evidence-20260909.5zwu63/run.log
sha256sum /home/users/shuai/.linuxbrew/Cellar/llvm/22.1.8/lib/clang/22/lib/linux/libclang_rt.tsan-x86_64.a
sha256sum /dev/shm/mako-registry-tsan-20260909.BUDV4D/x86_64-unknown-linux-gnu/debug/build/sto-masstree/4a0625f869f902f3/out/sto_masstree-4a0625f869f902f3
nm -g /dev/shm/mako-registry-tsan-20260909.BUDV4D/x86_64-unknown-linux-gnu/debug/build/sto-masstree/4a0625f869f902f3/out/sto_masstree-4a0625f869f902f3 | rg ' (__tsan_init|__tsan_read8|__tsan_write8)$'
for fingerprint in \
  /dev/shm/mako-registry-tsan-20260909.BUDV4D/x86_64-unknown-linux-gnu/debug/build/core/0c6329e691bc1688/fingerprint/lib-core.json \
  /dev/shm/mako-registry-tsan-20260909.BUDV4D/x86_64-unknown-linux-gnu/debug/build/std/69d8e721cf5bd073/fingerprint/lib-std.json \
  /dev/shm/mako-registry-tsan-20260909.BUDV4D/x86_64-unknown-linux-gnu/debug/build/sto-core/f56b3a0163841d5f/fingerprint/lib-sto_core.json \
  /dev/shm/mako-registry-tsan-20260909.BUDV4D/x86_64-unknown-linux-gnu/debug/build/sto-masstree/4a0625f869f902f3/fingerprint/test-lib-sto_masstree.json
do
  cp --no-clobber "$fingerprint" /home/users/shuai/.cache/mako-registry-tsan-evidence-20260909.5zwu63/
  rg '"rustflags"' "$fingerprint"
done
sha256sum /home/users/shuai/mako/.claude/worktrees/sto-rust/crates/sto-masstree/src/lib.rs /home/users/shuai/mako/.claude/worktrees/sto-rust/crates/sto-masstree/src/registry_growth_tests.rs
