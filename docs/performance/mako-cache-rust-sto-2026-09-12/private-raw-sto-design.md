# Raw STO attribution diagnostic

This is the preserved pre-measurement design. Completed paired results are
in `private-raw-sto-results.json` and the parent performance report.

This is a private diagnostic, not a proposed production API. Production Rust sources are unchanged. The same `engine_compare` binary runs `engine` for the facade or `raw-sto` for core plus Table. Each run starts a fresh process. Both report engine `sto`, protocol `raw-sto-attribution-v1`, and implementation `facade` or `raw`.

The fixed original workload is unchanged. Each worker cycles over its own 256 keys, encoded as worker*256+key in 8 big-endian bytes. Values are 128 bytes with the worker and iteration prefix. Warmup and measured counts, CPU affinity, barriers, PMU counters, and all 256 final values per worker use the same driver. The additional invariant branch selecting raw or facade is shared code in the same binary. This diagnostic is internally paired; do not compare its absolute throughput against binaries built from another driver.

## Matched behavior

The raw setup repeats the current LocalDb core/native/Table configuration. Core uses Serializable isolation, 460 workers, 2048 items and 2048 locks. The native runtime allows 460 threads. The direct table retains at most 2^40 records, 2^44 key bytes and 2^32 consumed IDs, with an unlimited RegistryBudget. Blocking structural admission, bounded atomic values through 160 bytes, and record-local versions are enabled. All other defaults stay unchanged. The main core/native attachment remains occupied, then each worker attaches on its own pinned OS thread before warmup. Native and core worker boxes remain process-lifetime, matching the facade's current operational model.

Each raw transaction calls `WorkerContext::begin`, ordinary `Table::put_with_previous_presence`, then ordinary `Transaction::commit` without a hook. The value is owned by core. No borrowed staging, resolved identity cache, special fixed-key batch, RCU scope reuse, or direct-plan shortcut is used. Ordinary native directory lookup performs its existing health and thread admission. Core begin and commit preserve their runtime health admission. Both arms therefore use the existing direct typed scalar commit path and local-version policy, with no Mako HLC allocation for unlogged commits.

## Deliberately omitted facade work

Raw access omits the TLS attachment search, facade active-state tracking, database/table facade checks, weighted transaction limits and key/value limits, TouchLog key/value copies and reuse, canonical log-operation/witness bookkeeping, and facade terminal error translation. No input in this bounded workload approaches a facade limit. Core and table limits remain active. A raw failure terminates the sample; it is not translated into a facade retry or recovery. The returned core outcome must be clean Committed. Verification outside timing also checks both raw runtime health flags. This does not claim raw APIs implement the cache facade's full failure-reporting or logging contract.

The result field acknowledged is the number of successful loop commits, including warmup, as in the original unlogged benchmark. It is not a durable storage ACK. Applied and queued remain zero, and drained remains false. Every final key is verified after all workers leave the measured region. No queue or RocksDB write occurs in either compared mode.

## Build and qualification

`build.sh` pins Rust 1.95.0, compiler 59807616e1fa2540724bfbac14d7976d7e4a3860, non-LTO, znver1, frame pointers, and debug info 1. It checks the exact native archive hashes before and after each build/test command. The production native metadata refresh has libmako SHA 53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402 and libmasstree SHA d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613, fingerprint 9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea. These inputs differ only in CI metadata from the earlier private cache candidate's native provenance.

The new native fixture checks exact config, previous-presence and repeated-write behavior, abort invisibility, clean local CommitInfo with no global ID, and later update/read success. CLI smoke checks must run both modes and validate counts/final values. Performance is pending. No production promotion is implied.
