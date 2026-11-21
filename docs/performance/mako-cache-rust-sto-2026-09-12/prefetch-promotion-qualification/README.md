# Prefetch-only promotion qualification

These checks ran on the private preparation copy `/var/tmp/mako-prefetch-promotion.3eMW85`, before the parent applied the five-file patch to production. They are not a post-promotion build or a full final-source qualification. The five promoted files match the tested copy byte for byte at handoff. Production native/cache/sanitizer qualification remains a separate run.

The copied `REVIEW.md` describes the preparation state and therefore still says production was unchanged. That statement is historical. The parent subsequently promoted this exact patch. No resolved-key cache, borrowed staging, reader-fence change, or frame-boxing change is included.

## Executed checks

- `check.sh native` passed the exact state-preservation test with Rust 1.95.0, LLVM 22.1.2 and real native Masstree archives. The host advertises `3dnowprefetch`; the runtime feature guard remained enabled. This checks unchanged clock state on the supported native path, not the scheduling of a particular cache transfer.
- `check.sh miri` passed all nine native-free engine library tests with nightly-2026-08-12. Isolation and leak checking stayed enabled. The hardware hint is a no-op under Miri, so this checks Rust ownership and state preservation, not the assembly instruction.
- The deterministic fixture seeds the stamp with `observe_stamp(42)`. It checks zero and nonzero stamps, free and held gates, and the next dense sequences 1, 2 and 3. No realtime clock call or isolation exemption is needed.
- `git apply --check` passed before promotion. No Cargo, CMake, native ABI, feature selector or quarantine entry changed. The existing complete engine library lanes discover the new ordinary test automatically.

The exact commands are in `check.sh`; `native.log` and `miri.log` are complete successful transcripts. The script retains the original absolute scratch paths as provenance, not as a portable rerun instruction. `source-sha256.txt` records the private Rust/Cargo source snapshot. `prefetch-promotion.patch` is the applied five-file change.

## Input and promoted-file identities

Native archive hashes remained unchanged after the private native test:

- `libmako.a`: `53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402`
- `libmasstree.a`: `d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613`

The tested and newly promoted files had these identical SHA256 values:

| File under `crates/` | SHA256 |
| --- | --- |
| `mako-clock/src/lib.rs` | `9f3ceb505da3b440069cbaa0617c7599a556f14b0a655d3b9ae337edba94bb55` |
| `mako-engine-sto/src/lib.rs` | `14dc82abd37e682658ebae107202dea8a1ec72a9e5d2008252ad0e17ed4c58c3` |
| `mako-engine-sto/src/prefetch.rs` | `bcf24d7f0bab1ebb234e1c04111154b34dbedf65dbac6a66b1ee6e7b96892664` |
| `mako-engine-sto/src/txn.rs` | `b9519555d7aed8bbe4fa0762a44a47f91ab9f6d32c7f2ff3a95ea69071d501d1` |
| `mako-engine-sto/src/worker.rs` | `d9b2485f892aa1fcf7bc1a9b6055bb690cd58a6ddd0ba68e3cdab96ea72ba799` |

Recorded artifact hashes:

| Artifact | SHA256 |
| --- | --- |
| `native.log` | `8d52c5d5bc4cea7ebff12123dcc677bef4ed29f214a673db717a0b814c58a74e` |
| `miri.log` | `525ada8d5604f4b57040623bf3cf4003321bb6f8457cb9a63ba25ebf8da50443` |
| `source-sha256.txt` | `8953c14d8e4b536699a041f35d5ba83d21f93fa2e612b51b58fbf27493b477f8` |
| `prefetch-promotion.patch` | `30d7ebf16c046fc5d990b8ef8dac917f941c96fc6b108a9452a0b0cef86afc9d` |
| `check.sh` | `6aff152ce505bf68091ad5ec100a579c3a84c3a6409d6b95d5f0fbb0afcf0474` |
| `REVIEW.md` | `33f1e6734d057b3ebf3d1a8059c05356f12b254c23f7e0c17e4b45f9da33e512` |

Prior private timing results retain their original source, compiler and native-build identities. This fixture change and production promotion do not turn those measurements into a final-source performance run. C ABI remains the default engine.
