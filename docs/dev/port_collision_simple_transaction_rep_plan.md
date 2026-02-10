# Plan: Avoid simpleTransactionRep port collisions in CI

## Goal
Ensure simpleTransactionRep-based CI scripts (Paxos/Raft simple replication) run on randomized port ranges to avoid bind errors.

## Problem
`simpleTransactionRep` always loads a fixed config file (e.g., `local-shards2-warehouses6.yml`) with static ports (e.g., 17001). When CI runs multiple tests or retries, the fixed port can already be bound, causing Raft replication tests to abort.

## Plan
1. Add `MAKO_CONFIG` override support in `examples/simpleTransactionRep.cc` (match `simpleTransaction.cc`).
2. Create a shared script helper to generate a temp config with randomized port base.
3. Update CI-invoked simpleTransactionRep test scripts to set `MAKO_CONFIG` via the helper.

## Test Plan
- Run `make clean` and `make -j32`.
- Run `./ci/ci.sh all` and verify all tests pass.
