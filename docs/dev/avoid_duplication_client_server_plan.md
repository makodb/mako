# Plan: Avoid Duplication in Decoupled Client-Server

## Overview

This document describes the plan to consolidate `makoServer.cc` functionality into `simpleTransactionRep.cc`, eliminating code duplication between the two files.

## Problem Statement

Currently, two files exist with duplicated server initialization code:
- `examples/makoServer.cc` (199 lines) - Standalone server that waits for shutdown
- `examples/simpleTransactionRep.cc` (1076 lines) - Server + tests, also has client mode

The duplication is primarily in:
1. Argument parsing (lines 64-83 in makoServer vs 965-969 in simpleTransactionRep)
2. Database options configuration (lines 119-131 vs 995-1006)
3. RPC server setup (lines 147-175 vs 1022-1035)
4. Signal handling for shutdown (lines 33-39 vs not present)

## Solution Design

Consolidate into `simpleTransactionRep.cc` with three modes:

```
./simpleTransactionRep <args>           # Server mode + tests (current behavior)
./simpleTransactionRep --server <args>  # Server-only mode (like makoServer)
./simpleTransactionRep --client <host> <port>  # Client mode (existing)
```

### Command-Line Interface

| Mode | Command | Description |
|------|---------|-------------|
| Server + Tests | `./simpleTransactionRep 2 0 6 localhost 1` | Run DB server and execute tests |
| Server Only | `./simpleTransactionRep --server 2 0 6 localhost 1` | Run DB server, wait for shutdown |
| Client Only | `./simpleTransactionRep --client localhost 31000` | Connect to remote server |

## Implementation Steps

### Step 1: Add Signal Handler to simpleTransactionRep.cc (~10 LOC)

Add the same signal handler that makoServer.cc uses:
```cpp
static std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signum) {
    printf("\nReceived signal %d, initiating shutdown...\n", signum);
    g_shutdown_requested.store(true);
}
```

### Step 2: Add --server Flag Parsing (~15 LOC)

Modify `main()` to detect `--server` flag:
```cpp
bool server_only_mode = false;
int arg_offset = 0;

if (argc >= 2 && strcmp(argv[1], "--server") == 0) {
    server_only_mode = true;
    arg_offset = 1;
}
```

### Step 3: Add Server-Only Mode Logic (~30 LOC)

After RPC server setup, check mode:
```cpp
if (server_only_mode) {
    // Server-only mode - wait for shutdown
    printf("\nServer running. Press Ctrl+C to shutdown.\n");
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("\nShutting down server...\n");
} else {
    // Default mode - run tests
    run_tests(mako_db);
}
```

### Step 4: Setup Client TCP Server for Server-Only Mode (~10 LOC)

Add the client TCP server setup (from makoServer.cc):
```cpp
if (benchConfig.getLeaderConfig()) {
    // ... existing RPC setup ...

    // Start TCP server for RemoteDB client connections
    int client_port = 31000 + shardIdx;
    if (mako::setup_client_tcp_server(client_port)) {
        printf("Client TCP server started on port %d\n", client_port);
    }
}
```

### Step 5: Remove makoServer.cc (~-200 LOC)

- Delete `examples/makoServer.cc`
- Remove from `CMakeLists.txt`

### Step 6: Update Documentation

Update the following files to reflect the consolidated interface:
- `docs/dev/client_decoupling_design.md`
- `docs/dev/client_rpc_implementation_plan.md`
- `docs/dev/multi_client_support_plan.md`

### Step 7: Update CI Tests

Verify/update CI tests that use makoServer:
- `ci/test_client_server.sh`

## Estimated LOC Changes

| Component | LOC |
|-----------|-----|
| Signal handler | +10 |
| --server flag parsing | +15 |
| Server-only mode logic | +30 |
| Client TCP server setup | +10 |
| Remove makoServer.cc | -199 |
| Remove CMakeLists entry | -5 |
| Documentation updates | ~50 |
| **Net change** | **-89** |

## Testing Strategy

1. Verify existing `--client` mode still works
2. Test new `--server` mode starts and waits for shutdown
3. Test combined client-server: start `--server`, connect with `--client`
4. Run full CI suite to ensure no regressions

## RustyCpp Safety Notes

- Signal handler uses `std::atomic<bool>` - thread-safe
- No new unsafe code introduced
- All existing safety annotations remain valid

## Success Criteria

1. `makoServer.cc` is completely removed
2. `simpleTransactionRep.cc` supports all three modes
3. All CI tests pass
4. Documentation is updated
