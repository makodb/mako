#pragma once

/**
 * @file config_schema.h
 * @brief Serializable configuration data structures for persistent storage.
 *
 * This file defines the schema for storing cluster configuration in RocksDB.
 * The structures are designed to be serialized using Marshal for persistence
 * and deserialized to reconstruct the Config singleton on node recovery.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <rusty/cell.hpp>
#include "../rrr/misc/marshal.hpp"

namespace janus {

// ============================================================================
// RocksDB Key Constants
// ============================================================================

// @safe - Key prefixes for RocksDB configuration storage
namespace config_keys {

// Version key - stores config version for cache invalidation
constexpr const char* VERSION = "config/version";

// Topology keys - store cluster structure
constexpr const char* SITES = "config/topology/sites";
constexpr const char* REPLICAS = "config/topology/replicas";

// Settings key - stores protocol and workload settings
constexpr const char* SETTINGS = "config/settings";

}  // namespace config_keys

// @safe - Key prefixes for sharding policy storage
namespace sharding_keys {

// Version key - stores policy version for cache invalidation
constexpr const char* VERSION = "sharding/version";

// Policy key - stores the full sharding policy set
constexpr const char* POLICY = "sharding/policy";

}  // namespace sharding_keys

// ============================================================================
// Serializable Data Structures
// ============================================================================

/**
 * @brief Serializable site information for persistence.
 *
 * This mirrors Config::SiteInfo but uses only primitive types
 * for easy serialization.
 */
// @safe - POD structure with no pointers
struct PersistentSiteInfo {
    uint32_t id;           // Unique site ID
    uint32_t locale_id;    // Datacenter/region grouping
    std::string name;      // Site name
    std::string proc_name; // Process name
    int role;              // 0=leader, 1=follower, 2=learner
    std::string host;      // Hostname
    uint32_t port;         // Port number
    uint32_t n_thread;     // Thread count
    int type;              // SiteInfoType as int (0=CLIENT, 1=SERVER)
    uint32_t partition_id; // Partition assignment

    // Default constructor
    // @safe
    PersistentSiteInfo()
        : id(0), locale_id(0), role(0), port(0), n_thread(1), type(1), partition_id(0) {}

    // Constructor from values
    // @safe
    PersistentSiteInfo(uint32_t id, uint32_t locale_id, const std::string& name,
                       const std::string& proc_name, int role, const std::string& host,
                       uint32_t port, uint32_t n_thread, int type, uint32_t partition_id)
        : id(id), locale_id(locale_id), name(name), proc_name(proc_name),
          role(role), host(host), port(port), n_thread(n_thread),
          type(type), partition_id(partition_id) {}
};

// @unsafe - Marshal operators for PersistentSiteInfo
inline rrr::Marshal& operator<<(rrr::Marshal& m, const PersistentSiteInfo& s) {
    m << s.id << s.locale_id << s.name << s.proc_name << s.role
      << s.host << s.port << s.n_thread << s.type << s.partition_id;
    return m;
}

// @unsafe
inline rrr::Marshal& operator>>(rrr::Marshal& m, PersistentSiteInfo& s) {
    m >> s.id >> s.locale_id >> s.name >> s.proc_name >> s.role
      >> s.host >> s.port >> s.n_thread >> s.type >> s.partition_id;
    return m;
}

/**
 * @brief Serializable replica group for persistence.
 *
 * Stores partition ID and the list of site IDs in the replication group.
 * Uses site IDs instead of pointers for serialization.
 */
// @safe - POD structure with no pointers
struct PersistentReplicaGroup {
    uint32_t partition_id;              // Partition ID
    std::vector<uint32_t> replica_ids;  // Site IDs in this group

    // Default constructor
    // @safe
    PersistentReplicaGroup() : partition_id(0) {}

    // Constructor from values
    // @safe
    PersistentReplicaGroup(uint32_t partition_id, const std::vector<uint32_t>& replica_ids)
        : partition_id(partition_id), replica_ids(replica_ids) {}
};

// @unsafe - Marshal operators for PersistentReplicaGroup
inline rrr::Marshal& operator<<(rrr::Marshal& m, const PersistentReplicaGroup& g) {
    m << g.partition_id;
    m << static_cast<uint32_t>(g.replica_ids.size());
    for (const auto& id : g.replica_ids) {
        m << id;
    }
    return m;
}

// @unsafe
inline rrr::Marshal& operator>>(rrr::Marshal& m, PersistentReplicaGroup& g) {
    m >> g.partition_id;
    uint32_t size;
    m >> size;
    g.replica_ids.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        m >> g.replica_ids[i];
    }
    return m;
}

/**
 * @brief Serializable protocol and workload settings.
 */
// @safe - POD structure
struct PersistentProtocolSettings {
    int32_t tx_proto;          // Transaction protocol enum
    int32_t replica_proto;     // Replication protocol enum
    int32_t benchmark;         // Workload type
    uint64_t txn_timeout_us;   // Transaction timeout in microseconds
    uint32_t scale_factor;     // Scale factor

    // Default constructor with sensible defaults
    // @safe
    PersistentProtocolSettings()
        : tx_proto(0), replica_proto(0), benchmark(0),
          txn_timeout_us(30000000), scale_factor(1) {}

    // Constructor from values
    // @safe
    PersistentProtocolSettings(int32_t tx_proto, int32_t replica_proto,
                               int32_t benchmark, uint64_t txn_timeout_us,
                               uint32_t scale_factor)
        : tx_proto(tx_proto), replica_proto(replica_proto), benchmark(benchmark),
          txn_timeout_us(txn_timeout_us), scale_factor(scale_factor) {}
};

// @unsafe - Marshal operators for PersistentProtocolSettings
inline rrr::Marshal& operator<<(rrr::Marshal& m, const PersistentProtocolSettings& s) {
    m << s.tx_proto << s.replica_proto << s.benchmark
      << s.txn_timeout_us << s.scale_factor;
    return m;
}

// @unsafe
inline rrr::Marshal& operator>>(rrr::Marshal& m, PersistentProtocolSettings& s) {
    m >> s.tx_proto >> s.replica_proto >> s.benchmark
      >> s.txn_timeout_us >> s.scale_factor;
    return m;
}

/**
 * @brief Top-level configuration container for serialization.
 *
 * Contains all configuration data needed to reconstruct the Config singleton.
 */
// @safe - Aggregate of safe structures
struct PersistentConfig {
    uint64_t version;                              // Config version for cache invalidation
    std::vector<PersistentSiteInfo> sites;         // All sites (servers and clients)
    std::vector<PersistentReplicaGroup> replica_groups;  // Replication topology
    PersistentProtocolSettings settings;           // Protocol settings

    // Default constructor
    // @safe
    PersistentConfig() : version(0) {}

    // Constructor from values
    // @safe
    PersistentConfig(uint64_t version,
                     const std::vector<PersistentSiteInfo>& sites,
                     const std::vector<PersistentReplicaGroup>& replica_groups,
                     const PersistentProtocolSettings& settings)
        : version(version), sites(sites), replica_groups(replica_groups),
          settings(settings) {}
};

// @unsafe - Marshal operators for PersistentConfig
inline rrr::Marshal& operator<<(rrr::Marshal& m, const PersistentConfig& c) {
    m << c.version;

    // Serialize sites
    m << static_cast<uint32_t>(c.sites.size());
    for (const auto& site : c.sites) {
        m << site;
    }

    // Serialize replica groups
    m << static_cast<uint32_t>(c.replica_groups.size());
    for (const auto& group : c.replica_groups) {
        m << group;
    }

    // Serialize settings
    m << c.settings;

    return m;
}

// @unsafe
inline rrr::Marshal& operator>>(rrr::Marshal& m, PersistentConfig& c) {
    m >> c.version;

    // Deserialize sites
    uint32_t sites_size;
    m >> sites_size;
    c.sites.resize(sites_size);
    for (uint32_t i = 0; i < sites_size; ++i) {
        m >> c.sites[i];
    }

    // Deserialize replica groups
    uint32_t groups_size;
    m >> groups_size;
    c.replica_groups.resize(groups_size);
    for (uint32_t i = 0; i < groups_size; ++i) {
        m >> c.replica_groups[i];
    }

    // Deserialize settings
    m >> c.settings;

    return m;
}

}  // namespace janus
