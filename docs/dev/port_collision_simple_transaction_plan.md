# Plan: Reduce port collisions in simpleTransaction and RPC client pool tests

## Goal
Prevent CI and local test flakiness caused by fixed ports (e.g., 31000, 1798x) being in use.

## Approach
- Add a retrying server start helper in `test/rpc_client_pool_test.cc` to pick a new port when bind fails.
- Allow `examples/simpleTransaction.cc` to read an override config path from `MAKO_CONFIG`.
- Update `ci/ci.sh` to generate a temporary config with a free port base for simpleTransaction before running the test.

## Rationale
Parallel test processes or stray local services can occupy fixed ports. Retrying or offsetting the ports keeps behavior unchanged while making tests robust.

## User Impact
No changes to default behavior unless `MAKO_CONFIG` is set. CI becomes less flaky.
