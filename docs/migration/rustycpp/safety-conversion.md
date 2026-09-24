# SRPC Code Rusty-cpp Safety Conversion Plan

## Current Status
- 15 SRPC files are under borrow checking (reactor, base, misc, rpc)
- 12 remaining files need safety annotations and conversion

## Files to Convert

### Phase 1: Small Utility Files (~500 LOC total)
1. **base/strop.cpp** (92 lines) - String operations
   - Likely mostly string manipulation, may need @unsafe for C-style strings

2. **base/unittest.cpp** (144 lines) - Unit test framework
   - Test infrastructure, may be excluded from borrow checking

3. **misc/rand.cpp** (147 lines) - Random number generation
   - Uses system RNG, will need @unsafe for low-level calls

4. **misc/recorder.cpp** (175 lines) - Recording/replay functionality
   - File I/O operations, @unsafe for system calls

### Phase 2: Message Queue (mq) (~1078 LOC total)
5. **mq/buf.cpp** (143 lines) - Message queue buffer
6. **mq/client.cpp** (229 lines) - Message queue client
7. **mq/polling.cpp** (240 lines) - Message queue polling
8. **mq/rpc.cpp** (124 lines) - Message queue RPC
9. **mq/server.cpp** (342 lines) - Message queue server

### Phase 3: Remote Logging (rlog) (~315 LOC total)
10. **rlog/log_server.cpp** (80 lines) - Remote log server
11. **rlog/log_service_impl.cpp** (97 lines) - Remote log implementation
12. **rlog/rlog.cpp** (138 lines) - Remote logging

## Approach for Each File
1. Add safety annotations (@safe/@unsafe) to all functions
2. Mark low-level operations (system calls, raw pointers) as @unsafe
3. Convert STL containers to rusty equivalents where beneficial
4. Add file to SRPC_BORROW_SRC in CMakeLists.txt
5. Run borrow checker and fix any violations
6. Run tests to verify no regressions

## Success Criteria
- All SRPC files have safety annotations
- Only system calls and low-level operations remain in @unsafe blocks
- All files pass borrow checking
- All tests pass

## Estimated Total: ~2000 LOC to review and annotate
