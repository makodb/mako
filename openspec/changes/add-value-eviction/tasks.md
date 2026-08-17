## 1. Test harness

The capability has no executable coverage today, so the harness comes
first: every task below is verified by it rather than by inspection.

- [ ] 1.1 Add a gtest target for the capability in `CMakeLists.txt`, linking rocksdb and the mako storage objects, and register it with ctest
- [ ] 1.2 Add a test fixture that opens a store on a temporary RocksDB directory and tears it down cleanly
- [ ] 1.3 Cover the existing write-back requirements: a `put` is visible to `get` immediately, and is present in RocksDB after `flush()` returns true
- [ ] 1.4 Cover deletion by tombstone: delete then read reports not-found, and re-inserting a deleted key succeeds
- [ ] 1.5 Cover existence reporting: `put` reports newly-inserted correctly, `insert` refuses a live key, `remove` refuses an absent key
- [ ] 1.6 Cover authoritative key residency: reopening a store over an existing RocksDB directory sees every prior key, with values starting non-resident
- [ ] 1.7 Cover ordered iteration: ascending and descending ranges, tombstones omitted, early stop honored, and a range spanning non-resident values

## 2. Capacity plumbing

- [ ] 2.1 Add a byte capacity to the store, defaulting to zero, and thread it through store open
- [ ] 2.2 Expose resident value bytes so tests can assert against the bound
- [ ] 2.3 Add a test asserting that with no capacity configured nothing is ever evicted

## 3. Sweeper

- [ ] 3.1 Implement a bounded CLOCK pass: walk a chunk from the cursor key, clear the reference bit on referenced values, evict unreferenced durable ones, and return bytes reclaimed
- [ ] 3.2 Wrap the cursor to the start of the keyspace when a chunk reaches the end, making it a clock hand
- [ ] 3.3 Add the sweeper thread, waking on capacity pressure and stopping cleanly on store close
- [ ] 3.4 Make a pass that reclaims nothing wait for flusher progress instead of re-sweeping immediately

## 4. Eviction safety

- [ ] 4.1 Verify `mrx_evict_value` refuses non-durable, non-resident, and tombstone values, and that its compare-and-swap loses to a concurrent write
- [ ] 4.2 Add a test that writes past capacity without flushing and asserts nothing is evicted and no write is lost
- [ ] 4.3 Add a test that evicts, then reads the evicted key back and asserts the value is unchanged
- [ ] 4.4 Add a concurrent test: writers, readers, and the sweeper on overlapping keys, asserting no torn or stale value is ever observed

## 5. Recency

- [ ] 5.1 Add a test that reads one key continuously while others idle under a forcing capacity, and asserts the hot key's value survives longest
- [ ] 5.2 Settle chunk size and sweeper idle interval against the tests, and record the chosen values

## 6. Wrap-up

- [ ] 6.1 Regenerate the DSL block with `scripts/regen_storage_dsl.sh` and confirm no drift with `--check`
- [ ] 6.2 Confirm the full gtest target passes, and record what was run
- [ ] 6.3 Update `docs/masstree-rocks-cache.md` to move eviction from planned to built, including the approximate-bound caveat
