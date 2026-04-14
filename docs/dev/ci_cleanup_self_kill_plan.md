# Plan: Prevent CI cleanup from killing its own process tree

## Goal
Avoid `ci/ci.sh` terminating itself (or parent shells) during cleanup when running commands like `./ci/ci.sh simpleTransaction`.

## Background
`cleanup_processes()` used `pgrep -f` for process names including the current command line (e.g., "simpleTransaction"), which can match the running `ci.sh` invocation and its ancestors. The loop then `kill -9`'d matching PIDs, causing the script to die with exit 137 before tests run.

## Plan
1. Add a helper to detect whether a PID is an ancestor of the current `ci.sh` process.
2. Switch the cleanup loop to use process substitution instead of a pipe (avoid subshell pitfalls).
3. Skip killing PID 1 and any ancestor PIDs; kill only true test processes.

## Test Plan
- Run `make clean` then `make -j32`.
- Run `./ci/ci.sh all` and confirm it completes.
