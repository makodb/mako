# Plan: Fix RPC stress crash port collisions

## Goal
Reduce test flakiness in `test_rpc_stress_crash` by avoiding hard-coded or reserved port ranges that can collide across parallel CI processes.

## Approach
- Replace fixed port allocation with per-test dynamic port selection.
- Add a retrying server start helper that selects a new port when bind fails.
- Update tests that restart servers to refresh addresses after retries.
- Keep the behavior of existing tests (crash, restart, storm, metrics) intact.

## Rationale
Port collisions caused intermittent bind errors in CI when multiple test processes ran in parallel. Retrying with a new port keeps tests functionally equivalent while avoiding flaky failures.

## User Impact
No runtime changes. Only unit tests are more stable under parallel CI execution.
