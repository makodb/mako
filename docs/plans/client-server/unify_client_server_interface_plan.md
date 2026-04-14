# Plan: Unify Client-Server Interfaces in simpleTransactionRep.cc

## Issue Reference
- Source: `issue-1.md`
- Priority: *high*
- Estimated LOC: ~300 (modifications to existing files)

## Requirements Analysis

From `issue-1.md`, the following changes are needed:

### 1. Unified Options (Remove RemoteOptions)
**Current State:**
- `mako::Options` (db.hh): Server-side options (shards, replication, transport)
- `mako::RemoteOptions` (remote_db.hh): Client-side options (server_host, server_port)

**Target State:**
- Single `mako::Options` with mode-specific members:
  - When in client mode: use `server_hosts` (vector for multiple shards)
  - When in server mode: use existing shard/replication config

### 2. Mode Handling with Explicit Enum
**Current State:**
- `--client` flag with separate argument handling
- `--server` flag for server-only mode
- Default mode runs server + tests

**Target State:**
```cpp
enum class RunMode { CLIENT_ONLY, SERVER_ONLY, COLOCATE };
```
- Mode determined by command line flags
- Clean separation of initialization logic by mode

### 3. Multi-Client Support
**Current State:**
- Single client connects to one server
- Manual specification of server_host:port

**Target State:**
- Client mode creates `nshards × nthreads` clients
- Each client connects to its corresponding shard server
- Server addresses derived from configuration or command line

### 4. Code Structure
**Current State:**
- `run_client_mode()` is separate from `run_tests()`
- Different code paths for local vs remote

**Target State:**
- Unified `run_tests(IDatabase* db)` works for both local and remote
- Initialization creates appropriate db (DB or RemoteDB) based on mode
- Tests run via same code path

## Implementation Plan

### Step 1: Extend mako::Options (db.hh)
Add client-mode specific fields to Options struct:
```cpp
struct Options {
    // ... existing fields ...

    // Client mode options
    struct ClientConfig {
        std::vector<std::string> server_hosts;  // One per shard
        std::vector<int> server_ports;          // Corresponding ports
        bool enabled = false;
    } client;
};
```

### Step 2: Modify RemoteDB::Connect signature
Update to accept `mako::Options` instead of `RemoteOptions`:
```cpp
static Status Connect(const Options& options, int shard_index, RemoteDB** dbptr);
```
- Keep backward compatibility with static helper that converts RemoteOptions

### Step 3: Refactor simpleTransactionRep.cc mode handling
```cpp
enum class RunMode { CLIENT_ONLY, SERVER_ONLY, COLOCATE };

// Determine mode from command line
RunMode determine_mode(int argc, char** argv) {
    bool has_client = /* check --client flag */;
    bool has_server = /* check --server flag */;

    if (has_client && has_server) return RunMode::COLOCATE;
    if (has_client) return RunMode::CLIENT_ONLY;
    if (has_server) return RunMode::SERVER_ONLY;
    return RunMode::COLOCATE;  // Default: run both
}

int main() {
    RunMode mode = determine_mode(argc, argv);
    mako::Options opts = parse_options(argc, argv);

    mako::IDatabase* db = nullptr;

    if (mode == RunMode::CLIENT_ONLY) {
        // Create RemoteDB
        mako::RemoteDB* remote_db = nullptr;
        mako::Status s = mako::RemoteDB::Connect(opts, shard_index, &remote_db);
        db = remote_db;
    } else if (mode == RunMode::SERVER_ONLY || mode == RunMode::COLOCATE) {
        // Create local DB
        mako::DB* mako_db = nullptr;
        mako::Status s = mako::DB::Open(opts, "/tmp/mako", &mako_db);
        db = mako_db;

        // Start server infrastructure
        setup_erpc_server();
        setup_helper(...);
        if (mode == RunMode::SERVER_ONLY) {
            setup_client_tcp_server(port);
        }
    }

    // Run tests if not server-only
    if (mode != RunMode::SERVER_ONLY && benchConfig.getLeaderConfig()) {
        run_tests(db);
    }

    // Wait for shutdown if server-only
    if (mode == RunMode::SERVER_ONLY) {
        // ... wait loop ...
    }
}
```

### Step 4: Update client mode test path
- Remove `run_simple_test()` calls inside mode branches
- Use unified `run_tests(db)` for all modes
- Client mode creates multi-shard connections

## Files to Modify

1. **src/mako/db.hh** (~30 LOC)
   - Add ClientConfig to Options struct

2. **src/mako/remote_db.hh** (~40 LOC)
   - Add Connect overload accepting mako::Options
   - Keep RemoteOptions for backward compatibility (deprecate later)

3. **examples/simpleTransactionRep.cc** (~200 LOC)
   - Add RunMode enum
   - Refactor main() to use unified mode handling
   - Update command-line parsing
   - Remove duplicate code paths

4. **docs/client_server_architecture.md** (~30 LOC)
   - Document unified Options usage
   - Update usage examples

## Testing Strategy

1. Run existing CI tests to ensure no regressions
2. Test modes:
   - `./simpleTransactionRep 2 0 6 localhost 1` (COLOCATE)
   - `./simpleTransactionRep --server 2 0 6 localhost 1` (SERVER_ONLY)
   - `./simpleTransactionRep --client localhost 31000` (CLIENT_ONLY)
3. Verify multi-shard client mode works with 2-shard configuration

## Risk Assessment

- **Low risk**: Changes are mostly structural refactoring
- **Backward compatibility**: Keep RemoteOptions for existing code
- **Testing**: Extensive CI coverage already exists

## Timeline

- Analysis: 15 minutes (done)
- Implementation: 1-2 hours
- Testing: 30 minutes
- Documentation: 15 minutes
