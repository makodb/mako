# Checked private resolved-cache candidate

This directory supersedes the unchecked cache experiment for exploratory timing. Production sources and the original frozen private binaries remain unchanged.

## Health contract

The existing `put_resolved_with_previous_presence` API intentionally does not admit against native singleton health. It accesses an already-resolved Rust-owned record, preserving row observation, read-your-writes, and OCC semantics. It does not preserve every check performed by ordinary native directory lookup. Its behavior is unchanged here.

The new `put_resolved_with_previous_presence_checked` API queries native health inside core's `with_item` callback before resolving or staging the record. `Tree::runtime_health` uses the existing owned runtime and existing health ABI. It adds no Arc clone, native ABI symbol, mutex, structural guard, or RCU scope. The table marks AccessError::Poisoned or boundary faults, then core's existing callback failure handling poisons that core runtime. A worker-local quarantine alone would not suffice.

This is health admission, not cancellation of operations that overlap a later native failure. The safe engine facade already supplies worker affinity, live table/runtime ownership, immutable key limits, and exclusive access to its native Worker. No new native index access takes place on a hit.

The test-only `masstree/test-runtime-health` feature overrides Tree::runtime_health on one thread. It is enabled only through private engine test-hooks and is absent from release benchmark builds. Runtime::health always reads the actual native flag. Its result injection verifies routing but is not evidence of actual native poisoning.

## Actual native-state proof

The independent proof at `/var/tmp/mako-native-health-proof.SMAbQK` compiles a separate private TU containing the actual production mtree_abi.cc. A macro-guarded helper invokes the existing `poison(&singleton_runtime())` Release transition. It does not overwrite opaque layouts, introduce reset/recovery, or modify production archives. The proof runs in fresh exact subprocesses.

Eight final subprocess cases passed their expected conditions:

- Corrected active/fresh warm hits reject actual native poison.
- Corrected active/fresh ordinary misses reject actual native poison.
- Restoring only the old unchecked hit method allows active/fresh transactions to commit with clean cleanup, demonstrating the original bug.
- Ordinary misses in that negative-control copy still reject.

Two databases and a second native worker are present before poison, and both core runtimes remain Healthy before error mapping. This proves the native-before-Rust-propagation interval and the shared-native/separate-core case. It injects the real state transition, not a reproduced arbitrary C++ exception. No healthy-fixture teardown or reset runs in poisoned children.

The report is `proof-report.json`, SHA256 `d41f7c6d08411d742205ac2e80955ebfdb344886a52bea3c3d8f90e246e6c68b`. Its108 scoped source/native input hashes stayed unchanged during final runs. Release binaries below link only the unchanged production archives; neither the native proof helper nor the Rust status-override symbols appear in them.

## Executed functional checks

- Six resolving/table tests passed, including registry/direct token modes and healthy ordered/local policies, real TableShared poison, injected directory-health poison, stale conflicts, abort/delete/reinsert, and scan-publication accounting.
- The cache engine library passed25 tests, with one intentional ignored LSan negative control. The combined arm passed26, adding the prefetch state-preservation test. Coverage includes a genuine other-worker core-cleanup panic, exact Tree-health result injection, scans over warm tombstones, new DB epochs, mixed ordered/local writes, and warmed fused ACK true+forget/true+Drop behavior.
- `smoke.py` passed96 local functional cases and18 pre-open rejections across all three arms. These disable perf and are not performance evidence.

No new Miri or sanitizer qualification is claimed for this private candidate. It requires normal release qualification before any production promotion.

## Frozen binaries

All builds explicitly pin Rust1.95.0, compiler59807616e1fa2540724bfbac14d7976d7e4a3860, non-LTO, znver1, frame pointers, and debug info1. The common benchmark source and workload module are identical in all three arms and unchanged from the prior experiment. The baseline binary is byte-identical to the prior baseline.

| Arm | SHA256 | Text bytes | Data bytes | BSS bytes |
| --- | --- | ---: | ---: | ---: |
| base | 8627776b3339ddd0dd3320c9dbaf44ca9922bed1befb5876187c81e4fc3be1f6 | 1131487 | 84512 | 366147 |
| cache | 637c975faa81e9e385b0d1672ac50851f99fcdf389b08dad2fc6729fd63f4f29 | 1154151 | 84664 | 363803 |
| cache-prefetch | 86224e3f274e351006eb23f393f6c09b5057b4b88b0a54895fe1468c3d497fc6 | 1154591 | 84672 | 367667 |

Paths are `target-ARM-rust195/release/engine_compare`. `base-to-checked-cache.patch` contains the complete baseline-to-cache source delta, including the safe Tree health getter, checked Table callback, and test-only proof module. `base-to-checked-cache-prefetch.patch` contains the complete combined delta. `unchecked-to-checked-cache.patch` isolates the health correction and its tests from the rejected unchecked candidate. `cache-prefetch.patch` adds only the previously tested hint to the corrected cache. The obsolete empty `checked-health.patch` was an export error, not the implementation record, and is removed. `build.sh` uses the production archives; the independent `run_proof.sh` is the only runner linking the private native proof archive.

The linked production native archives at build time had SHA256 `6ae5e5cce81031cbd9daefe9a3bb907fae5fa96e2bac42ed91af314bc2ee0209` for libmako.a and `d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613` for libmasstree.a. The parent subsequently announced a metadata/fingerprint relink. These hashes describe the frozen binaries' build inputs, not the changing shared build directory. No rebuild is part of this provenance correction.

The cache remains512 direct-mapped64-byte entries, with32KiB of logical Vec capacity excluding allocator overhead. It caches exact keys up to32 bytes and opaque record identity only. Collisions can make warmed hot256 a substantially lower-hit workload than its cardinality suggests. Larger cardinality, streaming, mixed tables, and long-key bypass cases remain required measurements.
