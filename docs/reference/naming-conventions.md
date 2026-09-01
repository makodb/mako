# SRPC Naming Convention Migration Analysis

## Overview

This document analyzes the migration of srpc code from C++ naming conventions to Rust naming conventions.

## Target Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Types/Classes | UpperCamelCase | `Event`, `Reactor`, `Coroutine` |
| Methods | snake_case | `is_ready`, `get_reactor`, `create_run` |
| Constants | SCREAMING_SNAKE_CASE | `MAX_EVENTS`, `POLL_TIMEOUT` |
| Variables | snake_case | `event_status`, `coro_id` |

## Current State Analysis

### Types/Classes - Already Compliant
Most types already follow UpperCamelCase:
- `Event`, `IntEvent`, `OrEvent`, `AndEvent`
- `Reactor`, `Coroutine`
- `Marshal`, `Marshallable`
- `PollThread`, `PollThreadWorker`

### Methods - Need Renaming
Current CamelCase methods that need snake_case conversion:

#### reactor/event.h (~25 methods)
| Current | Target |
|---------|--------|
| `IsReady()` | `is_ready()` |
| `Test()` | `test()` |
| `Wait()` | `wait()` |
| `IsSlow()` | `is_slow()` |
| `GetCoroId()` | `get_coro_id()` |
| `RecordPlace()` | `record_place()` |
| `IsCompositeEvent()` | `is_composite_event()` |
| `AddEvent()` | `add_event()` |
| `TestTrigger()` | `test_trigger()` |
| `VoteYes()` | `vote_yes()` |
| `VoteNo()` | `vote_no()` |

#### reactor/reactor.h (~15 methods)
| Current | Target |
|---------|--------|
| `GetReactor()` | `get_reactor()` |
| `GetDiskReactor()` | `get_disk_reactor()` |
| `Loop()` | `loop()` |
| `CreateSpEvent()` | `create_sp_event()` |
| `CreateEvent()` | `create_event()` |
| `CreateRunCoroutine()` | `create_run_coroutine()` |
| `ContinueCoro()` | `continue_coro()` |
| `CheckTimeout()` | `check_timeout()` |

#### reactor/coroutine.h (~8 methods)
| Current | Target |
|---------|--------|
| `CreateRun()` | `create_run()` |
| `CurrentCoroutine()` | `current_coroutine()` |
| `Run()` | `run()` |
| `Yield()` | `yield_()` (note: yield is keyword in some contexts) |
| `Continue()` | `continue_()` (note: continue is C++ keyword) |
| `Finished()` | `finished()` |
| `DoFinalize()` | `do_finalize()` |
| `Sleep()` | `sleep()` |

## Migration Strategy

1. **Per-file approach**: Rename methods in one header file at a time
2. **Update all call sites**: Search and replace across entire codebase
3. **Build and test**: Ensure no regressions after each file
4. **Commit**: One commit per file for easy rollback

## Estimated Effort

| File | Methods | Est. Call Sites | Est. LOC Changes |
|------|---------|-----------------|------------------|
| event.h | 25 | ~200 | ~300 |
| reactor.h | 15 | ~150 | ~200 |
| coroutine.h | 8 | ~100 | ~150 |
| quorum_event.h | 5 | ~50 | ~80 |
| threading.hpp | 10 | ~50 | ~100 |
| marshal.hpp | 20 | ~300 | ~400 |
| alock.hpp | 10 | ~30 | ~50 |

**Total estimated**: ~1300 LOC changes across ~100 files

## Notes

- `continue` and `yield` are C++ keywords, need to use trailing underscore: `continue_()`, `yield_()`
- Some methods like `Run()`, `Test()` are very common - need careful search/replace
- Virtual methods need override declarations updated too
