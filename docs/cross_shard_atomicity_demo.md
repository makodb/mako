# Cross-Shard Atomicity Demo Status

The Phase 12 side-by-side demo is now executable with:

```bash
third-party/redis/compat/run_cross_shard_demo.py
```

## Latest Captured Result

Captured: 2026-06-24 UTC

| System | Result |
|---|---|
| Redis Cluster | Rejects cross-slot transactional transfer: `CrossSlotTransactionError: All keys involved in a cluster transaction must map to the same slot` |
| Mako Redis fixture | Preserves bank invariant: `bank transfer invariant preserved runs=3 transfers=190615 retries=0` |

## Scope

The Mako side uses `third-party/redis/compat/fixtures/makocon_multishard.sh`,
which starts one Redis-facing `makoCon` process with `MAKO_NUM_SHARDS=3` and
`MAKO_LOCAL_SHARDS=0,1,2`. That exercises Redis transactions over Mako's
sharded table routing in one process.

This is the stable multi-shard Redis fixture available in this PR. The separate
`dbtest`/`bash/shard.sh` paths are benchmark/replication runners, not
Redis-serving backing services, so they are not used as the G2 Redis
compatibility target. This fixture is not a replicated failover fixture.

## Local Smoke

Single-shard smoke remains available:

```bash
MAKO_G2_ALLOW_SINGLE_SHARD=1 python3 third-party/redis/compat/run_bank_transfer.py
```
