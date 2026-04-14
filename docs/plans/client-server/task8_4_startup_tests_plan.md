# Task 8.4: Startup Tests Plan

## Objective
Add comprehensive tests for the sharding policy startup flow integration.

## Existing Test Coverage
- Unit tests in `test_sharding_policy_test.cc` (34 tests): Policy structures, builder, key extraction
- Unit tests in `test_tpcc_sharding_test.cc` (19 tests): TPC-C sharding initialization
- Integration tests in `test_config_service_test.cc` (9 tests): RPC and storage

## Missing Test Coverage
1. `init_config_node()` function - First boot vs reboot behavior
2. `fetch_sharding_policy_from_cnode()` function - Data node startup
3. `send_tpcc_sharding_policy_to_cnode()` function - Initializer node startup
4. End-to-end startup flow simulation

## Test Plan

### New Test File: `test/sharding_startup_test.cc`

#### Test Categories

1. **C-Node First Boot Tests** (~50 LOC)
   - `CNodeFirstBootNoStoredPolicy`: Verify c-node starts with no policy on first boot
   - `CNodeFirstBootInitializesService`: Verify config service starts and serves RPCs

2. **C-Node Reboot Tests** (~70 LOC)
   - `CNodeRebootLoadsStoredPolicy`: Verify c-node loads policy from RocksDB on reboot
   - `CNodeRebootPolicyAvailableViaRpc`: Verify loaded policy is served via RPC

3. **Data Node Startup Tests** (~100 LOC)
   - `DataNodeFetchFromCNode`: Verify data node fetches policy successfully
   - `DataNodeFetchNoPolicyFallback`: Verify fallback when no policy exists
   - `DataNodeFetchConnectionFailure`: Verify retry behavior on connection failure
   - `DataNodeNoCNodeAddrConfigured`: Verify graceful handling when c-node addr not set

4. **Initializer Node Startup Tests** (~100 LOC)
   - `InitializerSendsPolicyToCNode`: Verify initializer sends policy to c-node
   - `InitializerPolicyPersisted`: Verify sent policy is persisted in c-node
   - `InitializerInitializesLocalCache`: Verify local cache is initialized after send
   - `InitializerConnectionFailure`: Verify error handling on connection failure

5. **End-to-End Flow Tests** (~80 LOC)
   - `FullStartupFlow`: C-node starts → Initializer sends policy → Data nodes fetch
   - `StartupAfterReboot`: Verify all nodes recover state after reboot

## Implementation Details

### Test Fixtures
```cpp
// @safe - Test fixture for startup tests
class ShardingStartupTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    std::string temp_db_path_;

    void SetUp() override;
    void TearDown() override;

    // Helper: Create a c-node server
    std::unique_ptr<Server> create_cnode_server(int port);

    // Helper: Allocate unique port
    int next_port();
};
```

### Safety Annotations
- All test methods will have `// @safe` or `// @unsafe` annotations
- External I/O (RocksDB, RPC) will be marked with `// @unsafe { reason }` blocks

## Estimated LOC
- Test file: ~400 LOC (within 500 line limit)
- Test count: ~14 tests

## Success Criteria
1. All tests pass consistently
2. Tests verify startup flow works correctly
3. Tests verify error handling (connection failures, missing config)
4. All code follows rusty-cpp guidelines
