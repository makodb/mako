# TPC-C Sharding Behavior Guide

This document describes the expected sharding behavior for TPC-C benchmark transactions
with Mako's warehouse-based sharding policy.

## Warehouse-Based Sharding Policy

With warehouse-based sharding, data is partitioned by warehouse ID (`w_id`):
- Each warehouse and its associated data (districts, customers, orders, stock) are co-located
- This enables data locality for transactions that operate within a single warehouse

### Distribution Example
```
10 warehouses, 2 shards:
  Shard 0: w_id 1-5 (WAREHOUSE, DISTRICT, CUSTOMER, STOCK, etc.)
  Shard 1: w_id 6-10
```

## Expected Remote Transaction Ratios

### NewOrder Transactions

Remote NewOrder transactions occur when one or more items have a supplier warehouse
on a different shard than the home warehouse.

**Factors:**
- `new_order_remote_item_pct`: Percentage of items sourced from remote warehouses (default: 1%)
- Items per order: 5-15 (average ~10) per TPC-C spec
- Number of shards: Determines probability of cross-shard supplier

**Expected Remote Ratio Formula:**
```
P(remote) = 1 - (1 - new_order_remote_item_pct * (1 - 1/num_shards))^avg_items
```

**Typical Values (2 shards, 1% remote item):**
- Expected: ~5-10% of NewOrder transactions are remote
- Actual observed: ~5.2% (from CI tests with 12 warehouses, 2 shards)

### Payment Transactions

Remote Payment transactions occur when the customer is from a different warehouse
than the payment warehouse (~15% per TPC-C spec).

**Expected Remote Ratio:**
- ~15% of Payment transactions select remote customer
- With 2 shards: ~7.5% will be cross-shard (50% of remote selections)
- Actual observed: ~8.3%

## Comparison: Table-ID vs Warehouse-Based Sharding

| Metric | Table-ID Sharding | Warehouse-Based |
|--------|------------------|-----------------|
| NewOrder remote ratio | ~50-100% | ~5-10% |
| Payment remote ratio | ~50-100% | ~7-8% |
| Remote abort ratio | Higher | Lower |
| Data locality | Poor | Excellent |
| Network overhead | High | Low |

### Why Table-ID Sharding Has High Remote Ratio
With table-ID sharding, tables are distributed round-robin across shards:
- WAREHOUSE on shard 0, DISTRICT on shard 1, STOCK on shard 2, etc.
- Every NewOrder must access WAREHOUSE, DISTRICT, STOCK, ORDER → multiple shards
- Cross-shard coordination required for every transaction

### Why Warehouse-Based Sharding Has Low Remote Ratio
With warehouse-based sharding:
- All data for warehouse 1-5 is on shard 0
- Transactions operating on a single warehouse are fully local
- Only cross-warehouse operations (remote items, remote customers) go cross-shard

## Metrics Available

The benchmark outputs these metrics for monitoring sharding effectiveness:

```
NewOrder_remote_ratio: 5.2%          # % of NewOrder that are remote
NewOrder_remote_abort_ratio: 1.9%   # % of remote NewOrder that aborted
NewOrder_local_commit_latency: 0.02 ms
NewOrder_remote_commit_latency: 10.2 ms  # Higher due to cross-shard coordination

Payment_remote_ratio: 8.3%
Payment_remote_abort_ratio: 0.2%
```

## Configuration

Sharding policy is configured in the shard config YAML:
```yaml
num_warehouses: 12
num_shards: 2
```

The policy is automatically initialized during TPC-C benchmark startup:
```
TPC-C Sharding: Initialized policy with 12 warehouses across 2 shards (version 1234567890)
```
