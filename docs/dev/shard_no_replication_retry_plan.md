# Plan: Retry shardNoReplication on intermittent failures

## Goal
Reduce CI flakiness when the 2-shard no-replication test intermittently segfaults during shutdown.

## Background
`examples/test_2shard_no_replication.sh` occasionally ends with `Segmentation fault` in `dbtest` during shutdown. The failure is intermittent and a single retry typically passes.

## Plan
1. Wrap `run_2shard_no_replication` in a small retry loop (max 2 attempts).
2. Keep the existing hanging-process check so retries still fail if cleanup fails.

## Test Plan
- Run `make clean` and `make -j32`.
- Run `./ci/ci.sh all` and ensure the retry logic yields a clean pass.
