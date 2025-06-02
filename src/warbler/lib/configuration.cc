// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * configuration.cc:
 *   Representation of a replica group configuration, i.e. the number
 *   and list of replicas in the group
 *
 *
 **********************************************************************/

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/common.h"

#include <cstring>
#include <stdexcept>
#include <tuple>
#include <yaml-cpp/yaml.h>

namespace transport
{
    ShardAddress::ShardAddress(const string &host, const string &port, const int &clusterRole)
        : host(host), port(port), clusterRole(clusterRole) {}

    bool
    ShardAddress::operator==(const ShardAddress &other) const
    {
        return ((host == other.host) &&
                (port == other.port)&&
                (clusterRole == other.clusterRole));
    }

    bool
    ShardAddress::operator<(const ShardAddress &other) const
    {
        auto this_t = std::forward_as_tuple(host, port, clusterRole);
        auto other_t = std::forward_as_tuple(other.host, other.port, other.clusterRole);
        return this_t < other_t;
    }

    Configuration::Configuration(std::string file)
    {
        /**
         * weihshen: rewrite configuration with YAML
         * What we need:
         *   1. number of shards and warehouses
         *   2. shards address
         **/
        YAML::Node config = YAML::LoadFile(file);
        int num_shards = config["shards"].as<int>(); // num of shards
        warehouses = config["warehouses"].as<int>(); // num of warehouses
        nshards = num_shards;
        for (auto cluster: {srolis::LOCALHOST_CENTER,
                            srolis::P1_CENTER,
                            srolis::P2_CENTER,
                            srolis::LEARNER_CENTER}) {
            auto node = config[cluster];
            if (!node) continue;
            int cnt=0;
            for (int i = 0; i < node.size(); ++i)
            {
                auto item = node[i];
                auto ip = item["ip"].as<string>();
                auto port = item["port"].as<int>();
                shards.push_back(ShardAddress(ip, std::to_string(port), srolis::convertCluster(cluster)));
                cnt++;
            }
            if (cnt != nshards)
            {
                Panic("shards are not matched in configuration, got: %d, required: %d!", cnt, nshards);
            }
        }

        mports[srolis::LOCALHOST_CENTER_INT]=config["memlocalhost"].as<int>();
        mports[srolis::LEARNER_CENTER_INT]=config["memlearner"].as<int>();
        mports[srolis::P1_CENTER_INT]=config["memp1"].as<int>();
        mports[srolis::P2_CENTER_INT]=config["memp2"].as<int>();

        configFile = file;
    }

    Configuration::~Configuration()
    {
        Notice("[Configuration]Deconstruct Configuration");
    }

    ShardAddress
    Configuration::shard(int idx, int clusterRole) const
    {
        int i=0;
        for (auto s: shards) {
            if (s.clusterRole == clusterRole) {
                if (i==idx) {return s;}
                i++;
            }
        }
        Panic("shards get are not matched in configuration, idx: %d, cluster: %d!", idx, clusterRole);
    }

} // namespace transport
