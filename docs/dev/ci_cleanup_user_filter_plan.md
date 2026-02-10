# Plan: Scope CI cleanup and hanging-process checks to current user

## Goal
Prevent CI from failing due to other users' `dbtest` processes on shared machines.

## Problem
`check_for_hanging_processes` and `cleanup_processes` scan/kill all `dbtest` processes system-wide. On shared hosts this flags unrelated processes as hanging and causes CI failures.

## Plan
1. Filter `pgrep`/`ps`/`pkill` to the current user only.
2. Keep existing cleanup behavior and logging, but avoid touching other users' processes.

## Test Plan
- Run `make clean` and `make -j32`.
- Run `./ci/ci.sh all` and confirm `clientServer`/`simplePaxos` no longer fail due to unrelated `dbtest` processes.
