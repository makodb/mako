# Plan: Retry test_rpc port selection on bind failures

## Goal
Reduce flaky failures in `test_rpc` caused by transient port collisions.

## Problem
`test_rpc` uses a per-process incrementing port allocator. When a chosen port is already in use by another process, `Server::start()` fails and the test aborts.

## Plan
1. In `RPCTest::SetUp`, attempt to start the server on a newly allocated port.
2. If the bind fails, delete the server and retry with another port (bounded attempts).
3. Proceed with client setup only after a successful bind.

## Test Plan
- Run `make clean` and `make -j32`.
- Run `./ci/ci.sh all` and ensure `test_rpc` passes.
