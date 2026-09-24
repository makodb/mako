# Configuration Node (C-Node) Plan

## Overview

This document describes the plan to implement a Configuration Node (C-Node) that stores cluster configuration persistently in RocksDB. On system reboot, the c-node recovers configuration from RocksDB and serves it to other nodes via RPC.

## Goals

1. **Persistent configuration**: Configuration survives c-node reboot
2. **Centralized distribution**: Other nodes fetch configuration from c-node
3. **Simple design**: Single c-node, no replication (can extend later)
4. **Minimal changes**: Reuse existing RocksDB and RPC infrastructure

## Current State

### How Configuration Works Today

```
Startup Flow (Current):
  1. Each node loads YAML config file independently
  2. YAML parsed into Config singleton
  3. Config is read-only after startup
  4. No persistence - config lost on reboot
```

### What's in Config Singleton

| Category | Data | Source |
|----------|------|--------|
| Topology | `sites_` - all server sites | YAML `site.server` |
| Topology | `replica_groups_` - partition → replicas | YAML `site.server` |
| Topology | `proc_host_map_` - process → host | YAML `host` |
| Settings | `tx_proto_` - transaction protocol | YAML `mode.cc` |
| Settings | `repl_proto_` - replication protocol | YAML `mode.ab` |
| Settings | `txn_timeout_us_` - transaction timeout | YAML `mode.txn_timeout_ms` |
| Workload | `benchmark_` - workload type | YAML `bench` |
| Workload | scale factors, weights | YAML `bench` |

### RocksDB Usage Today

- Used only for transaction log persistence (`src/mako/rocksdb_persistence.cc`)
- Stores: log entries, epoch, shard metadata
- Does NOT store: configuration, topology

## Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────────┐
│                    C-Node (Configuration Node)                   │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │   Config    │  │ ConfigStore │  │    ConfigService        │  │
│  │  Singleton  │←→│  (RocksDB)  │  │    (RPC Server)         │  │
│  └─────────────┘  └─────────────┘  └───────────┬─────────────┘  │
│                                                 │                │
└─────────────────────────────────────────────────┼────────────────┘
                                                  │ GetConfig RPC
                    ┌─────────────────────────────┼─────────────────┐
                    │                             │                 │
              ┌─────▼─────┐                 ┌─────▼─────┐           │
              │  Node 1   │                 │  Node 2   │    ...    │
              │           │                 │           │           │
              │ ConfigClient                │ ConfigClient          │
              │     ↓     │                 │     ↓     │           │
              │  Config   │                 │  Config   │           │
              │ Singleton │                 │ Singleton │           │
              └───────────┘                 └───────────┘           │
                                                                    │
```

### C-Node Startup Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    C-Node Startup                                │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌───────────────────┐
                    │ Check RocksDB for │
                    │ existing config   │
                    └─────────┬─────────┘
                              │
              ┌───────────────┴───────────────┐
              │                               │
              ▼                               ▼
     ┌────────────────┐              ┌────────────────┐
     │ First Boot     │              │ Reboot         │
     │ (no RocksDB)   │              │ (RocksDB exists)│
     └───────┬────────┘              └───────┬────────┘
             │                               │
             ▼                               ▼
     ┌────────────────┐              ┌────────────────┐
     │ Load YAML      │              │ Load from      │
     │ config file    │              │ RocksDB        │
     └───────┬────────┘              └───────┬────────┘
             │                               │
             ▼                               │
     ┌────────────────┐                      │
     │ Save to        │                      │
     │ RocksDB        │                      │
     └───────┬────────┘                      │
             │                               │
             └───────────────┬───────────────┘
                             │
                             ▼
                    ┌────────────────┐
                    │ Initialize     │
                    │ Config singleton│
                    └───────┬────────┘
                            │
                            ▼
                    ┌────────────────┐
                    │ Start          │
                    │ ConfigService  │
                    └───────┬────────┘
                            │
                            ▼
                    ┌────────────────┐
                    │ Normal startup │
                    │ (server ops)   │
                    └────────────────┘
```

### Other Node Startup Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    Other Node Startup                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌───────────────────┐
                    │ Parse --config-node│
                    │ <host:port>        │
                    └─────────┬─────────┘
                              │
                              ▼
                    ┌───────────────────┐
                    │ Connect to c-node │
                    │ (with retry)      │
                    └─────────┬─────────┘
                              │
                    ┌─────────┴─────────┐
                    │                   │
                    ▼                   ▼
           ┌────────────────┐  ┌────────────────┐
           │ Success        │  │ Failure        │
           │                │  │ (after retries)│
           └───────┬────────┘  └───────┬────────┘
                   │                   │
                   ▼                   ▼
           ┌────────────────┐  ┌────────────────┐
           │ Call GetConfig │  │ Exit with      │
           │ RPC            │  │ error          │
           └───────┬────────┘  └────────────────┘
                   │
                   ▼
           ┌────────────────┐
           │ Deserialize    │
           │ config data    │
           └───────┬────────┘
                   │
                   ▼
           ┌────────────────┐
           │ Initialize     │
           │ Config singleton│
           └───────┬────────┘
                   │
                   ▼
           ┌────────────────┐
           │ Normal startup │
           │ (server ops)   │
           └────────────────┘
```

## Implementation Details

### Task 1: Configuration Schema

#### 1.1 Data Structures for Persistence

```cpp
// Serializable version of SiteInfo
struct SiteInfoData {
  uint32_t id;
  uint32_t locale_id;
  std::string name;
  std::string proc_name;
  int role;  // 0=leader, 1=follower, 2=learner
  std::string host;
  uint32_t port;
  uint32_t n_thread;
  int type;  // CLIENT or SERVER
  uint32_t partition_id;

  // Serialize to JSON string
  std::string to_json() const;
  // Deserialize from JSON string
  static SiteInfoData from_json(const std::string& json);
};

// Serializable version of ReplicaGroup
struct ReplicaGroupData {
  uint32_t partition_id;
  std::vector<uint32_t> site_ids;  // IDs of replicas

  std::string to_json() const;
  static ReplicaGroupData from_json(const std::string& json);
};

// Full configuration bundle
struct ConfigData {
  uint64_t version;
  std::vector<SiteInfoData> sites;
  std::vector<ReplicaGroupData> replica_groups;
  std::map<std::string, std::string> proc_host_map;
  int tx_proto;
  int repl_proto;
  uint64_t txn_timeout_us;
  int benchmark_type;
  // ... other settings

  std::string to_json() const;
  static ConfigData from_json(const std::string& json);
};
```

#### 1.2 RocksDB Key Schema

```
Key                               Value Type
─────────────────────────────────────────────────────────
config/version                    uint64_t (binary, 8 bytes)
config/data                       JSON string (full ConfigData)
```

Simple approach: Store entire configuration as single JSON blob under `config/data`. This avoids complexity of multi-key transactions.

### Task 2: ConfigStore Class

```cpp
// src/deptran/config_store.h

#pragma once

#include <string>
#include <memory>
#include <rocksdb/db.h>
#include "config.h"

namespace janus {

class ConfigStore {
public:
  // Initialize with path to RocksDB directory
  // @unsafe - RocksDB operations
  bool Initialize(const std::string& db_path);

  // Close RocksDB
  void Close();

  // Check if configuration exists in RocksDB
  // @safe
  bool HasConfig() const;

  // Get current configuration version
  // @safe
  uint64_t GetVersion() const;

  // Save configuration to RocksDB
  // @unsafe - RocksDB write
  bool Save(const Config* config);

  // Load configuration from RocksDB
  // Returns nullptr if no config stored
  // @unsafe - RocksDB read, memory allocation
  std::unique_ptr<Config> Load();

private:
  std::unique_ptr<rocksdb::DB> db_;
  std::string db_path_;
  uint64_t cached_version_ = 0;

  // Serialize Config to JSON
  std::string SerializeConfig(const Config* config);

  // Deserialize JSON to Config
  std::unique_ptr<Config> DeserializeConfig(const std::string& json);
};

}  // namespace janus
```

### Task 3: ConfigService RPC

```cpp
// src/deptran/config_service.h

#pragma once

#include "srpc/srpc.hpp"
#include "config.h"

namespace janus {

// RPC IDs for config service
constexpr uint32_t RPC_CONFIG_GET = 0xC001;
constexpr uint32_t RPC_CONFIG_GET_VERSION = 0xC002;

class ConfigService : public srpc::Service {
public:
  ConfigService(Config* config) : config_(config) {}

  // RPC: Get full configuration
  // Input: client_version (uint64_t) - client's current version (0 if none)
  // Output: config_data (string), current_version (uint64_t)
  void GetConfig(uint64_t client_version,
                 std::string* config_data,
                 uint64_t* current_version);

  // RPC: Get configuration version only
  // Output: version (uint64_t)
  void GetConfigVersion(uint64_t* version);

  // Register RPC handlers
  void __dispatch__(srpc::Request* req, srpc::ServerConnection* sconn) override;

private:
  Config* config_;
  std::string cached_serialized_config_;
  uint64_t cached_version_ = 0;
};

}  // namespace janus
```

### Task 4: ConfigClient

```cpp
// src/deptran/config_client.h

#pragma once

#include <string>
#include <memory>
#include "config.h"
#include "srpc/srpc.hpp"

namespace janus {

class ConfigClient {
public:
  // Connect to c-node and fetch configuration
  // @unsafe - network operations
  static std::unique_ptr<Config> FetchConfig(
      const std::string& c_node_addr,
      int max_retries = 5,
      int retry_delay_ms = 1000);

private:
  // Deserialize received config data
  static std::unique_ptr<Config> DeserializeConfig(const std::string& data);
};

}  // namespace janus
```

### Task 5: Startup Integration

#### Command-line Arguments

```cpp
// New arguments in config.cc CreateConfig()

// C-node mode
if (strcmp(argv[i], "--is-config-node") == 0) {
  is_config_node_ = true;
}

// Other nodes: specify c-node address
if (strcmp(argv[i], "--config-node") == 0) {
  config_node_addr_ = argv[++i];  // e.g., "192.168.1.100:9999"
}

// C-node data directory
if (strcmp(argv[i], "--config-db-path") == 0) {
  config_db_path_ = argv[++i];  // e.g., "/var/lib/mako/config_db"
}
```

#### Modified Config::CreateConfig()

```cpp
Config* Config::CreateConfig(int argc, char** argv) {
  // Parse command-line args first
  ParseArgs(argc, argv);

  if (!config_node_addr_.empty()) {
    // Other node mode: fetch from c-node
    Log_info("Fetching configuration from c-node: %s", config_node_addr_.c_str());
    auto config = ConfigClient::FetchConfig(config_node_addr_);
    if (!config) {
      Log_fatal("Failed to fetch configuration from c-node");
      return nullptr;
    }
    config_s = config.release();
    return config_s;
  }

  if (is_config_node_) {
    // C-node mode
    ConfigStore store;
    store.Initialize(config_db_path_);

    if (store.HasConfig()) {
      // Reboot: load from RocksDB
      Log_info("C-node reboot: loading configuration from RocksDB");
      auto config = store.Load();
      config_s = config.release();
    } else {
      // First boot: load from YAML, save to RocksDB
      Log_info("C-node first boot: loading from YAML, saving to RocksDB");
      config_s = new Config();
      config_s->LoadYML(yaml_path_);
      store.Save(config_s);
    }

    // Start ConfigService (done elsewhere in server startup)
    return config_s;
  }

  // Default: load from YAML (backward compatible)
  config_s = new Config();
  config_s->LoadYML(yaml_path_);
  return config_s;
}
```

### Task 6: Tests

#### 6.1 ConfigStore Unit Test

```cpp
// test/config_store_test.cpp

TEST(ConfigStore, SaveLoadRoundtrip) {
  // Create temp directory
  std::string db_path = "/tmp/config_test_" + std::to_string(getpid());

  ConfigStore store;
  ASSERT_TRUE(store.Initialize(db_path));

  // Create test config
  auto config = CreateTestConfig();

  // Save
  ASSERT_TRUE(store.Save(config.get()));
  EXPECT_EQ(store.GetVersion(), 1);

  // Load
  auto loaded = store.Load();
  ASSERT_NE(loaded, nullptr);

  // Verify
  EXPECT_EQ(loaded->GetSiteCount(), config->GetSiteCount());
  // ... more assertions

  // Cleanup
  store.Close();
  std::filesystem::remove_all(db_path);
}

TEST(ConfigStore, PersistenceAcrossRestart) {
  std::string db_path = "/tmp/config_persist_test";

  // First "process"
  {
    ConfigStore store;
    store.Initialize(db_path);
    auto config = CreateTestConfig();
    store.Save(config.get());
  }

  // Second "process" (simulating reboot)
  {
    ConfigStore store;
    store.Initialize(db_path);
    EXPECT_TRUE(store.HasConfig());

    auto loaded = store.Load();
    ASSERT_NE(loaded, nullptr);
    // Verify config matches
  }

  std::filesystem::remove_all(db_path);
}
```

#### 6.2 Integration Test Script

```bash
#!/bin/bash
# examples/test_config_node.sh

echo "Testing Configuration Node..."

# Start c-node
CMD_CNODE="./${BUILD_DIR}/dbtest \
    --is-config-node \
    --config-db-path /tmp/mako_config_db \
    -f config/test_config.yml \
    -P cnode \
    --config-service-port 9999"

nohup $CMD_CNODE > cnode.log 2>&1 &
CNODE_PID=$!
sleep 3

# Start other node fetching from c-node
CMD_NODE="./${BUILD_DIR}/dbtest \
    --config-node localhost:9999 \
    -P node1"

nohup $CMD_NODE > node1.log 2>&1 &
NODE_PID=$!
sleep 3

# Verify node1 got configuration
if grep -q "Fetched configuration from c-node" node1.log; then
    echo "✓ Node fetched config from c-node"
else
    echo "✗ Node failed to fetch config"
    exit 1
fi

# Kill c-node, restart it (test persistence)
kill $CNODE_PID
sleep 2

nohup $CMD_CNODE > cnode_reboot.log 2>&1 &
CNODE_PID=$!
sleep 3

if grep -q "loading configuration from RocksDB" cnode_reboot.log; then
    echo "✓ C-node loaded config from RocksDB on reboot"
else
    echo "✗ C-node did not load from RocksDB"
    exit 1
fi

# Cleanup
kill $CNODE_PID $NODE_PID 2>/dev/null
rm -rf /tmp/mako_config_db

echo "SUCCESS: Config node tests passed"
```

## Estimated LOC

| Task | LOC |
|------|-----|
| Task 1: Schema design | ~50 |
| Task 2: ConfigStore | ~200 |
| Task 3: ConfigService RPC | ~150 |
| Task 4: ConfigClient | ~150 |
| Task 5: Startup integration | ~100 |
| Task 6: Tests | ~200 |
| **Total** | **~850** |

## Success Criteria

1. **C-node persistence**: Configuration saved to RocksDB on first boot
2. **C-node recovery**: Configuration loaded from RocksDB on reboot (YAML not needed)
3. **Config distribution**: Other nodes successfully fetch config via RPC
4. **Backward compatibility**: Nodes can still load from YAML if no c-node specified
5. **Tests pass**: All unit and integration tests pass

## Future Extensions (Not in This Phase)

1. **Runtime updates**: Update configuration while system is running
2. **High availability**: Multiple c-nodes with leader election
3. **Change notifications**: Push config changes to other nodes
4. **Configuration history**: Store previous versions, enable rollback
5. **Access control**: Authenticate config requests

## RustyCpp Compliance

All new code must follow RustyCpp safety requirements:

```cpp
// ConfigStore methods
// @unsafe - RocksDB operations are not borrow-checked
bool ConfigStore::Save(const Config* config) { ... }

// @safe - Pure read from cached value
uint64_t ConfigStore::GetVersion() const {
  return cached_version_;
}
```

## References

- Current config: `src/deptran/config.h`, `config.cc`
- RocksDB usage: `src/mako/rocksdb_persistence.cc`
- RPC framework: `src/srpc/rpc/`
- YAML parsing: Uses `yaml-cpp` library
