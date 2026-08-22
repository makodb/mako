# Mako Documentation

Welcome to the Mako documentation.

---

## Documentation Structure

| Section | Description |
|---------|-------------|
| [Getting Started](#getting-started) | Installation, setup, and first steps |
| [Architecture](#architecture) | System design and components |
| [Configuration](#configuration) | YAML configuration reference |
| [Developer Guide](#developer-guide) | Development setup and internals |
| [RPC Framework](#rpc-framework) | RPC system documentation |
| [Persistence](#persistence) | Storage and disk persistence |
| [Performance](#performance) | Profiling and benchmarking |
| [Migration](#migration) | RustyCpp and Raft migration docs |
| [Development Plans](#development-plans) | Active development plans |
| [Reference](#reference) | Analysis and reference docs |
| [Testing](#testing) | CI and code review |
| [Troubleshooting](#troubleshooting) | Common issues and solutions |

---

## Getting Started

- **[Introduction](getting-started/introduction.md)** - What is Mako and its capabilities
- **[Quick Start](getting-started/quickstart.md)** - Get up and running in 10 minutes
- **[Installation](getting-started/installation.md)** - Step-by-step installation for Debian/Ubuntu
- **[Concepts](getting-started/concepts.md)** - Fundamental concepts: shards, replicas, transactions
- **[Use Cases](getting-started/use-cases.md)** - When and how to use Mako
- **[Docker Setup](getting-started/docker.md)** - Running Mako in Docker containers
  - [Docker Verification](getting-started/docker-verification.md)

---

## Architecture

- **[Architecture Overview](architecture/overview.md)** - High-level system architecture
- **[Client-Server Architecture](architecture/client-server.md)** - Client-server design
  - [Client-Server Roadmap](architecture/client-server-roadmap.md)
- **[Multi-Shard Single Process](architecture/multi-shard.md)** - Running multiple shards in one process
- **[Design Principles](architecture/design-principles.md)** - Core design philosophy and principles
- **[Speculative 2PC](architecture/speculative-2pc.md)** - Speculative two-phase commit protocol
- **[Paxos](architecture/paxos.md)** - Paxos benchmarking commands

---

## Configuration

- **[Configuration Reference](configuration/config.md)** - Complete YAML configuration options

---

## Developer Guide

- **[Development Setup](developer/development.md)** - Setting up development environment
- **[Coroutines & Reactor](developer/coroutines.md)** - Understanding RRR's async model
- **[Fiber API](developer/fiber-api.md)** - Fiber API documentation
  - [Fiber API Refactoring](developer/fiber-api-refactoring.md)
- **[Transport Backends](developer/transport-backends.md)** - Switching between rrr/rpc and eRPC
  - [Transport Stop Fix](developer/transport-stop-fix.md)
- **[C++ Multicore Optimizations](developer/cpp-multicore-optimizations.md)** - CPU optimization techniques

---

## RPC Framework

- **[RPC Overview](rpc/overview.md)** - RRR RPC framework guide
- **[RPC API Reference](rpc/api.md)** - RPC reliability API reference
- **[RPC Reliability](rpc/reliability.md)** - RPC reliability mechanisms
  - [Reliability Plan](rpc/reliability-plan.md)
  - [Safety Plan](rpc/safety-plan.md)
- **[RPC Migration Guide](rpc/migration-guide.md)** - Migrating to new RPC interface
- **[RPC Benchmarking](rpc/benchmark.md)** - RPC benchmarking setup

### RPC Implementation Plans

| Phase | Documents |
|-------|-----------|
| Phase 1 | [Connection State](rpc/phase1_connection_state.md), [Auto Reconnect](rpc/phase1_auto_reconnect.md), [Circuit Breaker](rpc/phase1_circuit_breaker.md), [Reconnect Policy](rpc/phase1_reconnect_policy.md) |
| Phase 2 | [Request Queue](rpc/phase2_request_queue.md), [Request Buffering](rpc/phase2_request_buffering.md), [Timeout Retry](rpc/phase2_timeout_retry.md) |
| Phase 3 | [Heartbeat](rpc/phase3_heartbeat.md), [Metrics](rpc/phase3_metrics.md), [Validation](rpc/phase3_validation.md) |
| Phase 4 | [Graceful Shutdown](rpc/phase4_graceful_shutdown.md), [Restart Detection](rpc/phase4_restart_detection.md) |
| Phase 5 | [Health Pool](rpc/phase5_health_pool.md) |
| Phase 6 | [Callbacks](rpc/phase6_callbacks.md), [Error Types](rpc/phase6_error_types.md) |
| Phase 7 | [Unit Tests](rpc/phase7_unit_tests.md), [Integration Tests](rpc/phase7_2_integration_tests.md) |

---

## Persistence

- **[Disk Persistence](persistence/disk_persistence.md)** - RocksDB persistence with callback implementation
- **[Table Allocation](persistence/table-allocation.md)** - Memory allocation strategies

---

## Performance

- **[Profiling & Plotting](performance/profiling.md)** - CPU profiling, memory analysis, and plotting
- **[CPU Throttling](performance/cpu_throttling.md)** - CPU throttling for scaling tests
  - [CPU Limiting Plan](performance/cpu_limiting_plan.md)

---

## Migration

### RustyCpp Migration (Memory Safety)

- **[Migration Overview](migration/rustycpp/overview.md)** - Master RustyCpp migration plan
- **[Safety Roadmap](migration/rustycpp/safety-roadmap.md)** - 5-phase safety roadmap
- **[RRR Migration](migration/rustycpp/rrr-migration.md)** - RRR-specific migration
- **[Safety Conversion](migration/rustycpp/safety-conversion.md)** - Safety conversion details
- **[RRR Unsafe Blocks](migration/rustycpp/rrr-unsafe-blocks.md)** - RRR unsafe code documentation
- **[Reactor RefCell](migration/rustycpp/reactor-refcell.md)** - Reactor RefCell migration
- **[Reactor Unsafe Blocks](migration/rustycpp/reactor-unsafe-blocks.md)** - Reactor unsafe code
- **[Raft Migration](migration/rustycpp/raft-migration.md)** - Raft-specific migration
- **[Masstree Migration](migration/rustycpp/masstree-migration.md)** - Masstree migration plan

### Raft Migration (Paxos to Raft)

- **[Raft Migration Overview](migration/raft/overview.md)** - Replace Paxos with Raft
- **[Architecture Analysis](migration/raft/architecture-analysis.md)** - Architecture for Raft
- **[Complete Architecture](migration/raft/complete-architecture.md)** - Complete Raft architecture
- **[Mako Explained](migration/raft/mako-explained.md)** - Mako explanation
- **[Raft Explained](migration/raft/raft-explained.md)** - Raft protocol explanation
- **[Raft Helper](migration/raft/raft-helper.md)** - Raft helper implementation
- **[Main Helper Implementation](migration/raft/main-helper-impl.md)** - Implementation details
- **[Migration Status](migration/raft/status.md)** - Migration status tracking

---

## Development Plans

### Rust transaction cache

- **[Silo-to-Mako transaction-cache roadmap](plans/rust-mako-transaction-cache.md)** — keep C++ Silo behind a C ABI first, add local transactional durability, port distributed Mako, then replace the local engine with native Rust.

### Config Node System

- **[Overview](plans/config-node/overview.md)** - Configuration persistence master plan
- [Task 1](plans/config-node/task1.md) | [Task 2](plans/config-node/task2.md) | [Task 3](plans/config-node/task3.md) | [Task 4](plans/config-node/task4.md) | [Task 5](plans/config-node/task5.md)

### Log Persistence & Recovery

| Phase 1: Log Storage | Phase 2: Recovery | Phase 3: Snapshots | Phase 4+ |
|---------------------|-------------------|-------------------|----------|
| [Interface](plans/log-persistence/phase1_1_log_persistence_interface.md) | [Recovery Manager](plans/log-persistence/phase2_1_recovery_manager.md) | [Snapshot Interface](plans/log-persistence/phase3_1_snapshot_interface.md) | [Pre-vote Protocol](plans/log-persistence/phase4_1_prevote_protocol.md) |
| [RocksDB Backend](plans/log-persistence/phase1_2_rocksdb_log_backend.md) | [Log Replay](plans/log-persistence/phase2_2_log_replay.md) | [Snapshot Format](plans/log-persistence/phase3_2_snapshot_format.md) | [Chaos Engineering](plans/log-persistence/phase7_4_chaos_engineering.md) |
| [Raft Integration](plans/log-persistence/phase1_3_raft_integration.md) | [Uncommitted Entries](plans/log-persistence/phase2_3_uncommitted_entries.md) | [Snapshot Storage](plans/log-persistence/phase3_3_snapshot_storage.md) | |
| [Paxos Integration](plans/log-persistence/phase1_4_paxos_integration.md) | [State Machine Recovery](plans/log-persistence/phase2_4_state_machine_recovery.md) | [Log Compaction](plans/log-persistence/phase3_4_log_compaction.md) | |

### Range Sharding

- [Task 1](plans/range-sharding/task1.md) | [Task 2](plans/range-sharding/task2.md) | [Task 3](plans/range-sharding/task3.md) | [Task 4](plans/range-sharding/task4.md)

### Client-Server Unification

- [Unify Interface](plans/client-server/unify_client_server_interface_plan.md)
- [Unify Mode](plans/client-server/unify_client_mode_plan.md)
- [Unify DB Interface](plans/client-server/unify_db_interface_plan.md)
- [CI Test Plan](plans/client-server/client_server_ci_test_plan.md)
- [Evaluation](plans/client-server/client_server_evaluation.md)
- [Test Client Service](plans/client-server/test_client_service_plan.md)
- [Legacy API Removal](plans/client-server/legacy_api_removal_plan.md)
- [Startup Tests](plans/client-server/task8_4_startup_tests_plan.md)

### Issue Fixes

- [Fix Commit/Rollback](plans/issues/fix_commit_rollback_plan.md)
- [Fix Txn ID Collision](plans/issues/fix_txn_id_collision_plan.md)
- [Node Crash Replication](plans/issues/node_crash_replication_plan.md)
- [Shard Crash Reboot](plans/issues/shard_crash_reboot_plan.md)
- [Transaction Timeout](plans/issues/txn_timeout_plan.md)
- [Leader Shutdown Hang](plans/issues/leader_shutdown_hang.md)

---

## Reference

- **[Function Dependencies](reference/function-dependencies.md)** - Function dependency analysis
- **[Naming Conventions](reference/naming-conventions.md)** - Codebase naming conventions
- **[TPC-C Sharding](reference/tpcc-sharding.md)** - TPC-C benchmark sharding behavior
- **[Event Rename Plan](reference/plan_event_rename.md)** - Event class renaming plan
- **[Glossary](reference/glossary.md)** - Terms and definitions

---

## Testing

- **[Judge System](testing/judge.md)** - Judge system documentation
- **[Commit Reviews](testing/commit_reviews.md)** - Code review guidelines
- **[Hourly CI Check](testing/hourly_ci_check.md)** - CI check procedure

---

## Troubleshooting

- **[Troubleshooting Guide](troubleshooting/guide.md)** - Common issues and solutions

---

## Archive

Legacy documentation kept for reference:

- [Build (Legacy)](archive/build-legacy.md) - Deprecated Python 2 build system
- [Run (Legacy)](archive/run-legacy.md) - Deprecated run.py scripts
- [EC2 (Legacy)](archive/ec2-legacy.md) - Deprecated AWS deployment
- [Docker Build Success](archive/DOCKER_BUILD_SUCCESS.md) - Historical build verification
- [README Janus](archive/README-janus.md) - Original Janus project quick start
- [New Config Format](archive/NEW_CONFIG_FORMAT.md) - Configuration format migration guide
- [RocksDB Guide](archive/RocksDB_Guide.md) - RocksDB API reference and usage patterns

---

## Comprehensive Guides (Books)

- **[Mako Book](mako-book.md)** - Complete developer guide to the Mako transactional datastore
- **[Raft Book](raft-book.md)** - Detailed guide to Raft consensus implementation in Mako
- **[SRPC Book](srpc-book.md)** - Technical guide to the RRR/RPC framework

---

## Additional Resources

### Papers & Publications

- [OSDI'25 Paper](https://www.usenix.org/conference/osdi25/presentation/shen-weihai) - Mako: Speculative Distributed Transactions with Geo-Replication

### Community

- [GitHub Repository](https://github.com/makodb/mako)
- [Issue Tracker](https://github.com/makodb/mako/issues)

---

*Last updated: April 2026*
