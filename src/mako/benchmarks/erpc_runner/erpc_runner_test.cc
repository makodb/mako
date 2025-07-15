#include "erpc_runner.h"
#include "configuration.h"
#include "common.h"
#include "erpc_runner_queue.h"
#include "simple_client.h"
#include "simple_server.h"

#include <iostream>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

using namespace std;

int get_key(DATA_TYPE x, DATA_TYPE y, DATA_TYPE z) {
    return 100 * (int) x + 10 * (int) y + (int) z;
}

void run_erpc_runner(
    const string &file,
    DATA_TYPE erpc_runner_id, DATA_TYPE replica_id, DATA_TYPE shard_id,
    map<int, ERPCRunner*> *erpc_runner_holder
) {
    auto *it = new ERPCRunner(file, erpc_runner_id, replica_id, shard_id);
    (*erpc_runner_holder)[get_key(erpc_runner_id, replica_id, shard_id)] = it;
    it->run();
}

void run_server(
    DATA_TYPE server_id, DATA_TYPE replica_id, DATA_TYPE shard_id,
    ERPCMessageServerQueue *message_queue,
    map<int, Server*> *server_holder
) {
    auto *it = new Server(replica_id, shard_id, server_id, message_queue);
    (*server_holder)[get_key(server_id, replica_id, shard_id)] = it;
    it->run();
}


int main() {
    string file = "benchmarks/erpc_runner/config.yml";
    Configuration config(file);

    vector<int> erpc_runner_keys;
    vector<int> server_keys;
    thread erpc_runner_thread_holder[1000];
    thread server_thread_holder[1000];
    map<int, ERPCRunner*> erpc_runner_holder;
    map<int, Client*> client_holder;
    map<int, Server*> server_holder;

    /* Initialize erpc runner */
    /* Only test one replica here */
    for (auto replica_id = 0; replica_id < 1; replica_id ++)
        for (auto shard_id = 0; shard_id < config.get_num_shards(); shard_id ++)
            for (auto erpc_runner_id = 0; erpc_runner_id < config.get_num_erpc_runner_per_shard(); erpc_runner_id ++) {
                int key = get_key(erpc_runner_id, replica_id, shard_id);
                erpc_runner_thread_holder[key] = thread(
                        run_erpc_runner,
                        file,
                        erpc_runner_id,
                        replica_id,
                        shard_id,
                        &erpc_runner_holder
                    );
                erpc_runner_keys.push_back(key);
                this_thread::sleep_for(std::chrono::seconds(1));
            }

    /* Initialize client */
    for (auto replica_id = 0; replica_id < 1; replica_id ++)
        for (auto shard_id = 0; shard_id < config.get_num_shards(); shard_id ++)
            for (auto client_id = 0; client_id < config.get_num_clients_per_shard(); client_id ++) {
                auto erpc_runner_id = config.get_erpc_runner_id_client_belongs_to(client_id);
                auto erpc_runner_key = get_key(erpc_runner_id, replica_id, shard_id);
                auto *client_queue = erpc_runner_holder[erpc_runner_key]->register_client(client_id, replica_id, shard_id);
                auto client_key = get_key(client_id, replica_id, shard_id);
                client_holder[client_key] = new Client(replica_id, shard_id, client_id, client_queue);
            }

    /* Initialize server */
    for (auto replica_id = 0; replica_id < 1; replica_id ++)
        for (auto shard_id = 0; shard_id < config.get_num_shards(); shard_id ++)
            for (auto server_id = 0; server_id < config.get_num_servers_per_shard(); server_id ++) {
                auto erpc_runner_id = config.get_erpc_runner_id_server_belongs_to(server_id);
                auto erpc_runner_key = get_key(erpc_runner_id, replica_id, shard_id);
                auto *server_queue = erpc_runner_holder[erpc_runner_key]->register_server(server_id, replica_id, shard_id);
                auto server_key = get_key(server_id, replica_id, shard_id);
                server_thread_holder[server_key] = thread(
                    run_server,
                    server_id,
                    replica_id,
                    shard_id,
                    server_queue,
                    &server_holder
                );
                server_keys.push_back(server_key);
                this_thread::sleep_for(std::chrono::seconds(1));
            }

    for (auto it: erpc_runner_holder)
        it.second->loading_finished();

    DATA_TYPE client_id = 0, shard_id = 0, replica_id = 0;
    uint target_shards_bitset = 0 | (1 << 1);
    client_holder[get_key(client_id, replica_id, shard_id)]->send_request_type_A(target_shards_bitset);

    for (auto key: erpc_runner_keys)
        erpc_runner_thread_holder[key].join();
    for (auto key: server_keys)
        server_thread_holder[key].join();
    return 0;
}


// erpc_runner:  $(O)/benchmarks/erpc_runner_test

// $(O)/benchmarks/erpc_runner_test: $(O)/benchmarks/erpc_runner/erpc_runner_test.o $(O)/benchmarks/erpc_runner/erpc_runner_queue.o $(O)/benchmarks/erpc_runner/common.o $(O)/benchmarks/erpc_runner/configuration.o $(O)/benchmarks/erpc_runner/erpc_runner.o $(OBJFILES) $(MASSTREE_OBJFILES) $(OBJS) $(BENCH_OBJFILES) third-party/lz4/liblz4.so
// 	$(CXX) -o $(O)/benchmarks/erpc_runner_test -O0 $^ $(BENCH_LDFLAGS) $(LZ4LDFLAGS) -lyaml-cpp
