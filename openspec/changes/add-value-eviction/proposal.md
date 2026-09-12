## Why

The cache currently has no upper bound on memory. Every value ever
written or read stays resident, so the "cache" is really a write buffer
that grows until the process dies. The key-resident redesign was
adopted specifically to make eviction cheap and safe — eviction swaps a
pointer inside a stable entry instead of removing a key from the tree —
but the policy that would actually reclaim anything was never built.

The supporting machinery is already in place and unused: values carry a
durability flag, entries carry a CLOCK reference bit, the store tracks
resident bytes, and a single-value eviction kernel exists. What is
missing is a capacity, a sweeper to enforce it, and evidence that any
of it is correct.

## What Changes

- Add a configurable byte capacity for the value tier. Keys and their
  entries remain resident unconditionally; only value bytes are
  reclaimed.
- Add a background sweeper that evicts durable values using a CLOCK
  (second-chance) pass over the tree, and that engages only when
  resident bytes exceed capacity.
- Refuse to evict any value that is not durable, so eviction can never
  discard the only copy of an acknowledged write.
- Define the behavior when the cache is saturated with non-durable
  values: the sweeper makes no progress and defers to the flusher
  rather than dropping data or spinning.
- Add the first executable test coverage for this capability — the
  eviction requirements plus the existing correctness traps
  (write-back visibility, tombstone reads, version-exact durability
  marking, fill-versus-write races), which have been argued correct in
  review but never run.

Not in scope: reclaiming tombstone entries, evicting keys, batching
adjacent non-resident keys during scans, and partitioned flushers.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `masstree-rocks-cache`: adds bounded value memory and the eviction
  safety rules. The existing "Value Residency And Read-Through"
  requirement gains a scenario, because eviction becomes a second way a
  value can become non-resident — until now that could only happen at
  startup.

## Impact

- `src/mako/storage/masstree_rocks_index.hh` — the DSL source block
  gains a capacity parameter and sweeper control; the `mrx_*` kernels
  gain the sweeper thread and the CLOCK pass. `mrx_evict_value` already
  exists and is the primitive the sweeper drives.
- `docs/masstree-rocks-cache.md` — S3 stage notes move from planned to
  built.
- `CMakeLists.txt` — a new gtest target for the capability.
- New test source under the project's existing gtest layout.
- No change to the `OrderedIndex` interface, so no caller is affected.
- Regeneration required: `scripts/regen_storage_dsl.sh` after any edit
  to the DSL block.
