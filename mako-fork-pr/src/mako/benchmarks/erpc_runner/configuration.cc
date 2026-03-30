#include "configuration.h"


using namespace std;


// Configuration::Configuration() {}

Configuration::Configuration(const string &file) {
    YAML::Node config = YAML::LoadFile(file);

    this->num_shards = config["num_shards"].as<int>();
    this->num_replicas = config["num_replicas"].as<int>();
    this->num_warehouses = config["num_warehouses"].as<int>();
    this->num_erpc_runner_uri = config["num_erpc_runner_uri"].as<int>();
    this->num_erpc_runner_per_shard = config["num_erpc_runner_per_shard"].as<int>();

    for (auto i = 0; i < this->num_erpc_runner_uri; i++) {
        auto uri = config["erpc_runner_uri"][i].as<std::string>();
        this->erpc_runner_uri.push_back(uri);
    }

    for (auto i = 0; i < this->num_erpc_runner_per_shard; i++) {
        DATA_TYPE from = config["client_id_range"][i][0].as<int>();
        DATA_TYPE to = config["client_id_range"][i][1].as<int>();

        for (DATA_TYPE j = from; j < to; j++)
            this->client_belongs_to[j] = i;
        
        from = config["server_id_range"][i][0].as<int>();
        to = config["server_id_range"][i][1].as<int>();

        for (DATA_TYPE j = from; j < to; j++)
            this->server_belongs_to[j] = i;
    }

    /* complete it later */
    // this->safe_check();
}

void Configuration::safe_check() {
    assert(num_erpc_runner_uri == num_shards * num_replicas * num_erpc_runner_per_shard);
    assert(this->client_belongs_to.size() == num_warehouses);
    assert(this->server_belongs_to.size() == num_warehouses * (num_shards - 1));

    // for (int i = 0; i < num_warehouses; i++) {
    //     assert(this->client_belongs_to.find(i) != this.client_belongs_to.end());
    // }

    // for (int i = 0; i < num_warehouses * (num_shards - 1); i++) {
    //     assert(this->server_belongs_to.find(i) != this.server_belongs_to.end());
    // }
}

DATA_TYPE Configuration::get_num_shards() {
    return this->num_shards;
}

DATA_TYPE Configuration::get_num_replicas() {
    return this->num_replicas;
}

DATA_TYPE Configuration::get_num_warehouses() {
    return this->num_warehouses;
}

DATA_TYPE Configuration::get_num_erpc_runner_per_shard() {
    return this->num_erpc_runner_per_shard;
}

string Configuration::get_erpc_runner_uri(DATA_TYPE shard_id, DATA_TYPE replica_id, DATA_TYPE erpc_runner_id) {
    auto idx = replica_id * (num_shards * num_erpc_runner_per_shard);
    idx += shard_id * num_erpc_runner_per_shard;
    idx += erpc_runner_id;
    return erpc_runner_uri[idx];
}

DATA_TYPE Configuration::get_erpc_runner_id_server_belongs_to(DATA_TYPE server_id) {
    return this->server_belongs_to[server_id];
}

DATA_TYPE Configuration::get_erpc_runner_id_client_belongs_to(DATA_TYPE client_id) {
    return this->client_belongs_to[client_id];
}

DATA_TYPE Configuration::get_target_server_id(DATA_TYPE sender_shard_id, DATA_TYPE sender_client_id, DATA_TYPE target_shard_id) {
    // assert(sender_shard_id != target_shard_id);
    return sender_shard_id * num_warehouses + sender_client_id - (sender_shard_id < target_shard_id ? 0 : num_warehouses);
}

DATA_TYPE Configuration::get_num_clients_per_shard() {
    return this->num_warehouses;
}

DATA_TYPE Configuration::get_num_servers_per_shard() {
    return this->num_warehouses * (this->num_shards - 1);
}