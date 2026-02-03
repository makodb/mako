# Client-Server CI Test Plan

## Overview

This document outlines the plan to enhance the `ci/test_client_server.sh` script to include comprehensive end-to-end testing of the decoupled client-server architecture.

## Current State

The existing test (`ci/test_client_server.sh`) only performs smoke testing:
- Test 1: Usage help verification
- Test 2: makoServer help verification
- Test 3: Client graceful failure when no server is running

**Missing**: Actual end-to-end test with a running server and client performing real operations.

## Implementation Plan

### Test 4: Full Client-Server Communication (NEW)

1. **Start makoServer** in background (single shard, no replication for simplicity)
2. **Wait for server readiness** - Check TCP port availability
3. **Run client operations** - Use `simpleTransactionRep --client` to perform:
   - BeginTransaction
   - Put operation
   - Get operation
   - Commit
4. **Verify results** - Check for success messages in client output
5. **Cleanup** - Kill server process

### Test Configuration

- **Server**: `./makoServer 1 0 4 localhost 0` (single shard, no replication)
- **Client**: `./simpleTransactionRep --client localhost 31000`
- **Timeout**: 30 seconds for entire test
- **Port**: 31000 (default client TCP port)

### Success Criteria

1. Server starts successfully
2. Client connects successfully (no "Failed to connect" message)
3. Transaction operations complete (BeginTransaction: OK)
4. No crashes or hangs

### Estimated LOC

~50 lines of bash script additions.

## Notes

The client-server RPC is fully implemented:
- Server: `src/mako/lib/server.cc` has handlers for all client operations
- Server: `src/mako/lib/client_tcp_server.h` handles TCP connections
- Client: `src/mako/remote_db.hh` implements RemoteDB with full TCP communication

The "stub implementation" message in `simpleTransactionRep.cc::run_client_mode()` is outdated - the actual implementation should work.
