# Bounded-reader main benchmark handoff

Both private binaries are frozen. This handoff contains functional and build evidence, not a performance result or a production promotion.

The source root is `/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-read-bound-crates`. Relative to the frozen no-mutex reader, runtime code adds one helper that executes the packed-order cut, then loads `NEXT_COMMIT_FENCE_THREAD_SLOT` with Acquire. The existing scan uses that registered prefix. There is no inert mutex field, added prefetch, changed writer registration, shorter wait, new public API or native change. The main benchmark source and its exact state/recovery verifier are unchanged.

## Independent source review

I read the complete baseline-to-bound and reader-to-bound diffs. The only other source changes are test fixtures using real registered slot IDs, a deterministic call-order test, and formatting of the existing concurrent-reader Loom model.

The bound is safe under the existing registration and packed-order contract. Slot registration is monotone, nonrecycling, and checked against the fixed writer-array capacity. A prefix writer registers before its Release publication in packed order. The reader acquires that publication or its release sequence before loading the registration count. Therefore the count cannot omit that prefix writer. A writer outside the cut can still enter later, with the same engine validation argument as the full scan. The code retains each scanned slot's Acquire load, exact-generation drain and subsequent unknown-outcome check. It does not clamp an oversized count or hide an invariant failure.

The root and validation agent own the full library tests, weak-memory models and before-cut negative control. Those are separate evidence. This handoff does not relabel the deterministic joined-thread fixture as weak-memory proof.

## Builds and input identity

`build-main.sh` pins Rust 1.95.0, LLVM 22.1.2, release optimization level 3, panic abort, one codegen unit, non-LTO and Zen 1. It uses the same frame-pointer, debug-info and linker settings as the original reader/layout trials. Each engine has a fresh target under this directory, `target-main-c-abi` or `target-main-sto`. No target from another variant or pre-existing binary was reused. No test-hook feature is enabled.

The C ABI build took 86 seconds and the STO build took 26 seconds. Full logs retain the copied source's existing warnings. Neither build required a source or linker correction.

`main-build-inputs.json`, SHA256 `535d2eddecca57ee30d6d865ae424fb466ce60cbc6c48e75777773dcbfa5a5d0`, records 254 source files plus one pre-existing Python bytecode file under `mako-cache/mutations/__pycache__`. Its snapshot covers 21 native/link inputs: all 17 declared static archives, the native build manifest, yaml-cpp, RocksDB and the private linker script. Each build verifies this exact snapshot before and after compilation, and the smoke script verifies it again before and after execution.

The principal unchanged native hashes are:

- libmako: `53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402`
- libmasstree: `d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613`
- RocksDB 8.9.1: `5e5c1b9f0adf5eef29131d8957089601b71ca9f2d9092831030bafc30e6861a6`

| Binary | SHA256 | text/data/BSS bytes |
| --- | --- | --- |
| `main-bound-c-abi` | `74cd8b407a8110a99877b0d6cf480c684d0b5148c2c7f50d7b701f4f1dbfa7b7` | 3344961/71544/540904 |
| `main-bound-sto` | `a2ff104c69ea9cb2dbaefd51bc24df2d0bfe2d0123de1762dc987a9d2f592e8d` | 3608779/119624/511755 |

## Executed smoke checks

`smoke-main.py` ran both engines with W1 and W4, 256 warmup and 2048 measured read transactions per worker. Each sample had a fresh database and a separate fresh recovery process. All eight child phases passed the unchanged exact main parser and verifier, including engine identity, schema, every expected value, counts, record/data byte accounting, drain/recovery and zero conflicts. Binary and verifier-helper hashes stayed unchanged. These local functional runs did not enforce the remote performance host's placement or frequency policy, so their elapsed rates are not measurement evidence.

`main-verification.json`, SHA256 `729c211db31232017da6c533eecd47486a069569cab7dda8fcf6a18f0744a0cd`, holds all four sample/recovery pairs and links to the frozen input manifest. Complete child stdout/stderr and small database scratch directories remain under `main-smoke-*`. No scratch data was deleted.

The exported reader-to-bound patches contain runtime/test source changes only. `reader-to-bound-lib.patch` has SHA256 `c25207458f136083e2365168e62fbbbee61cbc8abb9faa2fa4858baf595ebf31`; `reader-to-bound-loom.patch` has SHA256 `f11d05d6fa6f0727ae65b79fd14d2b5dfe89889bbb89516ccc82d605240c6bc4`. Production and the original reader/layout source copies remain unchanged by this build task.
