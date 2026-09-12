# masstree-rocks-cache Specification

## Purpose

Serve an ordered key-value store whose system of record is RocksDB and
whose accelerator is an in-memory Masstree, so that writes acknowledge
at memory speed and reads of hot data never touch disk. The cache is
deliberately isolated: no transaction runtime, no replication, no
sharding.

Two properties define it. A write acknowledges **before** it is
durable, trading a bounded loss window for latency. Masstree indexes
the **whole keyspace**, so absence is answerable in memory and the two
tiers never have to be reconciled to decide whether a key exists.

## Requirements

### Requirement: Ordered Index Interface

The cache SHALL implement the `OrderedIndex` trait so it is
substitutable for any other storage backend, and SHALL NOT implement
`TxnOrderedIndex` or `ShardParticipant`, because it has no transaction
runtime.

#### Scenario: Substituted for another backend
- **WHEN** a caller holds the cache through an `OrderedIndex` reference
- **THEN** `get`, `put`, `insert`, `remove`, `scan`, `rscan`, `size`,
  `clear`, `get_table_id`, and `get_is_remote` are all available
- **AND** values are exchanged as raw bytes in both directions

### Requirement: Write-Back Acknowledgement

A write SHALL be acknowledged once it is applied in Masstree, without
waiting for RocksDB. A background flusher SHALL move acknowledged
writes into RocksDB afterwards.

#### Scenario: Write returns before durability
- **WHEN** a caller issues `put`
- **THEN** the call returns as soon as the value is published in memory
- **AND** the value is visible to a subsequent `get` immediately
- **AND** the write reaches RocksDB at some later point

#### Scenario: Crash inside the acknowledgement window
- **WHEN** the process dies after a `put` returns but before the
  flusher has persisted it
- **THEN** that write is lost
- **AND** this is the accepted trade, not a defect

### Requirement: Durability Barrier

The cache SHALL expose `flush()`, which blocks until every write
acknowledged before the call is present in RocksDB, and SHALL report
whether it achieved that.

#### Scenario: Successful barrier
- **WHEN** a caller issues writes and then calls `flush()`
- **THEN** the call blocks until all of those writes are in RocksDB
- **AND** returns true

#### Scenario: Barrier cannot be satisfied
- **WHEN** a RocksDB write fails, or the store is shutting down
- **THEN** `flush()` returns false rather than blocking forever
- **AND** the affected values remain marked non-durable so they are
  never discarded

#### Scenario: Writes racing the barrier
- **WHEN** another thread issues a write after `flush()` has been
  entered
- **THEN** the barrier does not wait for that write

### Requirement: Authoritative Key Residency

Masstree SHALL hold an entry for every key present in RocksDB. A
Masstree miss SHALL therefore be treated as authoritative absence, and
no operation SHALL consult RocksDB to decide whether a key exists.

#### Scenario: Reading a key that does not exist
- **WHEN** `get` is called for a key with no Masstree entry
- **THEN** the result is "not found" without any RocksDB access

#### Scenario: Opening a database with existing rows
- **WHEN** the store opens a RocksDB directory that already contains
  rows
- **THEN** every key is loaded into Masstree before any operation runs
- **AND** each loaded value starts non-resident
- **AND** open cost is proportional to the size of the keyspace

#### Scenario: Opening an empty database
- **WHEN** the store opens an empty RocksDB directory
- **THEN** startup performs no key loading work

### Requirement: Value Residency And Read-Through

A key's value MAY be non-resident, meaning its bytes live only in
RocksDB. `get` SHALL fetch a non-resident value from RocksDB and
install it, and an install SHALL lose to any newer write.

#### Scenario: Reading a non-resident value
- **WHEN** `get` finds an entry whose value is not resident
- **THEN** the value is read from RocksDB, installed, and returned

#### Scenario: Install racing a concurrent write
- **WHEN** a write publishes a new value while a read-through fill is
  in flight
- **THEN** the fill does not overwrite the newer value
- **AND** this holds even if the newer value was itself already
  persisted and made non-resident again before the fill completes

#### Scenario: Row missing from the system of record
- **WHEN** an entry claims a key exists but RocksDB has no row for it
- **THEN** `get` reports "not found" rather than inventing a value

### Requirement: Existence-Reporting Writes

`put` SHALL report whether the key was newly inserted, `insert` SHALL
apply only when the key is absent, and `remove` SHALL report whether
the key existed. Each SHALL be atomic with respect to concurrent
writers of the same key.

#### Scenario: Overwriting an existing key
- **WHEN** `put` is called for a key that is currently live
- **THEN** the value is replaced and the call reports "not newly
  inserted"

#### Scenario: Insert on a live key
- **WHEN** `insert` is called for a key that is currently live
- **THEN** no write is applied and the call reports false

#### Scenario: Concurrent inserts of the same absent key
- **WHEN** two threads `insert` the same absent key simultaneously
- **THEN** exactly one reports true

#### Scenario: Remove on an absent key
- **WHEN** `remove` is called for a key that is absent or deleted
- **THEN** no write is applied and the call reports false

### Requirement: Deletion By Tombstone

A delete SHALL publish a tombstone rather than erasing the key from
Masstree, and the tombstone entry SHALL be retained.

#### Scenario: Read after delete
- **WHEN** a key is deleted and then read
- **THEN** the result is "not found"
- **AND** the still-present RocksDB row is never consulted

#### Scenario: Reinserting a deleted key
- **WHEN** `insert` is called for a key holding a tombstone
- **THEN** the insert succeeds and reports true

#### Scenario: Counting after deletes
- **WHEN** `size` is called on a store containing tombstones
- **THEN** the count includes them and therefore overcounts live keys

### Requirement: Ordered Range Iteration

`scan` and `rscan` SHALL iterate the Masstree alone, emitting resident
values directly, fetching non-resident ones from RocksDB, and skipping
tombstones. Iteration SHALL be chunked so no RocksDB read occurs inside
a Masstree range traversal.

#### Scenario: Ascending range
- **WHEN** `scan` is called over `[start, end)`
- **THEN** live keys in that range are delivered in ascending order
- **AND** deleted keys are omitted

#### Scenario: Range covering non-resident values
- **WHEN** a scanned range includes keys whose values are not resident
- **THEN** those values are fetched and delivered like any other

#### Scenario: Caller stops early
- **WHEN** the scan callback returns false
- **THEN** iteration stops and no further keys are delivered

### Requirement: Version-Exact Durability Marking

The flusher SHALL mark a value durable only if that exact value version
is still the published one, so that durability earned by one version is
never attributed to another.

#### Scenario: Write superseded mid-flush
- **WHEN** the flusher persists version N of a key while a writer
  publishes version N+1
- **THEN** version N+1 is not marked durable
- **AND** version N+1 is persisted by its own later flush

### Requirement: Truncation

`clear` SHALL remove all data from both tiers, because discarding only
the in-memory tier would contradict authoritative key residency.

#### Scenario: Clearing the store
- **WHEN** `clear` is called
- **THEN** both the Masstree entries and the RocksDB rows are removed
- **AND** a subsequent `get` for any previously stored key reports
  "not found"
