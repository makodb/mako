# SRPC FD Leak Regression Proof

Date: 2026-04-10

## Scope
Prove the Workstream A fd-leak tests fail before the fd-close fixes and pass after them.

## Fix Commits
- `8237763d` (`rpc: close client fd in terminal states`)
- `003d65f3` (`rpc: avoid terminal state in mark_closing`)
- `201ed2f0` (`reactor: close pollables in closed-fd cleanup`)

## Pre-fix Validation (Fail)
- Baseline commit: `70bdb988` (immediately before `8237763d`)
- Injected test file from current branch: `test/rpc_state_integration_test.cc`
- Command:
  - `./build/test_rpc_state_integration --gtest_filter='StateIntegrationTest.RepeatedErrorReconnectCyclesDoNotIncreaseFdCount:StateIntegrationTest.StressFastConnectCloseCyclesDoNotIncreaseFdCount'`
- Result:
  - Exit code: `1`
  - `StateIntegrationTest.RepeatedErrorReconnectCyclesDoNotIncreaseFdCount` failed
    - observed fd growth (`cycle_fd_count`/`final_fd_count` above baseline bounds)
  - `StateIntegrationTest.StressFastConnectCloseCyclesDoNotIncreaseFdCount` failed
    - fd did not close within timeout on cycle 0

## Post-fix Validation (Pass)
- Commit: `da64b472` (`origin/mako-dev` at validation time)
- Command:
  - `./build/test_rpc_state_integration --gtest_filter='StateIntegrationTest.RepeatedErrorReconnectCyclesDoNotIncreaseFdCount:StateIntegrationTest.StressFastConnectCloseCyclesDoNotIncreaseFdCount'`
- Result:
  - Exit code: `0`
  - Both tests passed.

## Conclusion
The leak tests now provide true regression coverage: they fail on pre-fix behavior and pass on the fixed implementation.
