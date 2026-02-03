# Node/Shard Crash Recovery with Replication Support

## Overview

This document describes the implementation plan for supporting node/shard crashes with automatic recovery using replication (Raft/Paxos). When a node crashes and reboots, it should recover its state from the replication log and rejoin the cluster without data loss.

## Current State Analysis

### What Works
- **RPC Reliability**: Client reconnection, circuit breaker, health monitoring (Phase 1-8 of RPC Enhancement)
- **Transaction Timeout**: Transactions timeout if shards fail (txn_timeout_us_)
- **Replication Protocols**: Raft and Paxos implementations exist in `src/deptran/raft/` and `src/deptran/paxos/`
- **Leader Election**: Raft has leader election with term-based voting
- **Log Replication**: Basic log replication via AppendEntries/Accept

### What's Missing
1. **State Recovery on Reboot**: Rebooted node starts with empty state
2. **Log Persistence**: Logs are in-memory only, lost on crash
3. **Snapshot Support**: No snapshot mechanism for fast recovery
4. **Client Leader Discovery**: Clients don't automatically discover new leader
5. **Partition Handling**: No handling of network partitions during recovery
6. **Transaction Recovery**: In-flight transactions not recovered after crash

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Client Layer                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐              │
│  │   Client    │  │   Client    │  │   Client    │              │
│  │  (Pool +    │  │  (Pool +    │  │  (Pool +    │              │
│  │  Failover)  │  │  Failover)  │  │  Failover)  │              │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘              │
└─────────┼────────────────┼────────────────┼─────────────────────┘
          │                │                │
          ▼                ▼                ▼
┌─────────────────────────────────────────────────────────────────┐
│                     Shard/Replica Layer                          │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    Shard 1 (Replica Group)               │    │
│  │  ┌─────────┐    ┌─────────┐    ┌─────────┐              │    │
│  │  │ Leader  │◄──►│Follower │◄──►│Follower │              │    │
│  │  │(Raft/   │    │ (Log    │    │ (Log    │              │    │
│  │  │ Paxos)  │    │  Sync)  │    │  Sync)  │              │    │
│  │  └────┬────┘    └────┬────┘    └────┬────┘              │    │
│  │       │              │              │                    │    │
│  │       ▼              ▼              ▼                    │    │
│  │  ┌─────────┐    ┌─────────┐    ┌─────────┐              │    │
│  │  │Persistent│   │Persistent│   │Persistent│              │    │
│  │  │  Log    │    │  Log    │    │  Log    │              │    │
│  │  │(RocksDB)│    │(RocksDB)│    │(RocksDB)│              │    │
│  │  └─────────┘    └─────────┘    └─────────┘              │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

## Implementation Phases

### Phase 1: Persistent Log Storage (~400 LOC)
Enable durable storage of Raft/Paxos logs so they survive crashes.

#### 1.1 Log Persistence Interface
- Create abstract `LogStorage` interface
- Methods: `append()`, `read()`, `truncate()`, `get_last_index()`, `get_term()`
- Support both in-memory (testing) and RocksDB backends

#### 1.2 RocksDB Log Backend
- Implement `RocksDBLogStorage` class
- Key format: `log:{partition_id}:{slot_id}`
- Value: serialized log entry (term, command, committed flag)
- Batch writes for performance

#### 1.3 Raft Integration
- Modify `RaftServer` to use `LogStorage` interface
- Persist: term, vote, log entries, commit index
- Load state on startup

#### 1.4 Paxos Integration
- Modify `PaxosServer` to use `LogStorage` interface
- Persist: max_ballot_seen_, accepted entries, committed entries

### Phase 2: State Recovery on Startup (~350 LOC)
Enable nodes to recover their state from persistent log after restart.

#### 2.1 Recovery Manager
- Create `RecoveryManager` class
- Detect if this is fresh start or recovery
- Coordinate recovery sequence

#### 2.2 Log Replay
- Replay committed log entries to rebuild state
- Apply entries in order up to commit index
- Verify state consistency

#### 2.3 Uncommitted Entry Handling
- Identify uncommitted entries from log
- For Raft: these will be resolved by leader
- For Paxos: run Paxos to determine commit status

#### 2.4 State Machine Recovery
- Rebuild transaction state from log
- Rebuild index structures
- Verify recovered state integrity

### Phase 3: Snapshot Support (~450 LOC)
Enable faster recovery by snapshotting state periodically.

#### 3.1 Snapshot Interface
- Create `SnapshotManager` class
- Methods: `take_snapshot()`, `load_snapshot()`, `list_snapshots()`
- Configurable snapshot interval

#### 3.2 Snapshot Format
- Define snapshot binary format
- Include: last_included_index, last_included_term, state machine data
- Compression support (optional)

#### 3.3 Snapshot Storage
- Store snapshots in RocksDB or separate files
- Retention policy (keep N most recent)
- Atomic snapshot writes

#### 3.4 Log Compaction
- Truncate log entries before snapshot
- Keep metadata for entries in snapshot range
- Coordinate compaction with replication

### Phase 4: Leader Election Enhancement (~300 LOC)
Improve leader election to handle crash scenarios better.

#### 4.1 Pre-Vote Protocol (Raft)
- Implement pre-vote to prevent disruption from partitioned nodes
- Node must get pre-votes before starting election
- Prevents term inflation from disconnected nodes

#### 4.2 Leader Lease
- Implement leader lease for linearizable reads
- Leader maintains lease through heartbeats
- Read-only operations served locally during lease

#### 4.3 Leadership Transfer
- Implement graceful leadership transfer
- Used before planned maintenance
- Ensure new leader is caught up before transfer

#### 4.4 Split-Brain Prevention
- Detect network partitions
- Ensure only one partition can elect leader (majority)
- Handle partition healing

### Phase 5: Client Failover (~350 LOC)
Enable clients to automatically failover to new leader.

#### 5.1 Leader Discovery
- Client queries any replica for current leader
- Cache leader info with TTL
- Retry logic on leader change

#### 5.2 Request Forwarding
- Non-leader replicas forward requests to leader
- Alternative: return leader hint to client
- Configurable behavior

#### 5.3 Failover Strategy
- Detect leader failure (connection error, timeout)
- Query replicas for new leader
- Retry with backoff

#### 5.4 Read Replica Support
- Option to read from followers (eventual consistency)
- Stale read detection
- Configurable consistency level

### Phase 6: In-Flight Transaction Recovery (~400 LOC)
Handle transactions that were in progress when crash occurred.

#### 6.1 Transaction Log Format
- Log transaction phases (prepare, commit, abort)
- Include participant list and vote status
- Durable transaction ID

#### 6.2 Coordinator Recovery
- Recover coordinator state from log
- Resume in-progress 2PC
- Timeout and abort stuck transactions

#### 6.3 Participant Recovery
- Recover participant state
- Query coordinator for transaction status
- Apply correct outcome (commit/abort)

#### 6.4 Orphan Transaction Cleanup
- Detect transactions with crashed coordinator
- Timeout and cleanup protocol
- Garbage collection

### Phase 7: Log Catchup Protocol (~350 LOC)
Efficient protocol for recovering nodes to catch up on missed logs.

#### 7.1 Incremental Log Sync
- Follower requests missing log entries
- Leader sends entries in batches
- Flow control for large backlogs

#### 7.2 Snapshot Transfer
- Transfer snapshot for very behind followers
- Chunked transfer for large snapshots
- Verify snapshot integrity

#### 7.3 Parallel Catchup
- Multiple shards catch up in parallel
- Prioritize critical shards
- Resource management

#### 7.4 Catchup Progress Tracking
- Track catchup progress per follower
- Expose metrics for monitoring
- Alerting on slow catchup

### Phase 8: Health Monitoring and Failure Detection (~300 LOC)
Detect node failures and trigger recovery.

#### 8.1 Heartbeat Enhancement
- Configurable heartbeat interval
- Adaptive timeout based on network conditions
- Distinguish slow from dead

#### 8.2 Failure Detector
- Phi accrual failure detector (or similar)
- Configurable sensitivity
- Avoid false positives

#### 8.3 Recovery Triggers
- Automatic recovery on failure detection
- Manual recovery option
- Recovery rate limiting

#### 8.4 Monitoring Integration
- Expose health metrics
- Recovery event logging
- Alerting hooks

### Phase 9: Testing (~500 LOC)
Comprehensive tests for all crash/recovery scenarios.

#### 9.1 Unit Tests
- Log persistence tests
- Recovery manager tests
- Snapshot tests
- Each component in isolation

#### 9.2 Integration Tests
- Single node crash and recovery
- Leader crash and election
- Follower crash and catchup
- Multiple node failures

#### 9.3 Stress Tests
- Repeated crash/recovery cycles
- Crash during log sync
- Crash during snapshot transfer
- High load during recovery

#### 9.4 Chaos Tests
- Random node kills
- Network partitions
- Combined failures
- Long-running stability

### Phase 10: Documentation (~100 LOC)
Document the crash recovery system.

#### 10.1 Architecture Documentation
- System design and rationale
- Component interactions
- Failure scenarios

#### 10.2 Operations Guide
- Configuration options
- Monitoring and alerting
- Manual recovery procedures

#### 10.3 API Documentation
- New configuration options
- Programmatic interfaces
- Error handling

## Dependencies

```
Phase 1 (Log Persistence) - No dependencies
Phase 2 (State Recovery) - Depends on Phase 1
Phase 3 (Snapshots) - Depends on Phase 1
Phase 4 (Leader Election) - No dependencies
Phase 5 (Client Failover) - Depends on Phase 4
Phase 6 (Txn Recovery) - Depends on Phase 1, 2
Phase 7 (Log Catchup) - Depends on Phase 1, 3
Phase 8 (Health Monitoring) - No dependencies
Phase 9 (Testing) - Parallel with all phases
Phase 10 (Documentation) - After implementation
```

## Success Criteria

1. **Data Durability**: No committed data lost on any single node failure
2. **Availability**: System remains available with minority failures
3. **Recovery Time**: Node recovers within configurable timeout (e.g., 30s)
4. **Correctness**: All invariants maintained during recovery
5. **Performance**: Recovery doesn't impact normal operation significantly
6. **Observability**: All recovery events logged and metricated

## Estimated Effort

| Phase | LOC | Tests | Description |
|-------|-----|-------|-------------|
| 1 | ~400 | 20 | Persistent Log Storage |
| 2 | ~350 | 25 | State Recovery |
| 3 | ~450 | 20 | Snapshot Support |
| 4 | ~300 | 15 | Leader Election Enhancement |
| 5 | ~350 | 20 | Client Failover |
| 6 | ~400 | 25 | In-Flight Transaction Recovery |
| 7 | ~350 | 20 | Log Catchup Protocol |
| 8 | ~300 | 15 | Health Monitoring |
| 9 | ~500 | 100 | Testing |
| 10 | ~100 | - | Documentation |
| **Total** | **~3500** | **260** | |

## RustyCpp Compliance

All new code must:
- Use rusty types (Box, Arc, Cell, RefCell, Option)
- Include @safe/@unsafe annotations
- Pass borrow checking
- Follow naming conventions (snake_case methods, UpperCamelCase types)
