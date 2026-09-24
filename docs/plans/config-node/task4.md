# Config Node Task 4: Implement Config Fetching for Other Nodes

## Overview

This task implements the client-side logic for fetching configuration from a
Configuration Node (c-node). Non-c-node processes will connect to the c-node
at startup to retrieve cluster configuration instead of loading from local
YAML files.

## Design

### ConfigClient Class

```cpp
class ConfigClient {
private:
    std::string c_node_addr_;           // Address of config node (host:port)
    srpc::PollMgr* poll_mgr_;            // Polling manager for RPC
    srpc::Client* rpc_client_;           // RPC client connection
    ConfigServiceProxy* proxy_;         // Generated proxy for ConfigService

    // Retry configuration
    uint32_t max_retries_;              // Max retry attempts (default: 10)
    uint32_t retry_delay_ms_;           // Initial retry delay (default: 1000)
    uint32_t max_retry_delay_ms_;       // Max retry delay (default: 30000)
    uint32_t connect_timeout_ms_;       // Connection timeout (default: 5000)

public:
    explicit ConfigClient(const std::string& c_node_addr);
    ~ConfigClient();

    // Connect to config node with retry
    // @safe - Uses rusty types internally
    bool connect();

    // Disconnect from config node
    // @safe
    void disconnect();

    // Fetch configuration from c-node
    // Returns Option::None if fetch fails after all retries
    // @safe - Returns rusty::Option
    rusty::Option<PersistentConfig> fetch_config();

    // Fetch only version (lightweight check)
    // @safe
    rusty::Option<uint64_t> fetch_version();

    // Check if c-node has configuration
    // @safe
    rusty::Option<bool> has_config();

    // Set retry configuration
    void set_max_retries(uint32_t retries);
    void set_retry_delay_ms(uint32_t delay_ms);
    void set_max_retry_delay_ms(uint32_t max_delay_ms);
    void set_connect_timeout_ms(uint32_t timeout_ms);
};
```

### RPC Usage Pattern

The client uses the `ConfigServiceProxy` generated from `rcc_rpc.rpc`:

```cpp
// Synchronous call pattern using Future
void ConfigClient::fetch_config_impl() {
    uint64_t current_version = 0;
    int32_t has_update = 0;
    std::string config_data;

    // Call async RPC, get Future
    auto future = proxy_->async_GetConfig(0);  // client_version=0 means get all

    // Wait for completion with timeout
    if (future->timed_wait(connect_timeout_ms_)) {
        // RPC completed
        current_version = future->get_reply<uint64_t>(0);
        has_update = future->get_reply<int32_t>(1);
        config_data = future->get_reply<std::string>(2);

        if (has_update && !config_data.empty()) {
            // Deserialize config_data to PersistentConfig
        }
    }
}
```

### Retry Logic with Exponential Backoff

```cpp
bool ConfigClient::connect_with_retry() {
    uint32_t retries = 0;
    uint32_t delay_ms = retry_delay_ms_;

    while (retries < max_retries_) {
        if (try_connect()) {
            return true;
        }

        // Exponential backoff with cap
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        delay_ms = std::min(delay_ms * 2, max_retry_delay_ms_);
        retries++;
    }

    return false;
}
```

## Files to Create/Modify

| File | Purpose |
|------|---------|
| `src/deptran/config_client.h` | New: ConfigClient class declaration |
| `src/deptran/config_client.cc` | New: ConfigClient implementation |
| `test/config_client_test.cc` | New: Unit tests for ConfigClient |
| `CMakeLists.txt` | Add config_client.cc to txlog, add test executable |

## Implementation Steps

1. **Create config_client.h** (~80 LOC)
   - ConfigClient class declaration
   - Include necessary headers (rcc_rpc.h, config_schema.h)
   - Use rusty types (Option, Cell) for state

2. **Create config_client.cc** (~150 LOC)
   - Constructor: parse address, create PollMgr
   - connect(): create Client, connect to c-node
   - disconnect(): cleanup resources
   - fetch_config(): call GetConfig RPC, deserialize response
   - fetch_version(): call GetConfigVersion RPC
   - has_config(): call HasConfig RPC
   - Retry logic with exponential backoff

3. **Create unit tests** (~150 LOC)
   - Test construction with valid/invalid addresses
   - Test connection to mock server
   - Test fetch_config returns valid data
   - Test retry logic on connection failure
   - Test timeout handling

## Dependencies

- Task 3 (ConfigService RPC) - provides server-side implementation
- Generated `ConfigServiceProxy` from rcc_rpc.rpc
- `PersistentConfig` from config_schema.h

## RustyCpp Safety

All new code will:
- Use `rusty::Option<T>` instead of raw pointers/nulls for optional returns
- Use `rusty::Cell<T>` for interior mutability where needed
- Include `@safe` or `@unsafe` annotations on all functions
- Mark calls to RPC layer as `@unsafe` (not borrow-checked)

## Testing Strategy

1. **Unit Tests** (mock server)
   - ConfigClient construction
   - Connection success/failure
   - Config fetch roundtrip
   - Version-only fetch
   - HasConfig check

2. **Integration Tests** (real server)
   - Start ConfigServiceImpl server
   - ConfigClient fetches from it
   - Verify config matches expected

## Success Criteria

1. ConfigClient successfully connects to c-node
2. fetch_config() returns valid PersistentConfig
3. Retry logic works with exponential backoff
4. Timeout handling prevents indefinite hangs
5. All unit tests pass
6. Code passes borrow checking (where applicable)
