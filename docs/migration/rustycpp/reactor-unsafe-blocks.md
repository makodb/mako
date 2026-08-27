# Reactor Folder Unsafe Blocks and Functions

This document lists all `@unsafe` annotated blocks and functions in `src/srpc/reactor/`, sorted by LOC.

**Generated**: 2026-01-03

---

## Summary

| File | Unsafe Count | Total LOC |
|------|--------------|-----------|
| epoll_wrapper.h | 6 | ~200 |
| reactor.cc | 20 | ~180 |
| reactor.h | 10 | ~50 |
| coroutine.cc | 3 | ~45 |
| coroutine.h | 5 | ~25 |
| event.h | 3 | ~15 |
| quorum_event.h | 2 | ~10 |

**Total**: ~49 unsafe annotations, ~525 LOC

---

## Detailed List (Sorted by LOC)

### epoll_wrapper.h

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `Epoll::Wait<ModeUpdater>` | 65 | 261-330 | HIGH | Waits for events, raw pointer dereference |
| 2 | `Epoll::Update` | 66 | 188-256 | HIGH | Updates poll mode, system calls |
| 3 | `Epoll::Add` | 36 | 119-157 | HIGH | Adds fd to epoll, raw pointer cast |
| 4 | `Epoll::Remove` | 24 | 160-185 | MED | Removes fd, ignores errors |
| 5 | `Epoll()` constructor | 9 | 87-96 | LOW | Creates epoll fd |
| 6 | Class annotation | - | 69 | - | Marks entire class unsafe |

### reactor.cc

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `PollThreadWorker::poll_loop` | 63 | 455-518 | HIGH | Main event loop |
| 2 | `process_commands` | 30 | 520-551 | MED | try_recv and std::visit |
| 3 | `do_close_pollable` | 27 | 603-630 | HIGH | Closes socket, drops Arc |
| 4 | `do_add_pollable` | 17 | 575-592 | HIGH | Raw pointer cast for userdata |
| 5 | `TriggerJob` | 20 | 553-573 | MED | std::set operations |
| 6 | `do_update_mode` | 20 | 632-654 | HIGH | Raw pointer dereference |
| 7 | `GetOrCreateCoroutine` | 28 | 114-144 | MED | RefCell operations |
| 8 | `process_pending_removals` | 22 | 666-688 | MED | unordered_set::swap |
| 9 | `CreateRunCoroutine` | 37 | 198-236 | MED | Multiple internal blocks |
| 10 | `update_mode` (worker) | 3 | 693-697 | MED | address-of, epoll mod |
| 11 | `SaveRunningCoroutine` | 8 | 147-157 | LOW | RefCell::borrow |
| 12 | `RestoreRunningCoroutine` | 4 | 160-165 | LOW | RefCell::borrow_mut |
| 13 | `SetRunningCoroutine` | 4 | 168-173 | LOW | RefCell::borrow_mut |
| 14 | `RegisterCoroutine` | 6 | 176-187 | LOW | verify() calls |
| 15 | `CheckTimeout` | 3 | 242-243 | LOW | Time::now() |
| 16 | `CurrentCoroutine` | 8 | 46-57 | LOW | RefCell::borrow, Rc::clone |
| 17 | `CreateRunImpl` | 8 | 59-67 | LOW | Calls GetReactor |
| 18 | `create` (worker) | 5 | 448-453 | LOW | Rc<RefCell> wrapping |
| 19 | `update_mode` (PollThread) | 8 | 806-814 | LOW | channel send |
| 20 | `do_add/remove_job` | 3+3 | 656-664 | LOW | std::set insert/erase |

### reactor.h

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `CreateSpEvent<Ev>` | 12 | 239-256 | MED | shared_ptr creation |
| 2 | `CreateEvent<Ev>` | 10 | 258-270 | MED | Returns reference to shared_ptr |
| 3 | `PollThreadWorker` class | - | 330 | - | Factory method annotation |
| 4 | `poll_loop` decl | - | 345 | - | Declaration only |
| 5 | `add_pollable_from_current_thread` | - | 353 | - | Declaration only |
| 6 | `update_mode` decl | - | 361-363 | - | Declaration only |
| 7 | `get_remove_count` | 2 | 372-373 | LOW | Atomic load |
| 8 | `PollThread` class | - | 417-418 | - | Thread-safe despite annotation |
| 9 | `GetReactor` | - | 138 | - | Declaration only |
| 10 | `GetDiskReactor` | - | 141 | - | Declaration only |

### coroutine.cc

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `Coroutine::Run` | 15 | 55-70 | MED | const_cast, boost coroutine |
| 2 | `Coroutine::Yield` | 12 | 74-87 | MED | boost::optional, status_ |
| 3 | `Coroutine::Continue` | 10 | 91-102 | MED | boost coroutine resume |

### coroutine.h

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `Coroutine` class | - | 47 | - | Class-level annotation |
| 2 | `CreateRun<Func>` | 3 | 64-68 | LOW | Wraps CreateRunImpl |
| 3 | `BoostRunWrapper` | - | 92 | - | std::bind, function ptrs |
| 4 | `Run()` | - | 96 | - | Internal @unsafe in impl |
| 5 | `CreateRunImpl` | - | 116 | - | Declaration only |

### event.h

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `Event::Wait` | ~35 | 84 | MED | Adds to waiting_events_ |
| 2 | `IntEvent::Set` | 6 | 154 | LOW | Triggers Test() |
| 3 | `TimeoutEvent::Wait` | 3 | 203 | LOW | Calls Event::Wait |

### quorum_event.h

| # | Function | LOC | Lines | Risk | Description |
|---|----------|-----|-------|------|-------------|
| 1 | `VoteYes` | ~5 | 83 | LOW | Calls Test(), Time::now() |
| 2 | `VoteNo` | ~3 | 86 | LOW | Calls Test() |

---

## Risk Assessment

### HIGH RISK (Pointer Safety)
| Function | Issue |
|----------|-------|
| `Epoll::Wait` | Raw pointer cast from userdata |
| `Epoll::Update` | System calls with raw pointers |
| `Epoll::Add` | Raw pointer cast for userdata |
| `do_add_pollable` | Raw pointer cast for epoll |
| `do_update_mode` | Raw pointer dereference |
| `do_close_pollable` | Arc drop, socket close |
| `poll_loop` | Coordinates all poll operations |

### MEDIUM RISK (Thread/Memory)
| Function | Issue |
|----------|-------|
| `process_commands` | Channel try_recv |
| `TriggerJob` | std::set operations |
| `Coroutine::Run/Yield/Continue` | Boost coroutine primitives |
| `CreateSpEvent/CreateEvent` | shared_ptr creation |
| `GetOrCreateCoroutine` | RefCell operations |

### LOW RISK (Wrapper/Boilerplate)
| Function | Issue |
|----------|-------|
| `CurrentCoroutine` | RefCell::borrow |
| `Save/Restore/SetRunningCoroutine` | RefCell operations |
| `RegisterCoroutine` | verify() calls |
| `do_add/remove_job` | std::set insert/erase |
| `VoteYes/VoteNo` | Test() trigger |

---

## Migration Status

### Already Safe (Completed)
- `Reactor::Loop()` - Now @safe
- `Reactor::ContinueCoro()` - Now @safe
- `Event::Test()` - Now @safe
- `Event::status_` -> `rusty::Cell<EventStatus>`
- `Coroutine::status_` -> `rusty::Cell<Status>`
- `Event::wp_coro_` -> `rusty::rc::Weak<Coroutine>`
- ThreadSafeIntEvent - **REMOVED** (unused)

### Candidates for Migration
1. **epoll_wrapper.h** - Complex, system interface, LOW priority
2. **coroutine.cc** - Boost dependency, MEDIUM priority
3. **reactor.cc PollThreadWorker** - Channel-based, could be simplified
4. **event.h Event::Wait** - Already mostly safe, needs cleanup

### Cannot Migrate (External Dependencies)
- Boost coroutine primitives
- epoll/kqueue system calls
- pthread operations
