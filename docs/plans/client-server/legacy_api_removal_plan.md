# Legacy Coroutine/Event API Removal Plan

## Overview
Remove backward-compatible aliases and fully migrate to Fiber API.

## Scope
- Replace `Coroutine::` with `Fiber::` in all source files
- Replace `AndEvent` with `WaitAll` in all source files
- Replace `OrEvent` with `WaitAny` in all source files
- Replace `NEvent` with `WaitN` in all source files
- Remove type aliases from `fiber_impl.h` and `fiber.h`

## Files to Modify

### Source Files with Coroutine:: (217 occurrences across 45 files)
Need to update `Coroutine::` → `Fiber::` in:
- src/run.cc
- src/deptran/s_main.cc
- src/deptran/carousel/scheduler.cc
- src/deptran/paxos/server.cc
- src/deptran/paxos/service.cc
- src/deptran/client_worker.cc
- src/deptran/rcc/server.cc
- src/deptran/copilot/service.cc
- src/deptran/copilot/coordinator.cc
- src/deptran/none/coordinator.cc
- src/deptran/helloworld_client/helloworld_impl.cc
- src/deptran/mongodb/server.h
- src/deptran/mencius/service.cc
- src/deptran/troad/scheduler.cc
- src/deptran/classic/coordinator.cc
- src/deptran/mencius/server.cc
- src/deptran/service.cc
- src/deptran/mencius/coordinator.cc
- src/deptran/raft/testconf.cc
- src/deptran/raft/service.cc
- src/deptran/raft/frame.cc
- src/deptran/raft/test.cc
- src/deptran/raft/server.cc
- src/deptran/fpga_raft/server.cc
- src/deptran/fpga_raft/coordinator.cc
- src/deptran/fpga_raft/service.cc
- src/srpc/reactor/event.cc
- src/srpc/reactor/reactor.cc
- src/srpc/reactor/quorum_event.cc
- src/srpc/rpc/client.cpp
- src/srpc/rpc/server.cpp
- src/srpc/rpc/server.hpp
- test/test_reactor_extended.cc
- test/test_reactor.cc
- test/coroutine.cc
- test/test_timeout_race.cc

### Source Files with AndEvent (48 occurrences across 16 files)
- src/deptran/communicator.cc (7)
- src/deptran/client_worker.h (1)
- src/deptran/communicator.h (3)
- test/test_and_event.cc (8)
- test/fiber_test.cc (3)
- src/srpc/reactor/event.h (6) - definition, keep but remove alias
- src/srpc/reactor/reactor.h (1)
- src/srpc/reactor/event.cc (1)
- src/srpc/reactor/fiber.h (1) - alias, remove

### Source Files with OrEvent (30 occurrences across 14 files)
- src/deptran/copilot/commo.h (1)
- test/coroutine.cc (1)
- test/fiber_test.cc (3)
- test/test_reactor_extended.cc (5)
- src/srpc/reactor/event.h (3) - definition, keep but remove alias
- src/srpc/reactor/reactor.h (1)
- src/srpc/reactor/event.cc (1)
- src/srpc/reactor/fiber.h (1) - alias, remove

### Source Files with NEvent (18 occurrences across 8 files)
- src/deptran/client_worker.h (1)
- test/fiber_test.cc (3)
- src/srpc/reactor/event.h (2) - definition, keep but remove alias
- src/srpc/reactor/fiber.h (1) - alias, remove

## Approach

### Phase 1: Update Source Files
1. Update all `Coroutine::` to `Fiber::` in source files
2. Update all `AndEvent` to `WaitAll` in source files
3. Update all `OrEvent` to `WaitAny` in source files
4. Update all `NEvent` to `WaitN` in source files

### Phase 2: Remove Aliases
1. Remove `using Coroutine = Fiber;` from fiber_impl.h
2. Remove `using WaitAll = AndEvent;` from fiber.h
3. Remove `using WaitAny = OrEvent;` from fiber.h
4. Remove `using WaitN = NEvent;` from fiber.h
5. Rename `AndEvent`, `OrEvent`, `NEvent` to `WaitAll`, `WaitAny`, `WaitN` in event.h

### Phase 3: Update Documentation
1. Update doc files to use new names

## Estimated LOC
- ~400 lines of changes across ~50 files
- Mostly search-and-replace with some structural changes

## Risk Assessment
- Breaking change for external code
- Internal code tested by CI

## Success Criteria
1. All Coroutine:: replaced with Fiber::
2. All AndEvent replaced with WaitAll
3. All OrEvent replaced with WaitAny
4. All NEvent replaced with WaitN
5. All aliases removed
6. All CI tests pass
7. Build succeeds
