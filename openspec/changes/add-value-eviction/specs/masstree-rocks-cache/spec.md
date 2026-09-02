## ADDED Requirements

### Requirement: Bounded Value Memory

The cache SHALL accept a byte capacity for its value tier and SHALL
reclaim value bytes to stay within it. Keys and their entries SHALL NOT
be reclaimed, so the memory floor is the size of the keyspace rather
than the capacity.

#### Scenario: Writing past the capacity
- **WHEN** more value bytes are written than the configured capacity
  allows, and those writes have been persisted
- **THEN** resident value bytes fall back to at or below the capacity
- **AND** every key remains present and readable

#### Scenario: Reading an evicted value
- **WHEN** a value that was evicted is read again
- **THEN** it is fetched from the system of record and returned
  unchanged

#### Scenario: Capacity smaller than the keyspace
- **WHEN** the capacity is set below the memory the keys alone require
- **THEN** all values may be evicted but the store continues to serve
  reads and writes correctly

#### Scenario: No capacity configured
- **WHEN** no capacity is set
- **THEN** no eviction occurs and every value stays resident

### Requirement: Durable-Only Eviction

A value SHALL be eligible for eviction only once it is present in the
system of record. Eviction SHALL NOT discard the only copy of an
acknowledged write.

#### Scenario: Value not yet persisted
- **WHEN** the sweeper encounters a value whose write has not reached
  RocksDB
- **THEN** that value is not evicted

#### Scenario: Eviction racing a write
- **WHEN** a write publishes a new value while the sweeper is evicting
  the old one
- **THEN** the new value is not evicted by that pass
- **AND** the new value remains readable

#### Scenario: Every resident value is non-durable
- **WHEN** resident bytes exceed capacity but no value is yet durable
- **THEN** the sweeper reclaims nothing and yields to the flusher
- **AND** no acknowledged write is dropped to satisfy the capacity

### Requirement: Recency-Biased Reclamation

Reclamation SHALL prefer values that have not been accessed recently,
so that repeatedly read values tend to stay resident.

#### Scenario: Hot key survives a sweep
- **WHEN** one key is read continuously while others are idle, and the
  capacity forces reclamation
- **THEN** the continuously read key's value is among the last evicted

## MODIFIED Requirements

### Requirement: Value Residency And Read-Through

A key's value MAY be non-resident, meaning its bytes live only in
RocksDB. A value becomes non-resident either at startup, when the
keyspace is loaded, or later, when it is reclaimed to respect the
configured capacity. `get` SHALL fetch a non-resident value from
RocksDB and install it, and an install SHALL lose to any newer write.

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
