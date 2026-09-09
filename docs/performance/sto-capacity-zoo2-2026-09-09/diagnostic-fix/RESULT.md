# Capacity diagnostic regression

Passed on source revision `a3ad9a110727f5ecc937cbeee23fe40152df719f`, captured locally on 2026-09-09. This evidence covers the C++ diagnostic fix, not a fresh full transaction, native integration, or performance gate.

The reporter writes one record of at most 512 bytes, including leading and trailing newlines. It replaces control characters with spaces and marks truncated messages with `...`. The existing successful benchmark result writer, resource-exhaustion validator, and quiescent usage reporting did not change.

`test_benchmark_output` passes 100 records per run. A separate logger thread writes between every ostream fragment using a request/acknowledgement handshake. A control case proves the former chained-insertion pattern gets fragmented. The production helper keeps each record intact. Cases include startup, load, run, embedded controls, null fields, an exact 512-byte fit, and an oversized detail.

| Retained run | Result |
| --- | --- |
| Direct Clang 22.1.8 native | 100 records, exit 0 |
| Direct Clang AddressSanitizer | 100 records, exit 0, no finding |
| Direct Clang UndefinedBehaviorSanitizer | 100 records, exit 0, no finding |
| Direct Clang ThreadSanitizer | 100 records, exit 0, no finding |
| Actual project CMake Release and CTest | 100 records, 1/1 passed |
| Actual project CMake Release with ASan and CTest | 100 records, 1/1 passed |

No sanitizer suppressions or sanitizer-option overrides were used. Direct builds use the compiler's default C++ library. The actual CMake builds use the project's libc++ flags. Retained command lists prove ASan is present on both compilation and linkage and absent from the unsanitized Release build. Only the standalone target was built. No native integration build or remote job ran for this collection.

Both CMake inventories register `test_benchmark_output` exactly once with labels `sto;rust;ffi;diagnostics;concurrency`, `RUN_SERIAL=TRUE`, and a 30-second timeout. There are 19 Rust-labeled tests in the registered inventory. The other 18 were not rerun by this standalone collection.

## Retained files

- `source-provenance.json` records six source hashes from `git show a3ad9a1:path`, compared byte-for-byte with the files compiled here. These include the helper, caller, test, CMake registration, and hosted CI/sanitizer inventories.
- `direct-*.log` contain exact build/run commands and explicit successful exit statuses.
- `cmake-*-*.log`, `cmake-*-commands.txt`, `cmake-*-inventory.json`, and `cmake-*-cache.txt` retain actual build, test, registration, and instrumentation evidence.
- `collect.sh` records the local collection commands. `verify.py` checks results against the committed source, rather than a later dirty worktree. `verify.log` records its successful run.
- `git-status-before.txt` and `git-status-after.txt` are byte-identical. The repository files and earlier failed CI/full-ASan records were not changed.
- `SHA256SUMS` covers every other retained file in this directory.

To check a copied bundle, run `sha256sum -c SHA256SUMS` from its directory, then run `python3 verify.py /path/to/repository`. The repository must contain the pinned commit. Binaries and large build artifacts are not retained in this compact bundle.

These are new capture runs of the final 100-record test. They do not reconstruct or overwrite earlier console-only runs.
