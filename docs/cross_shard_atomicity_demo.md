# Cross-Shard Atomicity Demo Status

The Phase 12 plan asks for a side-by-side demo:

1. Redis Cluster rejects a cross-slot transactional bank-transfer workload.
2. Mako preserves the account-sum invariant across the same cross-shard
   workload.

This artifact is not complete yet, but the fixture hooks now exist.

## Current Blocker

`tools/redis_compat/run_bank_transfer.py` now exists, but the current default
`makoCon` validation setup is single-shard/no-replication. Running the real
cross-shard demo still requires:

- the Redis Cluster fixture in `tools/redis_compat/fixtures/redis_cluster.sh`;
- a multi-shard Mako Redis fixture that keeps backing shard servers alive and
  sets `MAKO_G2_MULTI_SHARD=1`;
- an output format that records Redis Cluster's cross-slot rejection and Mako's
  final invariant check.

## Checked Blocker

Starting `makoCon` with multiple local shards is not enough. Point operations
route to backing shard RPC ports such as `31106` and `31206`. Starting those
ports through the existing `bash/shard.sh` / `dbtest` path is not a usable
Redis fixture because that wrapper is a benchmark runner, not a long-lived
Redis backing-shard service.

So the remaining G2 work is a real Redis-facing multi-shard service fixture,
not another pytest wrapper.

The bank-transfer harness has an opt-in local smoke mode:

```bash
MAKO_G2_ALLOW_SINGLE_SHARD=1 python3 tools/redis_compat/run_bank_transfer.py
```

That validates the transfer invariant through Redis `MULTI` on one `makoCon`
endpoint. It is not the cross-shard claim. Until a multi-shard Mako Redis
fixture exists, the Phase 12 acceptance runner reports G2 as `N/A`.
