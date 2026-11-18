# Prefetch-only promotion preparation

Production has not been changed. The candidate patch is `prefetch-promotion.patch`, SHA256 `30d7ebf16c046fc5d990b8ef8dac917f941c96fc6b108a9452a0b0cef86afc9d`. It changes exactly five files with108 additions and passes `git apply --check` against the current production worktree.

The instruction, capability check, Clock accessor, restricted-hook placement, and cold initialization match the measured private prefetch candidate. The only differences are the deterministic test fixture, module documentation, and rustfmt's line wrapping. No resolved-key cache, borrowed staging, reader-fence change, or frame-boxing change is included. No Cargo feature, dependency, native ABI, CMake selector, or quarantine-list change is required.

## Safety and ordering review

- `mako-clock/src/lib.rs` retains `forbid(unsafe_code)`. The hidden safe accessor obtains the actual HLC field address, so the engine assumes no field offset or Clock layout. It grants no permission to dereference or modify that word.
- The engine keeps its Clock borrow alive through one non-faulting PREFETCHW hint. CPUID checks extended-leaf availability and PRFCHW before the instruction. The assembly uses no stack or flag changes and declares readonly memory effects. Unsupported architectures and Miri take a no-op path.
- The capability byte sits in its own128-byte-aligned object. It is initialized only while creating a new WorkerSlot, after the existing-slot return at `worker.rs:168`. Repeated transaction attachment does not repeat CPU detection. The hot hint performs one cached relaxed capability load.
- The hint is inside `Driver::reserve_upper_metadata`, after the writeset-locked notification and before validation. It runs only for RestrictedPair and RestrictedStampOnly. The ordinary unlogged commit has no hook. General RustSequence and NativeLocked orders fail the mode test. Generic core calls this hook only when has_writes; the direct lane admits writes and excludes predicates before it reaches this hook.
- Mako timestamp allocation remains in pre_install after local validation. The hint changes no timestamp, sequence, gate, record, holder, ownership, or failure report. Abort after the hint requires no new cleanup. Existing local-version exhaustion rejects during acquisition, before this hook.

This review relies on existing restricted-profile admission at `txn.rs:413`, core generic hook admission at `sto-core/src/transaction.rs:3529`, and direct-lane admission at `transaction.rs:3815`. It does not infer writes from a nonempty ordered commit ID.

## Executed checks on this exact prefetch-only source

`check.sh native` passed the exact state-preservation test using Rust1.95.0 and real native Masstree archives. The test host exposes `3dnowprefetch`; the runtime CPU guard remains enabled. `check.sh miri` passed all nine native-free engine tests using nightly-2026-08-12 with isolation and leak checking enabled. No MIRIFLAGS relaxation was used.

The fixture seeds a nonzero stamp with `observe_stamp(42)` instead of querying realtime. It checks zero and nonzero stamps, free and held gates, and the next dense sequences1,2,3. The prefetch itself is a no-op under Miri, so that run checks Rust ownership and the fixture, not hardware behavior.

Logs are `native.log` and `miri.log`. The native archives remained SHA256 `53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402` for libmako and `d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613` for libmasstree after the run. `source-sha256.txt` records the Rust/Cargo source snapshot. No full post-promotion sanitizer or cache qualification is claimed here.

## Existing tests and CI affected

The new test is `prefetch::tests::hint_does_not_allocate_a_stamp_or_change_the_gate`. It needs no test-hooks feature and no leak exception.

- `scripts/run_rust_sto_miri.sh:191` already runs the full engine library. Its native-free inventory increases from8 to9 tests for this patch alone.
- `scripts/ci/run_rust_sto_cache_tests.py:50` already runs the complete all-features engine library. Its listing/execution completeness check at173–197 automatically requires the new ordinary test. The existing exact poison reruns and lost-TouchLog LSan negative control stay unchanged. No sto-masstree selector needs expansion because the rejected cache methods are absent.
- CMake `run_mako_engine_sto_tests` at4247 already runs all engine features and targets, followed by the cache library/integration/doc suites. It discovers the new test without a target edit. The engine library gains one ordinary positive, not a new ignored test.
- Existing regression coverage to rerun includes `per_worker_holder_terminal_uses_the_restricted_and_general_profiles`, `general_record_terminal_serializes_the_canonical_write_set`, `optimistic_conflicts_abort_the_loser`, fused holder/pool ownership, all eight actual core-poison tests, timestamp persistence, and the cache process-crash matrices. These exercise the surrounding phase and cleanup protocol; the hint does not alter their contracts.

Before promotion, apply the reviewed patch only, record fresh source/build identities, and run the existing full native facade/cache gates plus focused ASan/TSan/UBSan and cross-engine reopen on that final source. Retain prior performance artifacts as historical measured-source evidence. Do not relabel them as a post-promotion build. C ABI remains the default engine.
