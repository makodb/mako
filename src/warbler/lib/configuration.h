// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * configuration.h:
 *   shards information
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

using std::string;

namespace transport
{
    struct ShardAddress
    {
        string host;
        string port; // client port
        string cluster; // localhost, p1, p2, learner
        int clusterRole; 
        ShardAddress(const string &host, const string &port, const int &clusterRole);
        bool operator==(const ShardAddress &other) const;
        inline bool operator!=(const ShardAddress &other) const
        {
            return !(*this == other);
        }
        bool operator<(const ShardAddress &other) const;
        bool operator<=(const ShardAddress &other) const
        {
            return *this < other || *this == other;
        }
        bool operator>(const ShardAddress &other) const
        {
            return !(*this <= other);
        }
        bool operator>=(const ShardAddress &other) const
        {
            return !(*this < other);
        }
    };

    class Configuration
    {
    public:
        Configuration(std::string file);
        virtual ~Configuration();
        ShardAddress shard(int idx, int clusterRole=0) const;
        bool operator==(const Configuration &other) const;
        inline bool operator!=(const Configuration &other) const
        {
            return !(*this == other);
        }
        bool operator<(const Configuration &other) const;
        bool operator<=(const Configuration &other) const
        {
            return *this < other || *this == other;
        }
        bool operator>(const Configuration &other) const
        {
            return !(*this <= other);
        }
        bool operator>=(const Configuration &other) const
        {
            return !(*this < other);
        }

    public:
        int nshards;            // number of shards
        int warehouses;         // number of warehouses per shard
        std::string configFile; // yaml file
        std::unordered_map<int,int> mports;
    private:
        std::vector<ShardAddress> shards;
    };
} // namespace transport

#endif /* _LIB_CONFIGURATION_H_ */
