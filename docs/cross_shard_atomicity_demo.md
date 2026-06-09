# Cross-Shard Atomicity Demo Status

The Phase 12 plan asks for a side-by-side demo:

1. Redis Cluster rejects a cross-slot transactional bank-transfer workload.
2. Mako preserves the account-sum invariant across the same cross-shard
   workload.

This artifact is not complete yet.

## Current Blocker

The checkout does not contain the plan-named
`tools/redis_compat/run_bank_transfer.py` G2 harness, and the current
`makoCon` validation setup is single-shard/no-replication. Running a real demo
requires:

- a Redis Cluster test fixture;
- a multi-shard Mako Redis fixture;
- the G2 bank-transfer workload script;
- an output format that records Redis Cluster's cross-slot rejection and Mako's
  final invariant check.

Until those exist, the Phase 12 acceptance runner reports G2 as `N/A` rather
than claiming the cross-shard property.
