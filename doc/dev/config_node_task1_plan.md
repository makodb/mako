# Configuration Node - Task 1: Configuration Schema Design

## Overview

This task defines the configuration data structures and RocksDB key schema for persistent configuration storage. The goal is to serialize the `Config` class data so it can be stored in RocksDB and recovered on node reboot.

## Task 1.1: Define Configuration Data Structures for Persistence

### Data Structures to Persist

From `src/deptran/config.h`, the key structures are:

1. **SiteInfo** - Individual server/client site information
   - `siteid_t id` - Unique site ID
   - `uint32_t locale_id` - Datacenter/region grouping
   - `string name` - Site name
   - `string proc_name` - Process name
   - `int role` - 0=leader, 1=follower, 2=learner
   - `string host` - Hostname
   - `uint32_t port` - Port number
   - `uint32_t n_thread` - Thread count
   - `SiteInfoType type_` - CLIENT or SERVER
   - `uint32_t partition_id_` - Partition assignment

2. **ReplicaGroup** - Replication topology
   - `parid_t partition_id` - Partition ID
   - `vector<siteid_t> replica_ids` - Site IDs in this group (derived from replicas pointers)

3. **Protocol Settings**
   - `int32_t tx_proto_` - Transaction protocol enum
   - `int32_t replica_proto_` - Replication protocol enum
   - `int32_t benchmark_` - Workload type
   - `uint64_t txn_timeout_us_` - Transaction timeout

4. **Workload Settings**
   - `vector<double> txn_weight_` - Transaction weights
   - `map<string, double> txn_weights_` - Named transaction weights
   - `uint32_t scale_factor_` - Scale factor

### Serialization Format

We'll use a simple binary format with Marshal for consistency with existing RPC serialization:

```cpp
// config_schema.h

// Serializable SiteInfo for persistence
struct PersistentSiteInfo : public Marshallable {
    siteid_t id;
    uint32_t locale_id;
    string name;
    string proc_name;
    int role;
    string host;
    uint32_t port;
    uint32_t n_thread;
    int type;  // SiteInfoType as int
    uint32_t partition_id;

    // Marshal operators for serialization
    marshal_t& to_marshal(marshal_t& m) const;
    marshal_t& from_marshal(marshal_t& m);
};

// Serializable ReplicaGroup
struct PersistentReplicaGroup : public Marshallable {
    parid_t partition_id;
    vector<siteid_t> replica_ids;

    marshal_t& to_marshal(marshal_t& m) const;
    marshal_t& from_marshal(marshal_t& m);
};

// Serializable protocol settings
struct PersistentProtocolSettings : public Marshallable {
    int32_t tx_proto;
    int32_t replica_proto;
    int32_t benchmark;
    uint64_t txn_timeout_us;
    uint32_t scale_factor;

    marshal_t& to_marshal(marshal_t& m) const;
    marshal_t& from_marshal(marshal_t& m);
};

// Top-level config container
struct PersistentConfig : public Marshallable {
    uint64_t version;  // Config version for cache invalidation
    vector<PersistentSiteInfo> sites;
    vector<PersistentReplicaGroup> replica_groups;
    PersistentProtocolSettings settings;

    marshal_t& to_marshal(marshal_t& m) const;
    marshal_t& from_marshal(marshal_t& m);
};
```

## Task 1.2: Define RocksDB Key Schema

### Key Prefix Scheme

```
config/version                    -> uint64_t version
config/topology/sites             -> serialized vector<PersistentSiteInfo>
config/topology/replicas          -> serialized vector<PersistentReplicaGroup>
config/settings                   -> serialized PersistentProtocolSettings
```

### Design Rationale

1. **Single document per category** - We store sites and replicas as serialized vectors rather than individual keys per site. This simplifies:
   - Atomicity: Write all sites in one RocksDB write batch
   - Reading: Single read to get all sites
   - Versioning: One version counter for all config

2. **Version key** - Separate key allows quick version checks without loading full config.

3. **Binary serialization** - Using Marshal (existing RPC serialization) rather than JSON/protobuf:
   - No additional dependencies
   - Consistent with existing codebase
   - Fast and compact

## Implementation Notes

### Files to Create

1. `src/deptran/config_schema.h` - Data structures (~80 LOC)
2. `src/deptran/config_schema.cc` - Serialization implementation (~70 LOC)

### RustyCpp Safety

- All structures use primitive types and std::string (safe)
- Marshal operators will be marked @unsafe (inherent in I/O)
- Public API will be @safe where possible

## Estimated LOC

- Task 1.1: ~80 LOC (data structures)
- Task 1.2: ~20 LOC (key constants)
- Total: ~100 LOC
