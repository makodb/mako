## Context

See proposal.md — Why. The relevant current state is that the pieces
eviction needs already exist and are inert: each value record carries a
`durable` flag, each entry carries a `referenced` bit, the store keeps a
`resident_bytes` counter, and `mrx_evict_value` performs one safe
eviction via compare-and-swap. Nothing drives them.

Two properties of the existing design constrain the approach. Entries
are stable, allocated once per key and never moved, so eviction never
touches the tree structure. And an evicted value is not a null pointer
but a versioned record with `resident = 0`, which is what keeps a
read-through fill from resurrecting stale bytes.

## Goals / Non-Goals

Goals beyond the spec: eviction must not add a lock to the read or
write path, and a sweep must not hold an RCU region for the length of
the keyspace.

Non-goals at the design level: exact LRU ordering (a global recency
structure would serialize reads, which is the opposite of what this
storage engine is for), and any attempt to bound total memory —
capacity governs value bytes only, and the keyspace floor is a
consequence of the key-resident invariant, not something this change
can fix.

## Decisions

**CLOCK sweep over a tree cursor, not an LRU list.** A linked LRU list
demands a global lock on every read to reorder it, which would make
reads contend where they currently do not. CLOCK approximates recency
with a single per-entry bit that readers set with a relaxed store and
the sweeper clears as it passes. Alternative considered: sampling K
random entries and evicting the coldest. Rejected because Masstree has
no cheap random-position access, whereas it does have ordered range
iteration, which is exactly what a rotating cursor needs.

**Sweep in bounded chunks, resuming from a stored cursor key.** The
same chunking the scan path already uses, for the same reason: a full
keyspace pass inside one RCU region would pin reclamation for its
duration. The cursor is a key, not an iterator, so it stays valid
across chunks even as the tree changes; when it runs off the end it
wraps to the beginning, which is what makes it a clock hand.

**A dedicated sweeper thread, woken by pressure.** Alternatives were to
evict inline on the write path, or to fold sweeping into the flusher.
Inline eviction would put a RocksDB-dependent decision in the write
path and destroy the write-back latency this cache exists to provide.
Folding it into the flusher would couple reclamation progress to write
volume — a store under heavy read-fill pressure and no writes would
never reclaim.

**Second chance on the reference bit.** A value whose `referenced` bit
is set is not evicted; the bit is cleared instead, and the value
becomes eligible on the next pass. This is what makes a continuously
read key survive, and it costs readers one relaxed store.

**Capacity is a store-open parameter, and zero means unbounded.** This
preserves today's behavior by default, so the change cannot regress any
existing caller.

## Risks / Trade-offs

**Sweeper spins when nothing is evictable** (everything resident is
non-durable) → the sweep reports how many bytes it reclaimed; a pass
that reclaims nothing waits for the flusher to make progress rather
than immediately re-sweeping.

**Eviction and read-through fill can thrash** on a working set larger
than capacity: evict, refill from disk, evict again → accepted, and
inherent to any cache under a too-small capacity. The reference bit
limits it by protecting whatever is actually hot.

**`resident_bytes` is maintained with relaxed atomics across
concurrent swaps** → it is an accounting estimate, not a hard bound;
the capacity is enforced approximately. Making it exact would require
serializing publishes, which is not worth it.

**A very large chunk of the keyspace can be swept between capacity
checks**, briefly over-evicting → keep chunks small and re-check
resident bytes each chunk.

## Migration Plan

No migration. Capacity defaults to unbounded, so a store that does not
opt in behaves exactly as it does today, and no persisted format
changes — eviction only affects what is held in memory.

Rollback is to construct the store without a capacity.

## Open Questions

None that block implementation. Chunk size and the sweeper's idle
interval are tunables to be settled empirically once the tests exist;
neither changes the specs, the approach, or the task breakdown.
