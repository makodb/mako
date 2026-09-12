# Final capacity implementation ASan gate

The fresh v4 gate completed with exit status 0 on the final prepaid-segment
implementation. No implementation files changed during the run. The result
retains the gate's exact native-root and intentional-quarantine qualifications.

The command ran from `/home/users/shuai/mako/.claude/worktrees/sto-rust`:

```sh
env TMPDIR=/dev/shm \
  PKG_CONFIG=/usr/bin/pkg-config \
  PATH=/home/users/shuai/.cargo/bin:/usr/bin:/home/users/shuai/.linuxbrew/opt/llvm@22/bin:/bin \
  CC=/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang \
  CXX=/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang++ \
  CARGO_TARGET_DIR=/dev/shm/sto-capacity-asan-20260909-host-cargo \
  MAKO_SANITIZER_ALLOW_DIRTY=1 \
  MAKO_SANITIZER_BUILD_DIR=/dev/shm/sto-capacity-asan-20260909-v4 \
  MAKO_SANITIZER_JOBS=8 \
  bash scripts/ci/run_rust_sto_sanitizer.sh address
```

The build directory did not exist at launch. The isolated host Cargo target
was seeded by copying the existing host-tool cache. Instrumented first-party
Rust and native build outputs were produced in the fresh v4 tree. The earlier
v3 tree and its three manifests were not reused as the ASan build or modified.

The source base was `81aa134884219ad148d960c78decca12630648e3` on
`codex/sto-rust`, with the capacity changes present as uncommitted worktree
changes. The gate records `final_worktree=dirty-local-diagnostic-unchanged`;
the base commit alone does not reproduce the tested implementation.

The manifest records the four exact constructor-root suppression tables,
40 allocations/12,800 bytes total, RMW instrumentation checks, and
`gate_status=passed`. The copied CTest logs include all four qualified cases,
the remaining 14 unsuppressed Rust-labeled cases, and both RMW tests. Large
compile logs and binaries remain outside this evidence directory.

```text
scripts/ci/run_rust_sto_sanitizer.sh SHA-256
66bcbc6222e0360be82125adb422fc64b4aa23020c2348147b8b74faeb7448c3
rust-sto-sanitizer-manifest.txt SHA-256
97231e076237877c6b870713a025e129bb45d58ccf12054dbce3ee1f84df909f
/dev/shm/sto-capacity-asan-20260909-v4-driver.log SHA-256
1bfc97243fda1253d95f222dd6ff8b848e817746a295f583944efa35bf20e17a
```

The original v3 manifest hashes were rechecked after v4 completed:

```text
rust-sto-sanitizer-manifest.txt
3abd97cd6fbea5b8704215dc54d983ff5cc4776cb02d650c9be13990b21800b6
rust-sto-sanitizer-supplemental-policy-manifest.txt
3c01d55a5323ab809f664d683647b6f60aabe8f83d10a9c697660671e4c18661
rust-sto-sanitizer-supplemental-rmw-manifest.txt
64363bce857ec38ad0842f2fd279f8bd3a5907cf22d2a330af85fc4d6b23a984
```
