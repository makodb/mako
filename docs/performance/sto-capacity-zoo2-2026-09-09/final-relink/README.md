# Final native relink evidence

This bundle covers source commit `a3ad9a110727f5ecc937cbeee23fe40152df719f`, including the atomic capacity-diagnostic fix. Run `python3 verify.py` from this directory, or use `python3 verify.py --repo /path/to/repository --evidence-dir /path/to/copied/bundle`. The checks use exceptions, so `python3 -O` does not disable them.

The native build passed all 31 selected boundary CTests. The final relinked binary then passed four capacity smokes covering startup, load, one-worker runtime, and four-worker runtime. Each returned the expected exit 3, emitted one intact diagnostic, and produced no successful throughput result.

The relink reused the trained Rust archive without rebuilding it or retraining PGO. The 123-file captured Rust subtree matches the final committed subtree byte-for-byte. Recorded native link inputs stayed unchanged during relink and smoke testing. The retained comparison reports all 122 native compile commands unchanged from the previous build.

| Reported artifact | SHA-256 |
| --- | --- |
| Final relinked binary | `3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe` |
| Reused Rust PGO archive | `7d29c5077d9a62b9bdce4c4b1a7f1c7eecc206fc7cc92138e8b921321a579b87` |
| Reused Rust profile | `c14043bbd29a8f74459bb2ed1bfcb98917313ee508fd4c2a7f2a9412dadea62b` |

## Growth check

The final binary completed a 180-second run on CPU 10 with one worker, default workload mix, a 4 GiB native allocator setting, and an 8 GiB registry budget. It committed 10,187,161 transactions with zero aborts and exited 0.

`order_line_0` reached 46,141,818 retained records and consumed IDs, with 738,269,088 retained key bytes. Database structural registry allocations reached 4,557,103,264 bytes, leaving 4,032,831,328 bytes of budget. The row/key/ID quotas remained `UINT64_MAX`.

This is a capacity check, not a guarded performance comparison. The structural registry budget is not an RSS limit. It excludes variable payloads, allocator overhead, fixed table controls, and native allocations. IDs and tombstones are not reclaimed. Recoverable quota exhaustion does not promise graceful recovery from process or OS OOM.

## What the verifier checks

`selected-artifact-hashes.json` covers only the 17 raw files retained here. The verifier checks their hashes and sizes, committed native/CI source references, the full committed Rust subtree, relink metadata consistency, all 31 CTest results, four final-binary capacity smokes, and the growth counters. It never reads the machine-specific paths recorded inside the evidence.

The original `artifact-hashes.json` describes 170 remote artifacts, including the uncopied binary, source snapshots, and build logs. Only 14 of its entries are present here. The reused Rust archive and profile also remain remote. The original manifest itself, the later compile-command comparison, and the later growth log have separate local checksums in the selected manifest. The verifier does not claim to rehash the omitted remote files. Binary/archive identity checks compare retained metadata, not local binary bytes.

`driver.py` is an unchanged record of the remote collection program, not the portable verifier. Its machine-specific paths and historical assertions are preserved as evidence. Do not run it to check this bundle. The new `verify.py` is read-only and pins source lookups to the commit above, independent of later documentation changes.

The verifier passed on the original bundle in normal and optimized Python, and on a copy outside the repository. Changing the copied CTest summary from 31 tests to 30 was rejected with exit 1 and an artifact hash mismatch in both modes. The original raw files were unchanged.
