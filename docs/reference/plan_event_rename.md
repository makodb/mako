# Plan: Rename Event Methods to snake_case

## Scope

File: `src/srpc/reactor/event.h` and `src/srpc/reactor/event.cc`

## Methods to Rename

| Class | Current | Target | Virtual |
|-------|---------|--------|---------|
| Event | `Wait()` | `wait()` | final |
| Event | `Test()` | `test()` | virtual |
| Event | `IsSlow()` | `is_slow()` | virtual |
| Event | `IsReady()` | `is_ready()` | virtual |
| Event | `IsCompositeEvent()` | `is_composite_event()` | virtual |
| Event | `GetCoroId()` | `get_coro_id()` | virtual |
| Event | `RecordPlace()` | `record_place()` | no |
| BoxEvent | `Get()` | `get()` | no |
| BoxEvent | `Set()` | `set()` | no |
| BoxEvent | `Clear()` | `clear()` | no |
| IntEvent | `TestTrigger()` | `test_trigger()` | no |
| IntEvent | `Set()` | `set()` | no |
| SharedIntEvent | `Set()` | `set()` | no |
| SharedIntEvent | `Wait()` | `wait()` | no |
| SharedIntEvent | `WaitUntilGreaterOrEqualThan()` | `wait_until_gte()` | no |
| TimeoutEvent | `Wait()` | `wait()` | no |
| OrEvent | `AddEvent()` | `add_event()` | no |
| AndEvent | `AddEvent()` | `add_event()` | no |
| NEvent | `AddEvent()` | `add_event()` | no |
| DispatchEvent | `IsReady()` | `is_ready()` | override |
| SingleRPCEvent | `IsReady()` | `is_ready()` | override |

## Execution Steps

1. Rename methods in event.h
2. Rename methods in event.cc
3. Search and replace call sites across entire codebase
4. Build and test
5. Fix any issues

## Call Site Search Commands

```bash
# Find all usages
grep -rn "->Wait(" src/
grep -rn "->Test(" src/
grep -rn "->IsReady(" src/
grep -rn "->IsSlow(" src/
grep -rn "->IsCompositeEvent(" src/
grep -rn "->GetCoroId(" src/
grep -rn "->RecordPlace(" src/
grep -rn "->AddEvent(" src/
grep -rn "->TestTrigger(" src/
grep -rn "WaitUntilGreaterOrEqualThan(" src/
```

## Risk Assessment

- **Virtual methods**: Need to update all override declarations
- **Template methods**: AddEvent uses variadic templates
- **Macro usage**: Wait_recordplace macro uses Wait
