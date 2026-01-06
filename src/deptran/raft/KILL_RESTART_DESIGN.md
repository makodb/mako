# Kill/Restart RPC Service Design

## Problem

When `Kill()` deletes a RaftServer, the `RaftServiceImpl` (owned by RPC infrastructure) retains a dangling pointer. Subsequent RPCs crash or fail silently.

```
Before Kill:  RaftServiceImpl.svr_ --> RaftServer (valid)
After Kill:   RaftServiceImpl.svr_ --> ??? (dangling)
After Restart: RaftServiceImpl.svr_ --> ??? (still dangling, new server unreachable)
```

## Solution: Atomic Pointer with Service Registry

```cpp
// service.h
class RaftServiceImpl {
  static std::map<siteid_t, RaftServiceImpl*> service_registry_;
  static std::mutex registry_mutex_;
  std::atomic<RaftServer*> svr_;  // Atomic for lock-free RPC reads
};
```

### Registration (Constructor)
Each RaftServiceImpl registers itself in a static map keyed by `site_id`:
```cpp
service_registry_[site_id_] = this;
```

### Kill Flow
```cpp
// testconf.cc Kill()
RaftServiceImpl::UpdateServer(site_id, nullptr);  // Clear pointer first
usleep(100000);                                    // Let in-flight RPCs drain
delete frame;                                      // Now safe to delete
```

### Restart Flow
```cpp
// testconf.cc Restart()
// ... create new RaftFrame and RaftServer ...
RaftServiceImpl::UpdateServer(site_id, frame->svr_.get());  // Point to new server
```

### RPC Handler Pattern
```cpp
void HandleAppendEntries(...) {
  RaftServer* svr = GetServer();  // Atomic load
  if (svr == nullptr) {
    // Server killed - return failure response
    *followerAppendOK = 0;
    defer.reply();
    return;
  }
  svr->OnAppendEntries(...);  // Safe to use
}
```

## Thread Safety

- **RPC hot path**: Lock-free atomic load via `GetServer()`
- **Kill/Restart**: Mutex protects registry lookup (rare operations)
- **Memory ordering**: `memory_order_release` on store, `memory_order_acquire` on load

## Key Files

- `service.h` - Registry and atomic pointer declarations
- `service.cc` - UpdateServer/GetServer implementation
- `macros.h` - RpcHandler macro uses GetServer()
- `testconf.cc` - Kill/Restart call UpdateServer()
