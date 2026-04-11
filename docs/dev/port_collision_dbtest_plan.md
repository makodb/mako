# Plan: Avoid dbtest port collisions in shard scripts

## Goal
Ensure shard-based dbtest scripts use randomized port bases to avoid bind conflicts on shared hosts.

## Problem
`bash/shard.sh` always uses static config files with fixed ports (e.g., 31111/31112). On shared machines, those ports can already be occupied, causing shardNoReplication and replication tests to abort.

## Plan
1. Allow `bash/shard.sh` to honor `MAKO_CONFIG` when provided.
2. Update CI-invoked shard scripts to generate a temp config with randomized ports and export `MAKO_CONFIG`.

## Test Plan
- Run `make clean` and `make -j32`.
- Run `./ci/ci.sh all` and confirm shardNoReplication no longer fails due to port bind errors.
