// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * configuration_unified.cc:
 *   Unified configuration implementation
 *
 **********************************************************************/

#include "lib/configuration.h"
#include "lib/assert.h"
#include <algorithm>

namespace transport
{
    ShardAddress::ShardAddress(const string &host, const string &port, const int &clusterRole) :
        host(host), port(port), clusterRole(clusterRole)
    {
        cluster = mako::convertClusterRole(clusterRole);
    }

    bool ShardAddress::operator==(const ShardAddress &other) const
    {
        return host == other.host && port == other.port;
    }

    bool ShardAddress::operator<(const ShardAddress &other) const
    {
        if (host != other.host) return host < other.host;
        return port < other.port;
    }

    Configuration::Configuration(std::string file)
    {
        configFile = file;
        warehouses = 1; // default
        nshards = 0;
        is_new_format = false;
        multi_shard_mode = false;  // default: single-shard mode
        // local_shard_indices will be populated later via command-line args

        YAML::Node config = YAML::LoadFile(file);

        // Detect format
        is_new_format = DetectFormat(config);

        if (is_new_format) {
            ParseNewFormat(config);
        } else {
            ParseOldFormat(config);
        }
    }

    Configuration::~Configuration()
    {
    }

    bool Configuration::DetectFormat(YAML::Node& config)
    {
        // New format has "sites" and "shard_map"
        // Old format has "shards" (int) and cluster sections like "localhost", "p1", etc.
        return config["sites"] && config["shard_map"];
    }

    void Configuration::ParseOldFormat(YAML::Node& config)
    {
        Notice("Using old configuration format");
        
        int num_shards = config["shards"].as<int>();
        warehouses = config["warehouses"].as<int>();
        nshards = num_shards;
        
        for (auto cluster: {mako::LOCALHOST_CENTER,
                           mako::P1_CENTER,
                           mako::P2_CENTER,
                           mako::LEARNER_CENTER}) {
            auto node = config[cluster];
            if (!node) continue;
            int cnt = 0;
            for (int i = 0; i < node.size(); ++i) {
                auto item = node[i];
                auto ip = item["ip"].as<string>();
                auto port = item["port"].as<int>();
                shards.push_back(ShardAddress(ip, std::to_string(port), mako::convertCluster(cluster)));
                cnt++;
            }
            if (cnt != nshards) {
                Panic("shards are not matched in configuration, got: %d, required: %d!", cnt, nshards);
            }
        }
        
        mports[mako::LOCALHOST_CENTER_INT] = config["memlocalhost"].as<int>();
        mports[mako::LEARNER_CENTER_INT] = config["memlearner"].as<int>();
        mports[mako::P1_CENTER_INT] = config["memp1"].as<int>();
        mports[mako::P2_CENTER_INT] = config["memp2"].as<int>();
    }

    void Configuration::ParseNewFormat(YAML::Node& config)
    {
        Notice("Using new configuration format");
        
        // Parse warehouses if present
        if (config["warehouses"]) {
            warehouses = config["warehouses"].as<int>();
        }
        
        // Parse sites
        auto sites_node = config["sites"];
        for (size_t i = 0; i < sites_node.size(); i++) {
            auto site_node = sites_node[i];
            SiteInfo site;
            
            site.name = site_node["name"].as<string>();
            site.id = site_node["id"] ? site_node["id"].as<int>() : i;
            site.ip = site_node["ip"].as<string>();
            site.port = site_node["port"].as<int>();
            
            sites_map[site.name] = site;
        }
        
        // Parse shard_map
        auto shard_map_node = config["shard_map"];
        nshards = shard_map_node.size();
        
        for (size_t shard_id = 0; shard_id < shard_map_node.size(); shard_id++) {
            std::vector<string> replicas;
            auto replica_list = shard_map_node[shard_id];
            
            for (size_t replica_idx = 0; replica_idx < replica_list.size(); replica_idx++) {
                string site_name = replica_list[replica_idx].as<string>();
                replicas.push_back(site_name);
                
                // Update site info
                if (sites_map.find(site_name) == sites_map.end()) {
                    Panic("Site %s in shard_map not defined in sites", site_name.c_str());
                }
                
                sites_map[site_name].shard_id = shard_id;
                sites_map[site_name].replica_idx = replica_idx;
                sites_map[site_name].is_leader = (replica_idx == 0);
            }
            
            shard_map.push_back(replicas);
        }
        
        // Parse memory ports if present
        if (config["memlocalhost"]) {
            mports[mako::LOCALHOST_CENTER_INT] = config["memlocalhost"].as<int>();
        }
        if (config["memlearner"]) {
            mports[mako::LEARNER_CENTER_INT] = config["memlearner"].as<int>();
        }
        if (config["memp1"]) {
            mports[mako::P1_CENTER_INT] = config["memp1"].as<int>();
        }
        if (config["memp2"]) {
            mports[mako::P2_CENTER_INT] = config["memp2"].as<int>();
        }
        
        Notice("Loaded %zu sites in %d shards", sites_map.size(), nshards);
    }

    ShardAddress Configuration::shard(int idx, int clusterRole) const
    {
        if (is_new_format) {
            // Convert new format to old ShardAddress
            if (idx >= 0 && idx < (int)shard_map.size()) {
                // Map clusterRole to replica index
                int replica_idx = 0;
                if (clusterRole == mako::P1_CENTER_INT) replica_idx = 1;
                else if (clusterRole == mako::P2_CENTER_INT) replica_idx = 2;
                else if (clusterRole == mako::LEARNER_CENTER_INT) replica_idx = 3;
                
                if (replica_idx < (int)shard_map[idx].size()) {
                    string site_name = shard_map[idx][replica_idx];
                    auto it = sites_map.find(site_name);
                    if (it != sites_map.end()) {
                        return ShardAddress(it->second.ip, std::to_string(it->second.port), clusterRole);
                    }
                }
            }
            Panic("Invalid shard request: idx=%d, clusterRole=%d", idx, clusterRole);
        } else {
            // Old format logic
            int i = 0;
            for (auto s: shards) {
                if (s.clusterRole == clusterRole) {
                    if (i == idx) return s;
                    i++;
                }
            }
            Panic("shards get are not matched in configuration, idx: %d, cluster: %d!", idx, clusterRole);
        }
    }

    SiteInfo* Configuration::GetSiteByName(const string& name)
    {
        if (!is_new_format) return nullptr;
        
        auto it = sites_map.find(name);
        if (it != sites_map.end()) {
            return &it->second;
        }
        return nullptr;
    }

    SiteInfo* Configuration::GetLeaderForShard(int shard_id)
    {
        if (!is_new_format || shard_id >= nshards) return nullptr;
        
        string leader_name = shard_map[shard_id][0];
        return GetSiteByName(leader_name);
    }

    std::vector<SiteInfo*> Configuration::GetReplicasForShard(int shard_id)
    {
        std::vector<SiteInfo*> replicas;
        if (!is_new_format || shard_id >= nshards) return replicas;
        
        for (const auto& site_name : shard_map[shard_id]) {
            auto site = GetSiteByName(site_name);
            if (site) replicas.push_back(site);
        }
        return replicas;
    }

    bool Configuration::IsLeader(const string& site_name)
    {
        if (!is_new_format) {
            // For old format, check if it's "localhost"
            return site_name == "localhost" || site_name == mako::LOCALHOST_CENTER;
        }
        
        auto site = GetSiteByName(site_name);
        return site && site->is_leader;
    }

    int Configuration::GetNumReplicas(int shard_id) const
    {
        if (!is_new_format) {
            // Count replicas in old format (typically 3 or 4)
            return shards.size() / nshards;
        }
        
        if (shard_id >= 0 && shard_id < (int)shard_map.size()) {
            return shard_map[shard_id].size();
        }
        return 0;
    }

    bool Configuration::operator==(const Configuration &other) const
    {
        return configFile == other.configFile;
    }

    bool Configuration::operator<(const Configuration &other) const
    {
        return configFile < other.configFile;
    }

} // namespace transport