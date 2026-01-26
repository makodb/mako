# Unit Tests for MakoClientService Plan

## Issue
ISSUE-1886cab7-3

## Problem
MakoClientService lacks dedicated unit tests. Only integration tests via CI scripts exist.

## Analysis

### What Can Be Unit Tested
MakoClientService has these testable aspects:
1. **Transaction ID generation uniqueness** - atomic counter ensures unique IDs
2. **RPC ID registration** - __reg_to__ registers all 6 RPC IDs correctly
3. **Dispatch routing** - __dispatch__ routes to correct handlers
4. **Unknown RPC ID handling** - returns ENOENT for unknown IDs

### What Requires Integration Testing
Testing Put/Get/Delete operations requires:
- ShardReceiver with actual database
- abstract_db implementation
- Masstree storage layer

These are already covered by the CI clientServer integration test.

### Test Strategy

**Create a focused unit test file** that tests:
1. Transaction ID uniqueness across multiple calls
2. Transaction ID uniqueness with concurrent threads
3. RPC ID constants match expected values
4. Static counter behavior

**Mock approach**: Since we can't easily mock ShardReceiver (it requires database),
we'll test what we can without mocking:
- ID generation (just needs atomic counter)
- RPC constants (static values)

## Changes Required

1. **test/test_client_service.cc** (~150 lines):
   - Test transaction ID generation produces unique IDs
   - Test concurrent ID generation from multiple threads
   - Test RPC ID constants
   - Test that different MakoClientService instances have separate counters

2. **CMakeLists.txt** (~10 lines):
   - Add test_client_service executable
   - Link with appropriate libraries

### Estimated LOC: ~160 lines

## Verification
- Run new tests: `./build/test_client_service`
- Run full CI: `./ci/ci.sh all`
