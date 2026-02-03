# Raft Module Function Dependency Analysis
**For Bottom-Up @safe Migration**

**Generated**: 2025-10-24
**Purpose**: Guide function-by-function @safe annotation from leaves to complex functions

---

## Table of Contents
1. [RustyCpp Documentation Paths](#rustycpp-documentation-paths)
2. [Function Inventory](#function-inventory)
3. [Dependency Levels](#dependency-levels)
4. [Migration Order](#migration-order)
5. [Quick Reference](#quick-reference)

---

## RustyCpp Documentation Paths

### Core Documentation
- **Main README**: `/home/users/mmakadia/mako_temp/third-party/rusty-cpp/README.md`
- **Include README**: `/home/users/mmakadia/mako_temp/third-party/rusty-cpp/include/rusty/README.md`
- **CMake Integration**: `/home/users/mmakadia/mako_temp/third-party/rusty-cpp/cmake/README.md`
- **Borrow Checker**: `/home/users/mmakadia/mako_temp/third-party/rusty-cpp/dist/cpp-borrow-checker-*/README.md`

### Key Header Files (API Reference)
```
third-party/rusty-cpp/include/rusty/
├── box.hpp        - rusty::Box<T> (single ownership)
├── arc.hpp        - rusty::Arc<T> (thread-safe shared)
├── rc.hpp         - rusty::Rc<T> (single-thread shared)
├── option.hpp     - rusty::Option<T> (optional values)
├── result.hpp     - rusty::Result<T,E> (error handling)
├── cell.hpp       - rusty::Cell<T> (interior mutability)
└── mutex.hpp      - rusty::Mutex<T> (synchronized access)
```

### Borrow Checker Binary
```bash
# Location
./third-party/rusty-cpp/target/release/rusty-cpp-checker

# Usage
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party
```

---

## Function Inventory

### File: `server.h` / `server.cc`

#### Simple Getters (Level 0 - Leaf Functions)
```cpp
// Location: server.h:173
bool IsLeader()
  Dependencies: NONE (just returns is_leader_)
  Complexity: TRIVIAL
  Raw Pointers: NO
  Calls: NONE
  Status: ⭐ START HERE - Perfect leaf function
```

```cpp
// Location: server.h:143
double randDuration()
  Dependencies: RandomGenerator::rand_double() [EXTERNAL]
  Complexity: SIMPLE
  Raw Pointers: NO
  Calls: RandomGenerator::rand_double(0.4, 0.7)
  Status: ⭐ LEAF - Only calls external library
```

```cpp
// Location: server.h:78
RaftCommo* commo()
  Dependencies: NONE
  Complexity: TRIVIAL
  Raw Pointers: YES (returns RaftCommo*)
  Calls: NONE (just cast)
  Status: ⚠️ UNSAFE - Returns raw pointer
```

#### Simple Functions with Minimal Dependencies (Level 1)

```cpp
// Location: server.h:180
void GetState(bool *is_leader, uint64_t *term)
  Dependencies: IsLeader() [LEVEL 0]
  Complexity: SIMPLE
  Raw Pointers: YES (parameters)
  Calls: IsLeader(), std::lock_guard
  Status: Can be @safe after IsLeader() is @safe
```

```cpp
// Location: server.h:134
void resetTimer()
  Dependencies: Time::now() [EXTERNAL], timer_ [RAW POINTER]
  Complexity: SIMPLE
  Raw Pointers: YES (timer_->start())
  Calls: Time::now(), timer_->start()
  Status: ⚠️ UNSAFE - Uses raw pointer timer_
```

```cpp
// Location: server.h:288
void Reconnect()
  Dependencies: Disconnect() [LEVEL 2], resetTimer() [LEVEL 1]
  Complexity: SIMPLE
  Raw Pointers: NO
  Calls: Disconnect(false), resetTimer()
  Status: Depends on Disconnect and resetTimer
```

#### Functions with Lock Management (Level 1-2)

```cpp
// Location: server.h:121
void resetTimerBatch()
  Dependencies: resetTimer() [LEVEL 1], timer_ [RAW POINTER]
  Complexity: MEDIUM
  Raw Pointers: YES (timer_->elapsed())
  Calls: resetTimer(), timer_->elapsed(), counter_++
  Status: ⚠️ UNSAFE - Uses raw pointer timer_
```

```cpp
// Location: server.h:186
void SetLocalAppend(...)
  Dependencies: GetRaftInstance() [LEVEL 1]
  Complexity: HIGH
  Raw Pointers: NO (uses shared_ptr)
  Calls: GetRaftInstance(), std::lock_guard, std::make_shared
  Status: Depends on GetRaftInstance
```

#### Instance Management (Level 1)

```cpp
// Location: server.h:235
shared_ptr<RaftData> GetInstance(slotid_t id)
  Dependencies: std::make_shared [EXTERNAL]
  Complexity: SIMPLE
  Raw Pointers: NO
  Calls: std::make_shared<RaftData>()
  Status: Can be @safe (uses shared_ptr, but needs Arc migration)
```

```cpp
// Location: server.h:252
shared_ptr<RaftData> GetRaftInstance(slotid_t id)
  Dependencies: std::make_shared [EXTERNAL]
  Complexity: SIMPLE
  Raw Pointers: NO
  Calls: std::make_shared<RaftData>()
  Status: Can be @safe (uses shared_ptr, but needs Arc migration)
```

#### Complex Protocol Functions (Level 3+)

```cpp
// Location: server.h:81
void setIsLeader(bool isLeader)
  Dependencies: UNKNOWN (need to check .cc file)
  Complexity: UNKNOWN
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:83
void doVote(...)
  Dependencies: setIsLeader() [UNKNOWN], resetTimer() [LEVEL 1]
  Complexity: HIGH
  Raw Pointers: YES (parameters)
  Calls: setIsLeader(false), resetTimer(), cb()
  Status: Complex - analyze after dependencies
```

```cpp
// Location: server.h:178
bool Start(shared_ptr<Marshallable> &cmd, ...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: YES (parameters)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:264
void OnRequestVote(...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: YES (parameters)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:272
void OnAppendEntries(...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: VERY HIGH
  Raw Pointers: YES (parameters, shared_ptr)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:286
void Disconnect(const bool disconnect = true)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: UNKNOWN
  Raw Pointers: NO
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:293
bool IsDisconnected()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: UNKNOWN
  Raw Pointers: NO
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:119
void applyLogs()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:74
bool RequestVote()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:76
void Setup()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:77
void HeartbeatLoop()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: server.h:171
void StartElectionTimer()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: UNKNOWN
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

---

### File: `coordinator.h` / `coordinator.cc`

#### Simple Getters (Level 0)

```cpp
// Location: coordinator.h:41
uint32_t n_replica()
  Dependencies: NONE
  Complexity: TRIVIAL
  Raw Pointers: NO
  Calls: verify()
  Status: ⭐ LEAF - verify() is external macro
```

```cpp
// Location: coordinator.h:49
slotid_t GetNextSlot()
  Dependencies: NONE
  Complexity: SIMPLE
  Raw Pointers: YES (slot_hint_)
  Calls: verify()
  Status: ⚠️ UNSAFE - Uses raw pointer slot_hint_
```

```cpp
// Location: coordinator.h:56
uint32_t GetQuorum()
  Dependencies: n_replica() [LEVEL 0]
  Complexity: TRIVIAL
  Calls: n_replica()
  Status: Can be @safe after n_replica() is @safe
```

#### Cast Functions (Level 0)

```cpp
// Location: coordinator.h:19
RaftCommo* commo()
  Dependencies: NONE
  Complexity: TRIVIAL
  Raw Pointers: YES (returns RaftCommo*)
  Calls: verify()
  Status: ⚠️ UNSAFE - Returns raw pointer
```

#### Complex Functions (Level 2+)

```cpp
// Location: coordinator.h:46
bool IsLeader()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: UNKNOWN
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: coordinator.h:47
bool IsFPGALeader()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: UNKNOWN
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: coordinator.h:62
void Submit(shared_ptr<Marshallable> &cmd, ...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: NO (shared_ptr)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: coordinator.h:66
void AppendEntries()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: coordinator.h:67
void Commit()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: coordinator.h:68
void LeaderLearn()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: coordinator.h:73
void GotoNextPhase()
  Dependencies: UNKNOWN (need .cc file)
  Complexity: MEDIUM
  Raw Pointers: UNKNOWN
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

---

### File: `commo.h` / `commo.cc`

#### Simple Getters (Level 0)

```cpp
// Location: commo.h:14
bool HasAcceptedValue()
  Dependencies: NONE
  Complexity: TRIVIAL
  Raw Pointers: NO
  Calls: NONE (returns false)
  Status: ⭐ LEAF - Perfect simple function
```

```cpp
// Location: commo.h:29
int64_t Term()
  Dependencies: NONE
  Complexity: TRIVIAL
  Raw Pointers: NO
  Calls: NONE (returns highest_term_)
  Status: ⭐ LEAF - Simple getter
```

#### Functions with Logic (Level 1)

```cpp
// Location: commo.h:17
void FeedResponse(bool y, ballot_t term)
  Dependencies: VoteYes() [PARENT], VoteNo() [PARENT]
  Complexity: SIMPLE
  Raw Pointers: NO
  Calls: VoteYes(), VoteNo()
  Status: Depends on parent class QuorumEvent
```

#### RPC Functions (Level 2+)

```cpp
// Location: commo.h:57
shared_ptr<IntEvent> SendAppendEntries2(...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: YES (parameters)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: commo.h:75
shared_ptr<SendAppendEntriesResults> SendAppendEntries(...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: NO (shared_ptr)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

```cpp
// Location: commo.h:88
shared_ptr<RaftVoteQuorumEvent> BroadcastVote(...)
  Dependencies: UNKNOWN (need .cc file)
  Complexity: HIGH
  Raw Pointers: NO (shared_ptr)
  Calls: UNKNOWN
  Status: ⏳ Need to analyze implementation
```

---

### File: `frame.h` / `frame.cc`

All functions are factory methods that create components - these are complex and will be in Level 3+.

```cpp
// Location: frame.h:28
Executor* CreateExecutor(...)
  Status: ⏳ Complex factory - analyze later
```

```cpp
// Location: frame.h:29
Coordinator* CreateCoordinator(...)
  Status: ⏳ Complex factory - analyze later
```

```cpp
// Location: frame.h:35
TxLogServer* CreateScheduler()
  Status: ⏳ Complex factory - analyze later
```

```cpp
// Location: frame.h:36
Communicator* CreateCommo(...)
  Status: ⏳ Complex factory - analyze later
```

```cpp
// Location: frame.h:37
vector<rrr::Service*> CreateRpcServices(...)
  Status: ⏳ Complex factory - analyze later
```

---

## Dependency Levels

### Level 0: Leaf Functions (No Internal Dependencies)

**Perfect Leaves** (Start Here):
1. ⭐ `RaftServer::IsLeader()` - Just returns bool field
2. ⭐ `RaftServer::randDuration()` - Calls external RandomGenerator only
3. ⭐ `RaftVoteQuorumEvent::HasAcceptedValue()` - Returns false
4. ⭐ `RaftVoteQuorumEvent::Term()` - Returns field
5. ⭐ `CoordinatorRaft::n_replica()` - Returns field with verify

**Unsafe Leaves** (Raw Pointers):
6. ⚠️ `RaftServer::commo()` - Returns raw pointer (mark @unsafe)
7. ⚠️ `CoordinatorRaft::commo()` - Returns raw pointer (mark @unsafe)
8. ⚠️ `CoordinatorRaft::GetNextSlot()` - Uses raw pointer slot_hint_

**External-Only Dependencies**:
9. `RaftServer::GetInstance()` - Calls std::make_shared only
10. `RaftServer::GetRaftInstance()` - Calls std::make_shared only

### Level 1: Calls Level 0 Functions Only

1. `CoordinatorRaft::GetQuorum()` - Calls n_replica() [Level 0]
2. `RaftServer::GetState()` - Calls IsLeader() [Level 0]
3. `RaftServer::resetTimer()` - Uses timer_ (raw pointer) + external Time::now()
4. `RaftVoteQuorumEvent::FeedResponse()` - Calls parent VoteYes/VoteNo

### Level 2: Calls Level 0 + Level 1 Functions

1. `RaftServer::resetTimerBatch()` - Calls resetTimer() [Level 1]
2. `RaftServer::Reconnect()` - Calls Disconnect() + resetTimer() [Level 1]
3. `RaftServer::doVote()` - Calls setIsLeader() + resetTimer() [Level 1]

### Level 3+: Complex Functions (Need .cc Analysis)

These need implementation analysis from .cc files:
- `RaftServer::Start()`
- `RaftServer::OnRequestVote()`
- `RaftServer::OnAppendEntries()`
- `RaftServer::Setup()`
- `RaftServer::HeartbeatLoop()`
- `RaftServer::RequestVote()`
- `RaftServer::applyLogs()`
- `CoordinatorRaft::Submit()`
- `CoordinatorRaft::AppendEntries()`
- `CoordinatorRaft::Commit()`
- `RaftCommo::SendAppendEntries2()`
- `RaftCommo::SendAppendEntries()`
- `RaftCommo::BroadcastVote()`
- All `RaftFrame::Create*()` factory methods

---

## Migration Order

### Phase 1: Annotate Level 0 Leaves (Week 1, Days 1-2)

#### Day 1: Perfect Leaves (No Issues Expected)

**Step 1.1**: Annotate `RaftServer::IsLeader()`
```cpp
// File: src/deptran/raft/server.h:173
// @safe
bool IsLeader() {
  return is_leader_;
}
```
- Run borrow checker
- Should pass with no violations
- Commit: "Mark IsLeader() as @safe"

**Step 1.2**: Annotate `RaftServer::randDuration()`
```cpp
// File: src/deptran/raft/server.h:143
// @safe
double randDuration() {
  return RandomGenerator::rand_double(0.4, 0.7);
}
```
- Run borrow checker
- If RandomGenerator::rand_double is undeclared, mark this @unsafe instead
- Commit: "Mark randDuration() as @safe" OR "Mark randDuration() as @unsafe - calls undeclared external"

**Step 1.3**: Annotate `RaftVoteQuorumEvent::HasAcceptedValue()`
```cpp
// File: src/deptran/raft/commo.h:14
// @safe
bool HasAcceptedValue() {
  return false;
}
```
- Run borrow checker
- Should pass
- Commit: "Mark HasAcceptedValue() as @safe"

**Step 1.4**: Annotate `RaftVoteQuorumEvent::Term()`
```cpp
// File: src/deptran/raft/commo.h:29
// @safe
int64_t Term() {
  return highest_term_;
}
```
- Run borrow checker
- Should pass
- Commit: "Mark Term() as @safe"

**Step 1.5**: Annotate `CoordinatorRaft::n_replica()`
```cpp
// File: src/deptran/raft/coordinator.h:41
// @safe
uint32_t n_replica() {
  verify(n_replica_ > 0);
  return n_replica_;
}
```
- Run borrow checker
- If verify() is undeclared, mark @unsafe instead
- Commit appropriately

#### Day 2: Unsafe Leaves (Document Why)

**Step 1.6**: Mark pointer-returning functions @unsafe
```cpp
// File: src/deptran/raft/server.h:78
// @unsafe - Returns raw pointer to parent class member
RaftCommo* commo() {
  return (RaftCommo*) commo_;
}

// File: src/deptran/raft/coordinator.h:19
// @unsafe - Returns raw pointer to parent class member
RaftCommo* commo() {
  verify(commo_ != nullptr);
  return (RaftCommo *) commo_;
}

// File: src/deptran/raft/coordinator.h:49
// @unsafe - Uses raw pointer slot_hint_
slotid_t GetNextSlot() {
  verify(0);
  verify(slot_hint_ != nullptr);
  slot_id_ = (*slot_hint_)++;
  return 0;
}
```
- No borrow checker needed (explicitly unsafe)
- Commit: "Mark pointer-returning functions as @unsafe with reasons"

**Step 1.7**: Annotate instance getters
```cpp
// File: src/deptran/raft/server.h:235
// @safe  // Or @unsafe if shared_ptr not compatible yet
shared_ptr<RaftData> GetInstance(slotid_t id) {
  verify(id >= min_active_slot_ || lastLogIndex == 0);
  auto& sp_instance = logs_[id];
  if(!sp_instance)
    sp_instance = std::make_shared<RaftData>();
  return sp_instance;
}

// File: src/deptran/raft/server.h:252
// @safe  // Or @unsafe if shared_ptr not compatible yet
shared_ptr<RaftData> GetRaftInstance(slotid_t id) {
  verify(id >= min_active_slot_ || id == 0);
  auto& sp_instance = raft_logs_[id];
  if(!sp_instance)
    sp_instance = std::make_shared<RaftData>();
  return sp_instance;
}
```
- Run borrow checker
- May need @unsafe if shared_ptr operations violate rules
- Commit appropriately

---

### Phase 2: Annotate Level 1 Functions (Week 1, Days 3-4)

**Step 2.1**: Annotate `CoordinatorRaft::GetQuorum()`
```cpp
// File: src/deptran/raft/coordinator.h:56
// @safe  // Since n_replica() is now @safe
uint32_t GetQuorum() {
  return n_replica() / 2 + 1;
}
```
- Depends on: n_replica() [@safe from Step 1.5]
- Run borrow checker
- Should pass if n_replica() passed
- Commit: "Mark GetQuorum() as @safe"

**Step 2.2**: Annotate `RaftServer::GetState()`
```cpp
// File: src/deptran/raft/server.h:180
// @safe  // Since IsLeader() is now @safe
void GetState(bool *is_leader, uint64_t *term) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  *is_leader = IsLeader();  // Calls @safe IsLeader()
  *term = currentTerm;
}
```
- Depends on: IsLeader() [@safe from Step 1.1]
- Run borrow checker
- Might fail due to raw pointer parameters
- If fails: Mark @unsafe with reason "Raw pointer parameters for C compatibility"
- Commit appropriately

**Step 2.3**: Annotate `RaftServer::resetTimer()`
```cpp
// File: src/deptran/raft/server.h:134
// @unsafe - Uses raw pointer timer_
void resetTimer() {
  last_heartbeat_time_ = Time::now();
  if (failover_) {
    timer_->start();  // Raw pointer operation
  }
}
```
- Uses timer_ raw pointer
- Mark @unsafe
- Commit: "Mark resetTimer() as @unsafe - uses raw pointer timer_"

**Step 2.4**: Annotate `RaftVoteQuorumEvent::FeedResponse()`
```cpp
// File: src/deptran/raft/commo.h:17
// @safe or @unsafe depending on parent VoteYes/VoteNo
void FeedResponse(bool y, ballot_t term) {
  if (y) {
    VoteYes();  // Parent class function
  } else {
    VoteNo();   // Parent class function
    if(term > highest_term_) {
      highest_term_ = term;
    }
  }
}
```
- Depends on parent class QuorumEvent
- If VoteYes/VoteNo are undeclared: Mark @unsafe
- Otherwise: Mark @safe
- Commit appropriately

---

### Phase 3: Annotate Level 2 Functions (Week 1, Days 5-7)

**Step 3.1**: Annotate `RaftServer::resetTimerBatch()`
```cpp
// File: src/deptran/raft/server.h:121
// @unsafe - Calls resetTimer() which uses raw pointer + uses timer_ directly
void resetTimerBatch() {
  if (!failover_) return;
  auto cur_count = counter_++;
  if (cur_count > NUM_BATCH_TIMER_RESET) {
    if (timer_->elapsed() > SEC_BATCH_TIMER_RESET) {  // Raw pointer
      resetTimer();  // Calls @unsafe function - OK
    }
    counter_.store(0);
  }
}
```
- Uses timer_ raw pointer
- Calls resetTimer() [@unsafe from Step 2.3] - OK to call @unsafe from @safe, but this also uses raw pointer
- Mark @unsafe
- Commit: "Mark resetTimerBatch() as @unsafe - uses raw pointer timer_"

**Step 3.2**: Annotate `RaftServer::Reconnect()`
```cpp
// File: src/deptran/raft/server.h:288
// @unsafe - Calls @unsafe resetTimer()
void Reconnect() {
  Disconnect(false);  // Unknown status yet
  resetTimer();       // @unsafe from Step 2.3
}
```
- Depends on: Disconnect() [unknown], resetTimer() [@unsafe]
- Need to analyze Disconnect() first
- Mark @unsafe for now
- Commit: "Mark Reconnect() as @unsafe - calls @unsafe resetTimer()"

**Step 3.3**: Annotate `RaftServer::doVote()`
```cpp
// File: src/deptran/raft/server.h:83 (inline in header)
// @unsafe - Uses raw pointer parameters + calls @unsafe resetTimer()
void doVote(...) {
  *vote_granted = vote;  // Raw pointer parameter
  *reply_term = currentTerm;

  if(can_term > currentTerm) {
    currentTerm = can_term;
  }

  if(vote) {
    setIsLeader(false);  // Unknown status
    vote_for_ = can_id;
    resetTimer();  // @unsafe from Step 2.3
  }
  n_vote_++;
  cb();  // Function pointer call
}
```
- Uses raw pointer parameters
- Calls setIsLeader() [unknown], resetTimer() [@unsafe]
- Mark @unsafe
- Commit: "Mark doVote() as @unsafe - raw pointer parameters"

---

### Phase 4: Analyze .cc Files (Week 2)

Need to read implementation files to understand:
- `server.cc` - All complex RaftServer functions
- `coordinator.cc` - All CoordinatorRaft functions
- `commo.cc` - All RaftCommo RPC functions
- `frame.cc` - Factory methods

After reading each .cc file:
1. Update dependency analysis
2. Classify functions into levels
3. Continue bottom-up annotation

---

### Phase 5: Complex Functions (Week 3-4)

Based on .cc analysis, annotate complex functions following same bottom-up approach:
- Identify new leaves in .cc
- Work up through call graph
- Mark @safe where possible
- Document @unsafe with clear reasons

---

### Phase 6: Fix Violations (Week 5)

For each @safe function that fails borrow checker:
1. Understand the violation
2. Either:
   - Fix the code to be safe
   - Change to @unsafe with documented reason
3. Re-run borrow checker
4. Iterate until all @safe functions pass

---

## Quick Reference

### Borrow Checker Commands

```bash
# Check single file
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party

# Check all Raft files
for file in src/deptran/raft/*.cc; do
  echo "=== Checking $file ==="
  ./third-party/rusty-cpp/target/release/rusty-cpp-checker "$file" \
    -- -I src -I third-party
done

# Save violations to file
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party > violations.txt 2>&1
```

### Build Commands

```bash
# Clean build
rm -rf build && mkdir build
cmake -B build -DRAFT_TEST=OFF
cmake --build build -j32

# Test build
cmake -B build -DRAFT_TEST=ON
cmake --build build -j32
./build/deptran_server -f config/raft_lab_test.yml
```

### Git Workflow

```bash
# Before each step
git status
git diff

# After each function annotation
git add src/deptran/raft/
git commit -m "Mark FunctionName() as @safe/unsafe - reason"

# If something breaks
git revert HEAD
# or
git reset --hard origin/branch
```

---

## Progress Tracking

### Level 0 Functions (10 total)
- [ ] RaftServer::IsLeader()
- [ ] RaftServer::randDuration()
- [ ] RaftVoteQuorumEvent::HasAcceptedValue()
- [ ] RaftVoteQuorumEvent::Term()
- [ ] CoordinatorRaft::n_replica()
- [ ] RaftServer::commo() [@unsafe - raw pointer]
- [ ] CoordinatorRaft::commo() [@unsafe - raw pointer]
- [ ] CoordinatorRaft::GetNextSlot() [@unsafe - raw pointer]
- [ ] RaftServer::GetInstance()
- [ ] RaftServer::GetRaftInstance()

### Level 1 Functions (4 total)
- [ ] CoordinatorRaft::GetQuorum()
- [ ] RaftServer::GetState()
- [ ] RaftServer::resetTimer() [@unsafe - raw pointer]
- [ ] RaftVoteQuorumEvent::FeedResponse()

### Level 2 Functions (3 total)
- [ ] RaftServer::resetTimerBatch() [@unsafe - raw pointer]
- [ ] RaftServer::Reconnect()
- [ ] RaftServer::doVote() [@unsafe - raw pointers]

### Level 3+ Functions (TBD)
- [ ] Analyze all .cc files
- [ ] Build complete dependency graph
- [ ] Continue bottom-up

---

**Next Step**: Start with Level 0, Step 1.1 - Mark `RaftServer::IsLeader()` as @safe

