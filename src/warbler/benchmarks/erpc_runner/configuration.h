#pragma once

#include <vector>
#include <assert.h>
#include <algorithm>
#include <map>
#include <yaml-cpp/yaml.h>
#include "common.h"


class Configuration {
public:
    Configuration() {}

    Configuration(const std::string &file);

    void safe_check();

    DATA_TYPE get_num_shards();

    DATA_TYPE get_num_replicas();

    DATA_TYPE get_num_warehouses();

    DATA_TYPE get_num_erpc_runner_per_shard();

    DATA_TYPE get_num_clients_per_shard();

    DATA_TYPE get_num_servers_per_shard();

    std::string get_erpc_runner_uri(DATA_TYPE shard_id, DATA_TYPE replica_id, DATA_TYPE erpc_runner_id);

    DATA_TYPE get_erpc_runner_id_server_belongs_to(DATA_TYPE server_id);

    DATA_TYPE get_erpc_runner_id_client_belongs_to(DATA_TYPE client_id);

    DATA_TYPE get_target_server_id(DATA_TYPE sender_shard_id, DATA_TYPE sender_client_id, DATA_TYPE target_shard_id);
private:
    DATA_TYPE num_shards;
    DATA_TYPE num_replicas;
    DATA_TYPE num_warehouses;
    DATA_TYPE num_erpc_runner_uri;
    DATA_TYPE num_erpc_runner_per_shard;

    std::vector<std::string> erpc_runner_uri;
    std::map<DATA_TYPE, DATA_TYPE> client_belongs_to;
    std::map<DATA_TYPE, DATA_TYPE> server_belongs_to;
};
