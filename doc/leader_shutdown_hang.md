# Leader Shutdown Hang Issue - Deep Investigation

## Problem Summary
The simpleRaft test's **leader process (raft_a1) hangs during shutdown** and never prints PASS. Followers (p1, p2) shut down cleanly and pass.

## Current State (as of latest investigation)
- **Root cause IDENTIFIED**: Multiple interacting issues in the shutdown sequence
- **Reactor::Loop() interruptible fix IMPLEMENTED**: Pass stop_flag to Reactor::Loop()
- **signal_stop() method ADDED**: Allows signaling shutdown without blocking on join()
- **Poll loop drain logic ADDED**: Continues processing commands after stop_flag is set
- **CURRENT BLOCKER**: Server::~Server() hangs waiting for sconns_ctr_ to reach 0

---

## Architecture Overview

### Two Poll Threads in RaftWorker
The leader creates TWO separate `PollThread` instances:
1. **`svr_poll_thread_worker_`** (line 70 in raft_worker.cc) - Main RPC server
2. **`svr_hb_poll_thread_worker_g`** (line 124) - Heartbeat server

Each has its own:
- MPSC channel (sender/receiver pair)
- Shared `stop_flag_` (std::shared_ptr<std::atomic<bool>>)
- Join handle

### Current Shutdown Sequence (after fixes)
```
RaftWorker::ShutDown() {
  1. signal_stop() on both poll threads   // NEW: Signal early
  2. delete rpc_server_                   // <-- STILL HANGS HERE
  3. delete hb_rpc_server_
  4. shutdown svr_poll_thread_worker_     // Calls join()
  5. shutdown svr_hb_poll_thread_worker_g
}
```

---

## Fixes Implemented

### 1. Reactor::Loop() Accepts stop_flag (Option B - IMPLEMENTED)
**Purpose**: Allow Reactor::Loop() to exit early during shutdown

```cpp
// In reactor.h:
void Loop(bool infinite = false, bool check_timeout = true,
          std::atomic<bool>* stop_flag = nullptr) const;

// In reactor.cc - Reactor::Loop():
do {
  // Check stop_flag at start of outer loop
  if (stop_flag && stop_flag->load(std::memory_order_acquire)) {
    return;
  }
  // ... disk events processing ...

  while (found_ready_events) {
    // Check stop_flag at start of inner loop
    if (stop_flag && stop_flag->load(std::memory_order_acquire)) {
      return;
    }
    // ... event processing ...
  }
} while (looping_);

// In poll_loop():
Reactor::GetReactor()->Loop(false, true, stop_flag_.get());
```

**Status**: WORKING - Reactor::Loop() now exits when stop_flag is set.

### 2. signal_stop() Method (IMPLEMENTED)
**Purpose**: Signal shutdown without blocking on join()

```cpp
// In reactor.h - PollThread class:
void signal_stop() const;  // Signal stop without joining

// In reactor.cc:
void PollThread::signal_stop() const {
  stop_flag_->store(true, std::memory_order_release);
  sender_.send(CmdShutdown{});
}

void PollThread::shutdown() const {
  if (shutdown_called_.exchange(true)) return;
  signal_stop();  // Set the stop flag
  // ... then join if not called from poll thread ...
}
```

**Status**: WORKING - Allows early shutdown signaling.

### 3. Poll Loop Drain Logic (IMPLEMENTED)
**Purpose**: Continue processing commands after stop_flag is set

```cpp
void PollThreadWorker::poll_loop() {
  bool stopping = false;
  int drain_iterations = 0;
  constexpr int MAX_DRAIN_ITERATIONS = 50;

  while (true) {
    if (!stopping && stop_flag_->load(std::memory_order_acquire)) {
      stopping = true;
      Log_info("[POLL-LOOP] stop_flag set, starting drain");
    }

    if (stopping) {
      if (drain_iterations >= MAX_DRAIN_ITERATIONS) {
        break;  // Exit after max drain iterations
      }
      drain_iterations++;
    }

    // Process epoll events and commands
    poll_.Wait(...);
    process_commands();
    process_pending_removals();

    // Skip Reactor::Loop() during drain (avoid coroutine blocking)
    if (!stopping) {
      Reactor::GetReactor()->Loop(false, true, stop_flag_.get());
    }

    // Exit early if no more work
    if (stopping && fd_to_pollable_.empty() && pending_remove_.empty()) {
      break;
    }
  }
}
```

**Status**: WORKING - Poll loop now drains and exits properly.

### 4. Shutdown Sequence Modified (IMPLEMENTED)
**Purpose**: Signal poll threads before deleting servers

```cpp
// In raft_worker.cc - RaftWorker::ShutDown():
Log_info("[RAFT-WORKER-SHUTDOWN] signaling poll threads to stop");
if (svr_poll_thread_worker_.is_some()) {
  svr_poll_thread_worker_.as_ref().unwrap()->signal_stop();
}
if (svr_hb_poll_thread_worker_g.is_some()) {
  svr_hb_poll_thread_worker_g.as_ref().unwrap()->signal_stop();
}

// Then delete servers...
delete rpc_server_;  // <-- Still hangs here
```

**Status**: WORKING - Poll threads are signaled before server deletion.

---

## Current Problem: Server::~Server() Hangs

### The Issue
After all the above fixes, the poll loop exits properly:
```
[POLL-LOOP] stop_flag set, starting drain
[POLL-LOOP] max drain iterations reached, exiting
[POLL-LOOP] exiting, stop_flag=1
```

But `delete rpc_server_` still hangs because `Server::~Server()` waits for `sconns_ctr_` to reach 0:

```cpp
// In server.cpp - Server::~Server():
for (auto& it: sconns) {
    conn.close();
    poll_thread_worker_.as_ref().unwrap()->remove(conn);  // Sends command
}
// ...
for (;;) {
    int new_alive_connection_count = sconns_ctr_.peek_next();
    if (new_alive_connection_count <= 0) break;  // <-- Never reaches 0!
    // ... waits forever ...
}
```

### Why It Hangs
1. `signal_stop()` is called, poll thread starts draining
2. `delete rpc_server_` is called
3. `Server::~Server()` sends `CmdRemovePollable` commands via `poll_thread_worker_->remove()`
4. Poll thread finishes drain iterations (50) and EXITS
5. The remove commands sent in step 3 are never processed
6. `sconns_ctr_` never reaches 0
7. Server destructor waits forever

### The Race Condition
The poll thread and Server destructor are racing:
- Poll thread: "I've drained 50 iterations, time to exit"
- Server destructor: "I'm sending remove commands... wait, poll thread exited!"

---

## Proposed Solutions for Current Blocker

### Option 1: Don't Exit Until Channel is Empty AND fd_to_pollable_ is Empty
```cpp
// In poll_loop drain logic:
if (stopping) {
  // Don't use iteration count, instead check if work remains
  bool has_pending_commands = receiver_.try_recv().is_ok();  // Check channel
  if (fd_to_pollable_.empty() && !has_pending_commands) {
    break;  // Only exit when truly empty
  }
}
```

### Option 2: Server Destructor Doesn't Wait
```cpp
// In Server::~Server() - just close and don't wait
for (auto& it: sconns) {
    conn.close();
    poll_thread_worker_.as_ref().unwrap()->remove(conn);
}
// Remove the wait loop - let poll thread clean up asynchronously
```

### Option 3: Coordinate via Condition Variable
Add a condition variable that Server destructor waits on, and poll thread signals when it finishes processing removes.

### Option 4: Process All Pending Removes Before Exiting
```cpp
// In poll_loop after the main loop, before cleanup:
// Process all remaining commands synchronously
while (true) {
  auto result = receiver_.try_recv();
  if (result.is_err()) break;
  // Process command...
}
```

---

## Current Log Patterns

### Poll Thread Exits (WORKING NOW)
```
[RAFT-WORKER-SHUTDOWN] signaling poll threads to stop
[RAFT-WORKER-SHUTDOWN] deleting rpc_server_
[POLL-LOOP] stop_flag set, starting drain
[POLL-LOOP] max drain iterations reached, exiting
[POLL-LOOP] exiting, stop_flag=1
```

### Server Destructor Still Waiting (CURRENT PROBLEM)
After `[POLL-LOOP] exiting`, no more log messages appear. The process is stuck in `Server::~Server()` waiting for `sconns_ctr_`.

---

## File Locations

### Modified Files
- `src/rrr/reactor/reactor.cc` - PollThread, PollThreadWorker, poll_loop drain logic
- `src/rrr/reactor/reactor.h` - signal_stop(), Loop() with stop_flag parameter
- `src/deptran/raft/raft_worker.cc` - Shutdown sequence with signal_stop()

### Key Functions
- `PollThread::create()` - Creates shared stop_flag (line ~602)
- `PollThread::signal_stop()` - Sets stop_flag without joining (line ~636)
- `PollThread::shutdown()` - Sets stop_flag and joins (line ~643)
- `PollThreadWorker::poll_loop()` - Main loop with drain logic (line ~431)
- `Reactor::Loop()` - Event processing with stop_flag checks (line ~196)
- `RaftWorker::ShutDown()` - Shutdown sequence (line ~136)
- `Server::~Server()` - Waits for sconns_ctr_ (line ~342)

---

## Test Commands
```bash
# Run the test
timeout 45 ./examples/mako-raft-tests/simpleRaft.sh

# Check poll loop behavior
grep -E "POLL-LOOP" raft_a1.log

# Check shutdown sequence
grep -E "(RAFT-WORKER-SHUTDOWN|POLL-SHUTDOWN)" raft_a1.log

# Count coroutines
grep "coroutines on server" raft_a1.log
```

---

## Summary for Next Agent

**Progress Made**:
- Reactor::Loop() is now interruptible (passes stop_flag, checks it at loop boundaries)
- Poll thread properly exits after draining
- signal_stop() allows early shutdown signaling
- Followers continue to shut down cleanly

**Current Blocker**:
- Server::~Server() waits forever for sconns_ctr_ to reach 0
- Poll thread exits before Server destructor finishes sending remove commands
- Race condition between poll thread drain and Server destructor

**Next Steps**:
1. Fix the race condition - poll thread should not exit while Server destructor is still sending commands
2. Option 1 (don't exit until channel empty) is probably the cleanest fix
3. Alternatively, Option 4 (process remaining commands synchronously before exiting)
