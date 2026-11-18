# Private reader object-layout control

The control restores only the original initialized `CommitFence::read_only_serial: Mutex<()>` field. The reader algorithm never locks, reads or otherwise uses it. The original name, type, declaration order and `Mutex::new(())` cold construction are retained. A field-specific dead-code annotation documents that its nonuse is intentional. No scan bound, hint, padding array, `repr` change, public API or native change was added.

The candidate source is `/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-reader-layout-matched-crates.lFTsaC`. It contains no probe code. It was copied from the frozen reader root; `reader-to-matched.patch` is the only production-code difference and passes `git apply --check` against that root. The baseline and reader frozen roots remained unchanged. The same `mako-cache-bench` source and exact final-state/fresh-recovery verifier appear in all arms.

## Actual layout

Separate private copies added the same safe `size_of`, `align_of` and `offset_of` test. Six release test builds used Rust 1.95.0 and the original Zen 1/non-LTO/one-codegen-unit policy. All probes passed. RocksBlobs and MemBlobs instantiations gave identical offsets within each engine. The matched candidate equals baseline for every probed field.

| Type or field | C ABI base/matched | C ABI reader | STO base/matched | STO reader |
| --- | ---: | ---: | ---: | ---: |
| Cache size | 136 | 128 | 256 | 248 |
| Cache alignment | 8 | 8 | 8 | 8 |
| local offset | 16 | 16 | 16 | 16 |
| writeback offset | 40 | 40 | 160 | 160 |
| commit_fence offset | 48 | 48 | 168 | 168 |
| runtime offset | 72 | 64 | 192 | 184 |
| record_checksum offset | 128 | 120 | 248 | 240 |
| foreground_mode offset | 132 | 124 | 252 | 244 |
| backend_stall_threshold offset | 0 | 0 | 0 | 0 |
| single_producer_claimed offset | 133 | 125 | 253 | 245 |

CommitFence size is 24 bytes in baseline/matched and 16 in reader. Its alignment is 8 and writers offset is 0 throughout. The original mutex is at offset 16. Each independently allocated writer slot remains size/alignment 64.

These are object-layout controls, not identical text-layout or allocation-address controls. Rust does not promise `repr(Rust)` field order across arbitrary builds; the executed probes establish these layouts for this compiler and source. Main stores the cache in `Arc<MakoDb>`, which adds ownership-header storage. On 64-bit, the C ABI payload change from 136 to 128 corresponds to 152 versus 144 bytes with a 16-byte Arc header. It does not establish an allocator-size-class change. The separately allocated 460-slot writer array can also occupy different addresses and TLB entries in fresh processes.

## Executed checks and provenance

`run.sh` pins Rust 1.95.0, uses fresh per-variant/per-engine targets and the unchanged production native archives. Main binaries were built separately from the probe copies, so no private test code entered the timing source. No existing benchmark or target was overwritten.

- Both matched engines passed the four commit-fence component tests.
- Both passed `tests::two_readers_drain_one_pinned_writer_and_reject_unknown_outcome`, which drains the real pending-writer fence and then checks the real cache unknown-outcome path. It is not a native panic-injection test.
- `verify.py` checked all six layout results and exact baseline/matched equality. It ran one local W1 read sample per engine with 256 warmup and 2048 measured transactions, followed by a fresh recovery process. The unchanged main parser/verifier accepted exact engine identity, schema, values, counts, bytes and zero conflicts. These are functional smoke runs, not host-controlled performance measurements. Their small RocksDB scratch directories are retained.
- Native archive hashes remained `53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402` for libmako and `d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613` for libmasstree.

The first four probe builds linked successfully but their test launches failed before execution because this new runner omitted the RocksDB directory from `LD_LIBRARY_PATH`. Those original logs remain. The runner was corrected, and the exact already-built binaries then passed in the separate `*-executed.log` transcripts. The later two probes used the corrected environment directly. Existing copied-source warnings remain visible; no broad lint suppression was added.

The frozen timing binaries are:

| Binary | SHA256 | text/data/BSS bytes |
| --- | --- | --- |
| `main-matched-c-abi` | `7b081996ccbb854a8a2af020fe8d94db9f955fb880b609da9199f552a1bb820a` | 3345073/71512/540840 |
| `main-matched-sto` | `a80107e9a0d871b9b3628d86b6db9f140748a9e3e7424732e56ada5673abfe66` | 3608627/119592/511947 |

`verification.json`, SHA256 `ef3d5c95668362f9ee1d755f43131cc7b3413040e8a98e060fd072da35926bc7`, records all 254 timing-source file hashes, the native hashes, compiler identity, full layout values and functional results. `reader-to-matched.patch` has SHA256 `d9492743a9d56e60dd16ecbc59d16f7df17a2149759f313c8405deaf1a5ca8cf`. The layout test exists only in the three explicitly named probe copies, not the candidate source.

The performance agent owns the approved W1-only 18-cell baseline/reader/matched comparison. No measurement result or production promotion is claimed here. If the matched reader recovers W1 performance, object representation or associated code generation remains a plausible cause; it would not prove a specific false-sharing or allocator mechanism. If it does not, the original object layout alone does not account for the regression. Frequency and machine-code placement still require separate evidence.
