// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * configuration_unified.h:
 *   Unified configuration supporting both old and new formats
 *
 **********************************************************************/

#ifndef _LIB_CONFIGURATION_H_
#define _LIB_CONFIGURATION_H_

#include <iostream>
#include <fstream>
#include <stdbool.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <unordered_map>
#include <map>
#include "common.h"

using std::string;

namespace transport
{
    struct ShardAddress
    {
        string host;
        string port;
        string cluster;
        int clusterRole;
        
        ShardAddress() : clusterRole(0) {}
        ShardAddress(const string &host, const string &port, const int &clusterRole);
        bool operator==(const ShardAddress &other) const;
        inline bool operator!=(const ShardAddress &other) const { return !(*this == other); }
        bool operator<(const ShardAddress &other) const;
        bool operator<=(const ShardAddress &other) const { return *this < other || *this == other; }
        bool operator>(const ShardAddress &other) const { return !(*this <= other); }
        bool operator>=(const ShardAddress &other) const { return !(*this < other); }
    };

    // Site information for new format
    struct SiteInfo
    {
        string name;
        int id;
        string ip;
        int port;
        bool is_leader;
        int shard_id;
        int replica_idx;
        
        SiteInfo() : id(-1), port(0), is_leader(false), shard_id(-1), replica_idx(-1) {}
    };

    class Configuration
    {
    public:
        Configuration(std::string file);
        virtual ~Configuration();
        
        // Old interface (backward compatibility)
        ShardAddress shard(int idx, int clusterRole=0) const;
        
        // New interface
        SiteInfo* GetSiteByName(const string& name);
        SiteInfo* GetLeaderForShard(int shard_id);
        std::vector<SiteInfo*> GetReplicasForShard(int shard_id);
        bool IsLeader(const string& site_name);
        int GetNumReplicas(int shard_id) const;
        
        // Common interface
        bool operator==(const Configuration &other) const;
        inline bool operator!=(const Configuration &other) const { return !(*this == other); }
        bool operator<(const Configuration &other) const;
        bool operator<=(const Configuration &other) const { return *this < other || *this == other; }
        bool operator>(const Configuration &other) const { return !(*this <= other); }
        bool operator>=(const Configuration &other) const { return !(*this < other); }

    public:
        int nshards;            // number of shards
        int warehouses;         // number of warehouses per shard
        std::string configFile; // yaml file
        std::unordered_map<int,int> mports;

        // New format support
        bool is_new_format;
        std::map<string, SiteInfo> sites_map;
        std::vector<std::vector<string>> shard_map;

        // Multi-shard support: which shards run in this process
        bool multi_shard_mode;                  // true if running multiple shards in one process
        std::vector<int> local_shard_indices;   // list of shard indices to run locally (e.g., [0,1,2])

        
    private:
        std::vector<ShardAddress> shards;  // Old format
        
        void ParseOldFormat(YAML::Node& config);
        void ParseNewFormat(YAML::Node& config);
        bool DetectFormat(YAML::Node& config);
    };
} // namespace transport

#endif /* _LIB_CONFIGURATION_H_ */