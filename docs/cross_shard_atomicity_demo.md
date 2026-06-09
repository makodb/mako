# Cross-Shard Atomicity Demo Status

The Phase 12 plan asks for a side-by-side demo:

1. Redis Cluster rejects a cross-slot transactional bank-transfer workload.
2. Mako preserves the account-sum invariant across the same cross-shard
   workload.

This artifact is not complete yet.

## Current Blocker

`tools/redis_compat/run_bank_transfer.py` now exists, but the current default
`makoCon` validation setup is single-shard/no-replication. Running the real
cross-shard demo still requires:

- a Redis Cluster test fixture;
- a multi-shard Mako Redis fixture;
- an output format that records Redis Cluster's cross-slot rejection and Mako's
  final invariant check.

The bank-transfer harness has an opt-in local smoke mode:

```bash
MAKO_G2_ALLOW_SINGLE_SHARD=1 python3 tools/redis_compat/run_bank_transfer.py
```

That validates the transfer invariant through Redis `MULTI` on one `makoCon`
endpoint. It is not the cross-shard claim. Until the cluster fixtures exist,
the Phase 12 acceptance runner reports G2 as `N/A`.
