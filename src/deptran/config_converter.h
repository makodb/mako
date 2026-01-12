#pragma once

/**
 * @file config_converter.h
 * @brief Conversion utilities between transport::Configuration and PersistentConfig.
 *
 * This file provides functions to convert between the runtime configuration
 * format (transport::Configuration) and the persistent storage format
 * (PersistentConfig) used by the config node.
 */

#include "config_schema.h"
#include "../mako/lib/configuration.h"
#include <rusty/option.hpp>

namespace janus {

/**
 * @brief Convert transport::Configuration to PersistentConfig for storage.
 *
 * @param config The transport configuration loaded from YAML
 * @param version The version number to assign to the persistent config
 * @return PersistentConfig ready for RocksDB storage
 */
// @safe - Pure data transformation
inline PersistentConfig from_transport_config(
    const transport::Configuration& config,
    uint64_t version) {

    PersistentConfig persistent;
    persistent.version = version;

    // Convert sites from the new format
    if (config.is_new_format) {
        uint32_t site_id = 0;
        for (const auto& [name, site] : config.sites_map) {
            PersistentSiteInfo info;
            info.id = site_id++;
            info.locale_id = 0;  // Default locale
            info.name = site.name;
            info.proc_name = site.name;  // Use site name as proc name
            info.role = site.is_leader ? 0 : 1;  // 0=leader, 1=follower
            info.host = site.ip;
            info.port = static_cast<uint32_t>(site.port);
            info.n_thread = 1;  // Default thread count
            info.type = 1;  // SERVER type
            info.partition_id = static_cast<uint32_t>(site.shard_id);
            persistent.sites.push_back(info);
        }

        // Convert replica groups from shard_map
        for (size_t shard_id = 0; shard_id < config.shard_map.size(); ++shard_id) {
            PersistentReplicaGroup group;
            group.partition_id = static_cast<uint32_t>(shard_id);

            // Find site IDs for this shard's replicas
            for (const auto& site_name : config.shard_map[shard_id]) {
                // Look up site ID by name
                uint32_t id = 0;
                for (const auto& [name, site] : config.sites_map) {
                    if (name == site_name) {
                        group.replica_ids.push_back(id);
                        break;
                    }
                    ++id;
                }
            }
            persistent.replica_groups.push_back(group);
        }
    }

    // Set default protocol settings
    persistent.settings.tx_proto = 0;
    persistent.settings.replica_proto = 0;
    persistent.settings.benchmark = 0;
    persistent.settings.txn_timeout_us = 30000000;
    persistent.settings.scale_factor = 1;

    return persistent;
}

/**
 * @brief Convert PersistentConfig to transport::Configuration for runtime use.
 *
 * This creates a new Configuration object populated with data from persistent
 * storage. The caller is responsible for deleting the returned pointer.
 *
 * @param persistent The persistent configuration from RocksDB
 * @return rusty::Option<transport::Configuration*> containing allocated config,
 *         or None on failure. Caller owns the pointer.
 */
// @unsafe - Allocates memory with new
inline rusty::Option<transport::Configuration*> to_transport_config(
    const PersistentConfig& persistent) {

    // Create a minimal YAML to bootstrap Configuration
    // We'll populate the fields directly after construction
    YAML::Node root;
    root["shards"] = YAML::Node(YAML::NodeType::Sequence);

    // Group sites by partition to build shard map
    std::map<uint32_t, std::vector<std::string>> shard_to_sites;
    for (const auto& site : persistent.sites) {
        shard_to_sites[site.partition_id].push_back(site.name);
    }

    // Build shards array for old format compatibility
    for (const auto& [partition_id, sites] : shard_to_sites) {
        if (!sites.empty()) {
            // Find leader site for this shard
            for (const auto& site : persistent.sites) {
                if (site.partition_id == partition_id && site.role == 0) {
                    YAML::Node shard_entry;
                    shard_entry["host"] = site.host;
                    shard_entry["port"] = std::to_string(site.port);
                    root["shards"].push_back(shard_entry);
                    break;
                }
            }
        }
    }

    // Write temp YAML file
    std::string temp_file = "/tmp/mako_config_" + std::to_string(persistent.version) + ".yml";
    std::ofstream ofs(temp_file);
    if (!ofs) {
        return rusty::None;
    }
    ofs << root;
    ofs.close();

    // @unsafe { new operator }
    transport::Configuration* config = new transport::Configuration(temp_file);

    // Populate with persistent data
    config->is_new_format = true;
    config->nshards = static_cast<int>(shard_to_sites.size());

    // Build sites_map
    for (const auto& site : persistent.sites) {
        transport::SiteInfo info;
        info.name = site.name;
        info.id = static_cast<int>(site.id);
        info.ip = site.host;
        info.port = static_cast<int>(site.port);
        info.is_leader = (site.role == 0);
        info.shard_id = static_cast<int>(site.partition_id);
        info.replica_idx = 0;  // Will be set from replica groups

        config->sites_map[site.name] = info;
    }

    // Build shard_map
    config->shard_map.resize(shard_to_sites.size());
    for (const auto& [partition_id, sites] : shard_to_sites) {
        config->shard_map[partition_id] = sites;
    }

    // Update replica indices
    for (const auto& group : persistent.replica_groups) {
        int replica_idx = 0;
        for (uint32_t site_id : group.replica_ids) {
            // Find site by ID
            for (const auto& site : persistent.sites) {
                if (site.id == site_id) {
                    auto it = config->sites_map.find(site.name);
                    if (it != config->sites_map.end()) {
                        it->second.replica_idx = replica_idx++;
                    }
                    break;
                }
            }
        }
    }

    // Cleanup temp file
    std::remove(temp_file.c_str());

    return rusty::Some(config);
}

}  // namespace janus
