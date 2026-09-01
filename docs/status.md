# Mako Project Status Report

**Date**: 2026-04-13
**Purpose**: Assess each component's production-readiness and identify gaps to drive future development.

---

## Executive Summary

Mako is a **research-grade distributed transactional datastore** with strong academic foundations (OSDI'25). The core transaction engine and RPC framework are mature and well-tested. However, significant gaps remain in persistence, crash recovery, security, observability, and operational tooling before it can be considered production-ready.

| Component | Status | Production-Ready? |
|-----------|--------|-------------------|
| [RPC Framework (SRPC)](#1-rpc-framework-srpc) | 90% | Yes |
| [Raft Consensus](#2-raft-consensus) | 70% | Partial |
| [Paxos Consensus](#3-paxos-consensus) | 60% | No |
| [Masstree (In-Memory Storage)](#4-masstree-in-memory-storage) | 90% | Yes (in-memory only) |
| [Transaction Engine](#5-transaction-engine) | 80% | Partial |
| [RocksDB Persistence](#6-rocksdb-persistence) | 20% | No |
| [Client API (RocksDB-Compatible)](#7-client-api-rocksdb-compatible) | 40% | No |
| [Redis-Compatible Interface](#8-redis-compatible-interface) | 15% | No |
| [Testing & CI](#9-testing--ci) | 80% | Yes |
| [Security](#10-security) | 5% | No |
| [Observability](#11-observability) | 10% | No |
| [Operational Tooling](#12-operational-tooling) | 20% | No |

---

## 1. RPC Framework (SRPC)

**Status: 90% — Production-Ready**

The SRPC framework is the most mature component. Nearly all reliability features are fully implemented, not stubs.

### What Works

- **Stackful coroutines (fibers)**: Custom x86_64 assembly context switching, 100K+ concurrent fibers per thread. Proven in production benchmarks.
- **Reactor pattern**: Thread-local event loops with epoll/kqueue abstraction. Efficient I/O multiplexing.
- **Binary RPC**: Compact wire format with code generation from `.rpc` definitions. Fast marshalling.
- **Circuit breaker**: Full state machine (CLOSED/OPEN/HALF_OPEN) with configurable thresholds.
- **Reconnection**: Exponential backoff with jitter, max retries, configurable delays.
- **Request buffering**: Thread-safe queue with TTL expiration and overflow strategies.
- **Graceful shutdown**: 5-phase shutdown (RUNNING → STOP_ACCEPTING → DRAINING → CLOSING → STOPPED).
- **Heartbeat/keepalive**: Configurable stale connection detection.
- **Structured errors**: 20+ error types with categorization.
- **Connection metrics**: Latency, success/failure rates, bytes transferred.
- **Test coverage**: 26 test files covering all reliability features.

### Gaps

- **Async thread pool**: `run_async()` executes inline instead of dispatching to a thread pool (low impact since handlers run in fiber contexts).
- **No TLS support**: All communication is unencrypted.

### Recommendation

Ready for production use. Add TLS for security-sensitive deployments.

---

## 2. Raft Consensus

**Status: 70% — Partially Production-Ready**

Core safety guarantees (no split-brain, crash recovery, leader election) are solid. Advanced features need work.

### What Works

- **Leader election**: Standard Raft with term-based voting, preferred leader bias (150-300ms vs 500ms-2s timeout).
- **Log replication**: Parallel AppendEntries with batching optimization (`RAFT_BATCH_OPTIMIZATION`).
- **Speculative voting**: Two-phase voting with memory (fast) and durable (safe) acks.
- **Preferred leader system**: Election suppression prevents churn under CPU contention.
- **Leadership transfer**: TimeoutNow protocol for controlled failover.
- **Persistence**: LogStorage with sync and async modes. Recovery of term, vote, commitIndex, and log entries.
- **Mako integration**: Complete bridge via `raft_main_helper` with callback registration.
- **Test coverage**: 20+ test categories including persistence, partitions, speculative replication.

### Gaps

| Gap | Severity | Impact |
|-----|----------|--------|
| **No snapshots** | HIGH | Memory grows unbounded with log entries. Followers cannot catch up efficiently after long partitions. |
| **No membership changes** | HIGH | Cannot add/remove replicas without full cluster restart. |
| **Client notification incomplete** | MEDIUM | No ROLLEDBACK notification to clients when speculative entries fail. |
| **Follower apply bottleneck** | MEDIUM | `in_applying_logs_` guard drops work when AppendEntries arrives during log application. |
| **Non-leader log drop** | MEDIUM | `add_log_to_nc()` silently drops logs on non-leader nodes. |
| **specCommitIndex not persisted** | LOW | After crash, unsecured entries may be lost even if speculatively committed. |

### Recommendation

Suitable for development and small clusters (3-5 replicas). For production: implement snapshots, membership changes, and fix the follower apply bottleneck.

---

## 3. Paxos Consensus

**Status: 60% — Not Production-Ready**

The Paxos implementation has more unfinished code paths than Raft.

### What Works

- Basic Multi-Paxos consensus phases (Prepare, Accept, Commit).
- Bulk operations (BulkPrepare, BulkAccept, BulkCommit).
- Ballot/epoch management with proper numbering.
- Per-slot instance tracking.

### Gaps

| Gap | Severity | Impact |
|-----|----------|--------|
| **Incomplete leader election** | HIGH | ElectionState class exists but full protocol is stubbed. ~39 `verify(0)` assertions in paxos/ alone. |
| **No snapshotting** | HIGH | Log compaction only at fixed 100-slot intervals. Memory unbounded. |
| **No crash recovery testing** | HIGH | Unlike Raft, no persistence test infrastructure. |
| **`verify(0)` error handling** | HIGH | 39+ assertions kill the process on any unexpected state. No graceful degradation. |
| **No membership changes** | HIGH | Static partition membership from config. |

### Recommendation

Functional for benchmarking under normal conditions. Not suitable for any deployment where failures may occur. Consider focusing development effort on Raft instead.

---

## 4. Masstree (In-Memory Storage)

**Status: 90% — Production-Ready (for in-memory use)**

Masstree is the original MIT implementation (Eddie Kohler, Yandong Mao, Robert Morris), modified for Mako's transaction layer.

### What Works

- Concurrent B+tree optimized for multi-core CPUs.
- Lock-free reads with multi-version support.
- Cache-friendly memory layout with sub-microsecond read latency.
- Full get/put/delete/scan operations.
- Transaction-aware cursor (`masstree_tcursor.hh`) for OCC/MVCC.
- Sharded variant (`mbta_sharded_ordered_index`) for multi-shard deployments.

### Gaps

| Gap | Severity | Impact |
|-----|----------|--------|
| **In-memory only** | HIGH | All data lost on process crash. No native persistence. |
| **No recovery from disk** | HIGH | No mechanism to rebuild Masstree from RocksDB or WAL. |
| **Tight coupling with STO** | MEDIUM | Transaction semantics woven into index layer, making standalone use complex. |

### Recommendation

Proven, high-performance in-memory index. The critical missing piece is crash recovery — Masstree needs a way to rebuild from persistent storage after restart.

---

## 5. Transaction Engine

**Status: 80% — Partially Production-Ready**

### What Works

- **OCC (Optimistic Concurrency Control)**: Mature implementation with MVCC, thread-local transaction state, read/write set tracking.
- **Transaction API**: Clean `abstract_db` interface with `new_txn`, `commit_txn`, `abort_txn`.
- **Speculative 2PC**: Core Mako innovation — watermark-based validation with background replication.
- **Protocol plugin system (Frame)**: Factory pattern for the remaining transaction and replication protocols.
- **Coordinator framework**: Phase-based execution (DISPATCH → PREPARE → COMMIT) with pluggable backends.

### What's Research-Only (Not Production-Ready)

| Protocol | Completeness | Status |
|----------|-------------|--------|
| OCC | 65% | Lazy versioning, ad-hoc design |

The former standalone Janus, Mencius, SNOW/RO6, Extern-C, 2PL, Rule, TAPIR,
FPGA-Raft, Copilot, RCC/Rococo, TROAD, MDCC, Carousel, and Februus protocol
implementations are retired and are not supported configuration options. The
old `deptran` and `deptran_er` names were RCC aliases and are retired too.
The former internal MemDB transaction and storage stack is retired; Mako uses
STO `Transaction` with MassTrans/Masstree through `mbta_wrapper`. The original
Silo `txn`/`txn_btree`/`txn_proto2` engine is also retired, source-guarded, and
not selectable. `SiloRuntime` remains live allocator/RCU/Masstree support and
must not be confused with that retired transaction engine.
The `rpc_null` benchmark mode and its no-op Classic RPC endpoint are retired.
Generic Jetpack recovery code remains as a legacy subsystem pending a separate
audit; it is not a supported replacement for the retired Rule protocol.
EPaxos, Replicated Commit, and Multi-Paxos Plus were unimplemented selector
placeholders; those names are rejected instead of silently selecting `none`.

### Critical Issues

- **`verify(0)` error handling**: 100+ assertions across all protocols kill the process on unexpected state. No graceful degradation.
- **No crash recovery**: Transactions are not recoverable after process restart.
- **No external client library**: Only benchmark harness exists (ClientWorker). No language bindings.

### Recommendation

The OCC engine and speculative 2PC are the production path. Other protocols should be treated as research artifacts. Replace `verify(0)` with proper error handling before production use.

---

## 6. RocksDB Persistence

**Status: 20% — Not Production-Ready**

### What Exists

- `RocksDBPersistence` class wrapping RocksDB C API.
- Async write queueing with ordered callbacks.
- Per-partition database isolation.
- Transaction log persistence (not KV data).

### What's Missing

| Gap | Severity | Impact |
|-----|----------|--------|
| **Not integrated with Masstree** | CRITICAL | RocksDB only stores transaction logs, not actual KV data. |
| **No crash recovery** | CRITICAL | No mechanism to replay logs and rebuild Masstree state. |
| **No WAL-based recovery** | CRITICAL | No write-ahead log protocol connecting Masstree writes to RocksDB. |
| **No checkpoint/snapshot** | HIGH | No way to create a consistent point-in-time snapshot. |
| **No backup/restore API** | MEDIUM | No operational tooling for data management. |

### Recommendation

The persistence layer needs fundamental redesign. Current implementation only persists Paxos/Raft transaction logs, not the actual key-value data that Masstree holds. The path forward:

1. Wire Masstree Put/Get/Delete to also write to RocksDB.
2. Implement WAL-based recovery to rebuild Masstree from RocksDB on restart.
3. Add checkpointing for fast recovery.

---

## 7. Client API (RocksDB-Compatible)

**Status: 40% — Not Production-Ready**

### What Exists

A RocksDB-like API is partially implemented:

```cpp
// Local mode
DB::Open(options, path, &db);
auto txn = db->BeginTransaction();
table->Put(txn, key, value);
table->Get(txn, key, &value);
table->Delete(txn, key);
db->Commit(txn);

// Remote mode (via RPC)
RemoteDB::Connect(options, shard_index, &db);
// Same Put/Get/Delete/Commit API
```

- `DB` and `RemoteDB` both implement `IDatabase` interface.
- `ITable` provides `Put`, `Get`, `Delete` operations.
- `Options` struct with `create_if_missing`, `num_threads`, `num_shards`, etc.
- Working example: `examples/simpleTransaction.cc`.

### What's Missing

| Gap | Severity | Impact |
|-----|----------|--------|
| **No WriteBatch** | HIGH | No atomic multi-write outside transactions. |
| **No iterators/range queries** | HIGH | Scan exists internally but not exposed in ITable. |
| **No column families** | MEDIUM | Single namespace for all data. |
| **No CompactRange, Flush, etc.** | MEDIUM | No administrative operations. |
| **No connection pooling** | MEDIUM | RemoteDB creates single connections. |
| **No language bindings** | MEDIUM | C++ only. No C, Python, Java, Go bindings. |
| **No data durability** | CRITICAL | Underlying storage is in-memory only (see section 6). |

### Recommendation

The API shape is right but the implementation is shallow. Priority: (1) add data durability via RocksDB integration, (2) add iterators/range queries, (3) add WriteBatch, (4) add language bindings.

---

## 8. Redis-Compatible Interface

**Status: 15% — Early Prototype**

### What Exists

- **makoCon** server: Located in `third-party/redis/cpp/makoCon.cc` and `third-party/redis/`.
- Supports Redis strings, collections, Pub/Sub, connection commands, and `MULTI/EXEC`; see `docs/redis_interface.md` for the scoped command surface.
- Thread-per-core architecture with one shared nonblocking listener and deterministic round-robin connection assignment.
- Python compatibility tests in `third-party/redis/compat/` use `makoCon` as the server.

### What's Missing

| Gap | Severity | Impact |
|-----|----------|--------|
| **No Redis AUTH** | HIGH | No authentication support. |
| **No Redis Cluster protocol** | HIGH | No CLUSTER SLOTS, MOVED/ASK redirects. |
| **No full Redis command parity** | HIGH | Streams, modules, scripting, clustering, and full Redis Search compatibility remain outside the scoped command surface. |
| **No persistence semantics** | HIGH | In-memory only, no RDB/AOF equivalent. |
| **No replication protocol** | MEDIUM | No Redis REPLCONF/PSYNC equivalent. |
| **No Lua scripting** | LOW | No EVAL/EVALSHA support. |
| **No TTL/expiry** | MEDIUM | No EXPIRE, TTL, PTTL commands. |

### Recommendation

The Redis interface is a thin protocol wrapper, not a Redis replacement. To be useful as a Redis alternative:

1. Implement core data structures (HASH, LIST, SET, ZSET).
2. Add AUTH and ACL.
3. Add TTL/expiry support.
4. Implement Redis Cluster protocol for transparent sharding.
5. Wire persistence through the RocksDB layer.

---

## 9. Testing & CI

**Status: 80% — Production-Ready**

### Strengths

- **CI pipeline**: 20+ test suites on GitHub Actions, 180-minute timeout, CPU throttling for reproducibility.
- **Docker**: Full-featured containerized build and test (Ubuntu 24.04, ARM64/AMD64 support).
- **Integration tests**: Comprehensive CI harness (`ci/ci.sh`, 751 lines) with dynamic port allocation, memory limiting, hanging process detection.
- **Correctness tests**: 17 Python-based test files covering KV operations, transactions, isolation, stress, workloads (bank, counter, inventory, sessions, crash recovery).
- **Raft lab tests**: 20+ test categories including persistence, partitions, speculative replication.
- **RPC tests**: 26 test files covering all reliability features.
- **Borrow checking**: RustyCpp static analysis integrated into CMake.

### Gaps

| Gap | Severity | Impact |
|-----|----------|--------|
| **No sanitizer builds** | MEDIUM | No AddressSanitizer, MemorySanitizer, ThreadSanitizer in CI. |
| **No performance regression tracking** | MEDIUM | No baseline TPS/latency comparison between commits. |
| **No coverage reporting** | LOW | No line/branch coverage metrics. |
| **Debug build not tested in CI** | LOW | Only Release builds are tested. |
| **Self-hosted runner offline** | MEDIUM | GitHub CI runner `mako-ci-1` has been offline for ~50 days. Tests only run locally. |

### Recommendation

Add ASan/TSan builds to CI, implement performance regression tracking, and fix the CI runner.

---

## 10. Security

**Status: 5% — Not Production-Ready**

### What Exists

- Nothing meaningful. No TLS, no authentication, no encryption, no ACL.

### What's Needed

| Feature | Priority | Effort |
|---------|----------|--------|
| **TLS/mTLS for RPC** | P0 | Medium — wrap socket layer with OpenSSL/BoringSSL |
| **Authentication** | P0 | Medium — add client auth tokens or certificates |
| **Encryption at rest** | P1 | Low — RocksDB supports encryption natively |
| **ACL/authorization** | P1 | Medium — role-based access control |
| **Audit logging** | P2 | Low — log access patterns |

### Recommendation

Security is a hard blocker for any production deployment handling real data. Prioritize TLS for inter-node and client communication.

---

## 11. Observability

**Status: 10% — Not Production-Ready**

### What Exists

- Basic `fprintf(stderr, ...)` logging (no structured format).
- Benchmark throughput/latency counters during test runs.
- RPC connection metrics (internal, not exported).

### What's Missing

| Feature | Priority | Effort |
|---------|----------|--------|
| **Structured logging** (JSON) | P0 | Medium |
| **Metrics export** (Prometheus) | P0 | Medium — expose counters via HTTP endpoint |
| **Health check endpoint** | P0 | Low — HTTP `/health` and `/ready` |
| **Distributed tracing** | P1 | Medium — OpenTelemetry integration |
| **Log levels and rotation** | P1 | Low |
| **Dashboard templates** | P2 | Low — Grafana dashboards |

### Recommendation

Without observability, operators cannot diagnose issues in production. This is a hard blocker alongside security.

---

## 12. Operational Tooling

**Status: 20% — Not Production-Ready**

### What Exists

- YAML-based cluster configuration (40+ pre-built configs).
- Docker support for development/testing.
- CI scripts for automated testing.
- `dbtest` binary as the main server process.
- `makoCon` as Redis-compatible server.

### What's Missing

| Feature | Priority | Effort |
|---------|----------|--------|
| **Admin CLI** | P0 | Medium — cluster status, reconfiguration, data migration |
| **Graceful rolling restart** | P0 | Medium — drain, upgrade, restart without downtime |
| **Backup/restore** | P1 | Medium — snapshot-based backup and point-in-time recovery |
| **Shard rebalancing** | P1 | High — dynamic shard migration |
| **Service discovery** | P1 | Medium — replace static IP configs with DNS/Consul/etc. |
| **systemd unit files** | P2 | Low — process management |
| **Config hot-reload** | P2 | Medium — change config without restart |

---

## Priority Roadmap

Based on this assessment, here are the recommended development priorities:

### P0 — Must Have for Any Production Use

1. **Data durability**: Wire Masstree writes to RocksDB. Implement crash recovery (WAL replay to rebuild Masstree).
2. **TLS/authentication**: Encrypt inter-node and client communication. Add client auth.
3. **Observability**: Structured logging, Prometheus metrics, health check endpoints.
4. **Raft snapshots**: Prevent unbounded memory growth. Enable follower catch-up.

### P1 — Needed for Production Deployment

5. **Raft membership changes**: Add/remove replicas without restart.
6. **Client API completeness**: Iterators, WriteBatch, range queries.
7. **Admin CLI**: Cluster status, reconfiguration, operational commands.
8. **Backup/restore**: Snapshot-based backup and point-in-time recovery.
9. **Graceful rolling restart**: Zero-downtime upgrades.
10. **Error handling overhaul**: Replace `verify(0)` with proper error codes and recovery.

### P2 — Production Hardening

11. **Redis command coverage**: HASH, LIST, SET, ZSET, TTL/expiry, AUTH.
12. **Language bindings**: C, Python, Java, Go client libraries.
13. **Performance regression CI**: Automated TPS/latency tracking per commit.
14. **Sanitizer builds**: ASan, TSan, MSan in CI pipeline.
15. **Dynamic sharding**: Shard split/merge/rebalance without downtime.

### P3 — Nice to Have

16. **Distributed tracing**: OpenTelemetry integration.
17. **Config hot-reload**: Change settings without restart.
18. **Multi-region optimizations**: Latency-aware leader placement, follower reads.
19. **SQL query layer**: Simple SELECT/INSERT/UPDATE on top of KV.

---

## Component Dependency Graph

```
                    Client Applications
                          |
              +-----------+-----------+
              |                       |
        RocksDB-like API       Redis Interface
              |                       |
              +-----------+-----------+
                          |
                  Transaction Engine (OCC)
                          |
              +-----------+-----------+
              |                       |
         Masstree              RocksDB Persistence
       (in-memory)            (crash recovery needed)
              |                       |
              +-----------+-----------+
                          |
                 Replication Layer
              +-----------+-----------+
              |                       |
           Paxos                    Raft
              |                       |
              +-----------+-----------+
                          |
                   RPC Framework (SRPC)
                          |
                    Fiber / Reactor
```

**Bottom-up readiness**: SRPC and Masstree are solid foundations. The critical gap is the **persistence and recovery layer** connecting Masstree to RocksDB. Everything above (client APIs, Redis interface) depends on this being solved first.

---

*This report should be revisited monthly as development progresses. Each section's percentage should be updated as gaps are addressed.*
