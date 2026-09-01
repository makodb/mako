# Glossary

## 1. Raft-Specific Terms

| Term | Definition |
|------|------------|
| **Term** | A monotonically increasing integer that identifies a period of leadership.  Each election increments the term.  All Raft RPCs include the sender's term; a server with a stale term steps down. |
| **Log Index** | The position of an entry in the Raft log.  Indices start at 1 and increment sequentially.  Also called `slot_id` in the implementation. |
| **Commit Index** | The highest log index known to be committed (replicated to a majority).  Once committed, an entry is safe to apply to the state machine. |
| **Match Index** | Per-follower array maintained by the leader.  `matchIndex[i]` is the highest log index known to be replicated on follower `i`.  Used to advance the commit index. |
| **Next Index** | Per-follower array maintained by the leader.  `nextIndex[i]` is the next log index to send to follower `i`.  Initialised to leader's last log index + 1; decremented on AppendEntries rejection. |
| **Election Timeout** | Duration a follower waits before starting an election.  Randomised to prevent split votes.  In tests: base 5,000 ms with random jitter. |
| **Heartbeat** | An empty AppendEntries RPC sent by the leader to maintain authority and prevent elections.  Interval: typically 150 ms in tests. |
| **Leader** | The server that handles all client requests, replicates log entries, and decides when entries are committed.  At most one leader per term. |
| **Follower** | A passive server that responds to RPCs from the leader.  Does not initiate requests.  Becomes a candidate if the election timeout expires. |
| **Candidate** | A server running for leader.  Increments its term, votes for itself, and sends RequestVote RPCs to all other servers. |
| **RequestVote** | RPC sent by a candidate to request a vote.  Arguments: term, candidateId, lastLogIndex, lastLogTerm.  A server grants a vote if the candidate's log is at least as up-to-date. |
| **AppendEntries** | RPC sent by the leader to replicate log entries and serve as heartbeats.  Arguments: term, leaderId, prevLogIndex, prevLogTerm, entries[], leaderCommit. |
| **TimeoutNow** | RPC used for leadership transfer.  The current leader sends this to a preferred replica, causing it to immediately start an election without waiting for election timeout. |
| **Preferred Leader** | A designated server that should become leader.  Has a shorter election timeout, and the current leader uses TimeoutNow to transfer leadership to it.  Implemented via `IS_PREFERRED_LEADER` flag. |
| **NO-OP** | A no-operation log entry committed by a new leader to establish its authority.  Contains no application data but ensures all previous-term entries are committed. |
| **Log Compaction** | Removing log entries that have been applied to the state machine and captured in a snapshot.  Bounds log growth.  See `CompactLog()`. |

## 2. Mako-Specific Terms

| Term | Definition |
|------|------------|
| **Shard** | A horizontal partition of data.  Each shard has its own set of replicas and runs an independent consensus group.  TPC-C warehouses are distributed across shards. |
| **Partition** | A sub-division within a shard.  Each partition is a unit of data ownership.  A shard with 6 threads typically has 6 partitions. |
| **Partition Group** | The set of replicas responsible for a single partition.  In a 3-replica Raft cluster with 6 partitions, there are 6 partition groups of 3 replicas each. |
| **Watermark** | A progress indicator used by Mako's speculative execution engine.  The replication watermark tracks how far the follower's replay has progressed.  Used in `raft_worker.cc` for batching decisions. |
| **Epoch** | In Multi-Paxos, the equivalent of Raft's term.  Identifies a period of leadership.  Stored as `cur_epoch` in metadata. |
| **Speculative Execution** | Mako's key optimisation: executes transactions speculatively before replication completes.  Results are committed only after the replication layer confirms durability. |
| **agg_persist_throughput** | Aggregate persisted transaction throughput in ops/sec.  Calculated as total committed transactions / measured runtime.  The primary benchmark metric. |
| **replay_batch** | Count of replication batches processed by a follower.  Higher count indicates more frequent, smaller batches.  Raft: ~3,674 vs Paxos: ~669 in 1-shard TPC-C. |
| **Learner** | A non-voting replica in Multi-Paxos that receives committed entries for read scaling or backup.  Raft does not use learners in this implementation. |
| **Preferred Replica** | Same as preferred leader — the designated server that should hold leadership for deterministic placement. |

## 3. Transaction Terms

| Term | Definition |
|------|------------|
| **TPC-C** | Transaction Processing Performance Council benchmark C.  Industry-standard OLTP benchmark modelling a wholesale supplier with 5 transaction types. |
| **NewOrder** | TPC-C transaction (45% of mix).  Creates a new order with multiple line items.  Can be cross-shard if items are on different shards. |
| **Payment** | TPC-C transaction (43% of mix).  Processes a customer payment.  Can be cross-shard if customer and warehouse are on different shards. |
| **Delivery** | TPC-C transaction (4% of mix).  Delivers pending orders in batch.  Single-shard only. |
| **OrderStatus** | TPC-C transaction (4% of mix).  Read-only query of order status. |
| **StockLevel** | TPC-C transaction (4% of mix).  Read-only check of stock levels below threshold. |
| **OCC** | Optimistic Concurrency Control.  Transactions execute without locks, then validate at commit time.  Aborts if conflicts detected. |
| **2PC** | Two-Phase Commit.  Protocol for coordinating cross-shard transactions.  Phase 1: prepare (lock resources).  Phase 2: commit or abort. |
| **Abort Ratio** | Percentage of transaction attempts that are aborted (local conflicts or remote conflicts).  Higher in multi-shard configurations due to cross-shard contention. |
| **Commit Latency** | Time from transaction start to successful commit, in milliseconds.  Measured per transaction type. |

## 4. System and Infrastructure Terms

| Term | Definition |
|------|------------|
| **srpc** | The custom RPC framework used by Mako.  Provides TCP/IP-based remote procedure calls with ~10-50 us latency on localhost. |
| **eRPC** | High-performance RDMA-based RPC backend.  ~1-2 us latency.  Optional alternative to srpc.  Not used in CI benchmarks. |
| **DPDK** | Data Plane Development Kit.  Kernel-bypass networking library.  Supported by the legacy Deptran transport layer. |
| **Masstree** | High-performance in-memory concurrent B-tree index.  Used by Mako as the primary storage engine for transaction data. |
| **RocksDB** | Facebook's persistent key-value store based on LSM trees.  Used for Raft/Paxos log persistence via `RocksDBLogStorage`. |
| **jemalloc** | A memory allocator designed for multi-threaded applications.  Used by Mako for reduced allocation overhead and better cache behaviour. |
| **RustyCpp** | A C++ library providing Rust-style smart pointers and safety annotations.  Used for memory safety: `rusty::Box`, `rusty::Arc`, `rusty::Cell`, `rusty::Option`. |
| **Marshal** | The srpc framework's serialization format.  `to_marshal()` and `from_marshal()` convert structs to/from wire format for RPC. |
| **dbtest** | The main Mako test binary.  Runs TPC-C benchmark with configurable replication (Raft/Paxos), shards, and threads. |
| **simpleRaft** | Standalone Raft replication test binary.  Submits fixed-size log entries without transaction processing. |
| **simpleTransactionRepRaft** | Simple key-value transaction test binary with Raft replication and data integrity verification. |
| **GDB** | GNU Debugger.  `MAKO_NO_GDB=1` is set in CI to disable GDB wrapping, which would break log parsing. |
| **Coroutine** | Cooperative multitasking primitive used by the srpc framework.  Raft test cases run as coroutines via `RAFT_TEST_CORO` and `Reactor::CreateSpEvent()`. |
| **Fiber** | Alternative name for coroutine in parts of the codebase.  `Fiber::create_run()` creates and runs a new coroutine. |

## 5. Persistence Terms

| Term | Definition |
|------|------------|
| **WAL** | Write-Ahead Log.  RocksDB's mechanism for crash recovery.  Writes are logged to WAL before being applied to memtables. |
| **fsync** | System call that flushes file data and metadata to disk.  Used by RocksDBLogStorage (`sync = true`) and FileSnapshotManager to ensure durability. |
| **WriteBatch** | RocksDB feature for atomic multi-key writes.  Used by `put_batch()` and `remove_range()` to ensure log consistency. |
| **Snapshot** | A point-in-time capture of the state machine.  Allows log entries before the snapshot index to be discarded.  Binary format: 52-byte header + data + CRC32. |
| **CURRENT file** | RocksDB's manifest pointer file.  Its presence indicates a valid, openable database.  Used by `detect_mode()` to distinguish `FRESH_START` from `NORMAL_RECOVERY`. |
