# Retire standalone caches

Keep the distributed transaction path with RocksDB as the persistent row store
and optional Masstree acceleration. Retire the earlier standalone `mako-cache`
and `mrx` engines, their cache-only facades, C API, tools and native ABI.

1. Move the shared timestamp into `mako-clock`. Preserve the byte-store contract
   and its tests in the RocksDB binding, renamed `mako-rocksdb-ffi`.
2. Remove standalone cache crates, cache-only engine facades and the `mtx` ABI.
   Remove their build, installation, CI, sanitizer and benchmark entry points.
3. Update supported-path documentation, dependency audits and component census.
   Historical qualification reports remain evidence for their recorded revisions.
4. Validate dependency closure, retained contracts, both distributed storage
   modes, migration, recovery/reopen and the native build source closure. Run
   the isolated upper protocol gate. Performance campaigns remain separate.

This phase retained the direct RocksDB/Raft service and shared STO/Masstree
components. The service is subsequently removed by the
[service retirement plan](retire-rocksdb-raft-service.md). This cache retirement
changes code ownership and removes unused entry points; the distributed
journal, checkpoint and user-row formats must remain compatible.

Status: complete. Removed 11 Cargo packages and the unused cache ABI, tools and
CI lanes. The workspace now contains 28 packages. The component census decreased
from 48 to 36 accounting units and from 394,855 to 339,246 nonblank source lines.

Both supported storage modes passed qualification: 126 native runtime/API cases,
48 RocksDB-only cases, component tests, strict Clippy and dependency audits. The
isolated upper protocol gate passed in 166.949 seconds on 32 CPUs. See the
[qualification record](../testing/standalone-cache-retirement/README.md) for
commands, evidence and test limits.
