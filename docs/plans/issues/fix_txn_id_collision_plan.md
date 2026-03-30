# Fix Transaction ID Collision Risk Plan

## Issue
ISSUE-1886cab7-1, ISSUE-33b02756-1

## Problem
`HandleBeginTxn` in `src/mako/client_service.cc:65-72` uses `client_id` directly as `txn_id`. This causes collisions when:
1. A client calls BeginTxn multiple times (same txn_id for different transactions)
2. Multiple clients happen to have the same client_id

## Solution
Implement the documented txn_id encoding scheme:
- Upper 32 bits: client_id (unique per client connection)
- Lower 32 bits: per-client transaction counter

### Changes Required

1. **client_service.h**:
   - Add `std::atomic<uint32_t> next_txn_counter_` member
   - Initialize to 0 in constructor

2. **client_service.cc** (HandleBeginTxn):
   - Change: `uint64_t txn_id = static_cast<uint64_t>(client_id);`
   - To: `uint64_t txn_id = (static_cast<uint64_t>(client_id) << 32) | next_txn_counter_.fetch_add(1);`

3. **docs/client_server_architecture.md**:
   - Add note that the encoding is now implemented

### Estimated LOC: ~10 lines changed

## Verification
- Run CI tests: `./ci/ci.sh all`
- The clientServer test should continue to pass
