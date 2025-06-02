//
// Created by weihshen on 2/3/21.
//

#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include "benchmarks/bench.h"
#include "benchmarks/mbta_wrapper.hh"
#include "benchmarks/tpcc.h"
#include "common.h"
#include "lib/server.h"
#include "lib/shardClient.h"
#include "deptran/s_main.h"
#include "benchmarks/sto/Transaction.hh"
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/common3.h"
using namespace std;
using namespace srolis;

INIT_SYNC_UTIL_VARS

static FastTransport *server_transport;
int par_id=0;
int client_shardIndex=0;
int server_shardIndex=1;
int num_warehouses=1;


class simple_tpcc_worker {
public:
    simple_tpcc_worker(abstract_db *db) : db(db) {
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
    }

    void init() {
        scoped_db_thread_ctx ctx(db, true);
        mbta_ordered_index::mbta_type::thread_init();
        abstract_ordered_index *checking = simple_tpcc_worker::OpenTablesForTablespace(db, "checking") ;
        abstract_ordered_index *saving = simple_tpcc_worker::OpenTablesForTablespace(db, "saving") ;
        open_tables["checking"] = checking;
        open_tables["saving"] = saving;
        for (int i=0; i<2; i++) {
            abstract_ordered_index *table = simple_tpcc_worker::OpenTablesForTablespace(db, ("dummy_"+std::to_string(i)).c_str()) ;
            open_tables["dummy_"+std::to_string(i)] = table;
        }
    }

    void load() {
        for (int i=0; i<10; i++) {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_BASIC);
            std::string key = "key_checking_" + std::to_string(i);
            std::string value = std::to_string(100);
            open_tables["checking"]->put(txn, key, StringWrapper(value));
            db->commit_txn(txn);
        }

        for (int i=0; i<10; i++) {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_BASIC);
            std::string key = "key_saving_" + std::to_string(i);
            std::string value = std::to_string(100);
            open_tables["saving"]->put(txn, key, StringWrapper(value));
            db->commit_txn(txn);
        }
    }

    static abstract_ordered_index * OpenTablesForTablespace(abstract_db *db, const char *name) {
        auto ret = db->open_index(name, 1, false, false); // create a table instance: mbta_ordered_index
        std::cout << "table-name: " << name << ", table_id: " << ret->get_table_id() << std::endl;
        return ret;
    }

    void client() {
        std::string obj_v;
        TThread::sclient = new srolis::ShardClient(config->configFile,
                                                 "localhost",
                                                 client_shardIndex,
                                                 par_id,
                                                 workload_type);

        for (size_t i=0; i<5; i++) {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            std::string key = "key_checking_" + std::to_string(i);
            std::string value = "";
            open_tables["checking"]->get(txn, key, value);
            value = std::to_string(atoi(value.c_str()) + 10);
            open_tables["checking"]->put(txn, key, StringWrapper(value));
            open_tables["dummy_0"]->get(txn, key, obj_v);  // the checking on the remote shard
            obj_v = std::to_string(atoi(obj_v.c_str()) - 10);
            open_tables["dummy_0"]->put(txn, key, obj_v);
            db->commit_txn(txn);
        }

        auto calloc = scan_tables(db, open_tables["checking"]);
        for (int i=0; i<5; i++)
            ASSERT_EQ(calloc[i].second, "110");
    }

    void static helper_server(
                          transport::Configuration *config,
                          abstract_db *db,
                          srolis::HelperQueue *queue,
                          srolis::HelperQueue *queue_response,
                          const map<string, abstract_ordered_index *> &open_tables) {
        scoped_db_thread_ctx ctx(db, true);  // invoke thread_init
        TThread::set_id(2);
        TThread::set_mode(1);
        TThread::enable_multiverison();
        TThread::set_nshards(config->nshards);
        std::string file=config->configFile;
        TThread::set_shard_index(server_shardIndex);
        map<string, vector<abstract_ordered_index *>> partitions;
        map<string, vector<abstract_ordered_index *>> dummy_partitions;
        srolis::ShardServer *ss=new srolis::ShardServer(config->configFile,
                                                        client_shardIndex, 
                                                        server_shardIndex, par_id);
        ss->Register(db, queue, queue_response, open_tables, partitions, dummy_partitions);
        ss->Run();  // it is event driven!
    }

    void setup_helper() {
        auto t=std::thread(helper_server,
                           config, db,
                           queue_holders[0],
                           queue_holders_response[0],
                           open_tables);
        t.detach();
    }

    void static erpc_server(std::string cluster,
                          transport::Configuration *config) {
        std::string local_uri = config->shard(server_shardIndex, srolis::convertCluster(cluster)).host;
        int id = num_warehouses;
        server_transport = new FastTransport(config->configFile,
                                    local_uri,  // local_uri
                                    cluster,
                                    1, 11,
                                    1,          // physPort
                                    0,          // numa node
                                    server_shardIndex, // used to get basic port
                                    id);
        auto *it = new srolis::HelperQueue();
        server_transport->c->queue_holders[0] = it;
        auto *it_res = new srolis::HelperQueue();
        server_transport->c->queue_holders_response[0] = it_res;
        server_transport->Run(); 
    }

    void setup_erpc_server() {
        auto t=std::thread(erpc_server, cluster, config);
        sleep(1);
        queue_holders[0] = server_transport->c->queue_holders[0];
        queue_holders_response[0] = server_transport->c->queue_holders_response[0];
        t.detach();
    }

    void validate() {
        sleep(3);
        auto calloc = scan_tables(db, open_tables["checking"]);
        for (int i=0; i<5; i++)
            ASSERT_EQ(calloc[i].second, "90"); 
    }

    void print_stats() {
        auto calloc = scan_tables(db, open_tables["checking"]);
        std::cout << "checking:" << std::endl;
        for (int i=0; i<calloc.size();i++) 
            std::cout << "  k: " << calloc[i].first << ", value: " << calloc[i].second << std::endl;

        std::cout << "saving:" << std::endl;
        calloc = scan_tables(db, open_tables["saving"]);
        for (int i=0; i<calloc.size();i++) 
            std::cout << "  k: " << calloc[i].first << ", value: " << calloc[i].second << std::endl;
    }

protected:
    abstract_db *const db;
    std::map<std::string, abstract_ordered_index *> open_tables;
    str_arena arena;
    std::string txn_obj_buf;
    inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

    std::unordered_map<uint8_t, srolis::HelperQueue*> queue_holders;
    std::unordered_map<uint8_t, srolis::HelperQueue*> queue_holders_response;
};

int main(int argv, char **args) {
    static thread_local std::shared_ptr<StringAllocator> instance = std::shared_ptr<StringAllocator>(
            new StringAllocator(2, 50331648, 1000));
    size_t w=0;
    instance->getLogOnly(w);
    std::cout << "the w is: " << w << std::endl;

    // NOT use it
    shardIndex = 0;

    int is_client=atoi(args[1]);
    abstract_db *db = new mbta_wrapper;
    config = new transport::Configuration("./config/local-shards2-warehouses1.yml");
    workload_type = 0;
    cluster="localhost";
    auto worker = new simple_tpcc_worker(db) ;
    worker->init();
    worker->load();

    if (is_client) {
        worker->client();
        //worker->print_stats();
    } else {
        worker->setup_erpc_server();
        worker->setup_helper();
        worker->validate();
        //worker->print_stats();
    }
    return 0;
}