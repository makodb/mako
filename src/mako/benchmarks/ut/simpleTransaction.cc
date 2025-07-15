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
#include "benchmarks/common3.h"
#include "benchmarks/sto/sync_util.hh"
using namespace std;

INIT_SYNC_UTIL_VARS

bool cmpFunc2_v3(const std::string& newValue,const std::string& oldValue)
{
    return true;
}

class simple_tpcc_worker {
public:
    simple_tpcc_worker(abstract_db *db) : db(db) {
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
    }

    void txn_basic() {
        static abstract_ordered_index *customerTable = simple_tpcc_worker::OpenTablesForTablespace(db, "customer_0") ;
        std::this_thread::sleep_for (std::chrono::seconds (1));

        // write 5 keys:
        for (size_t i=0; i < 5; i++) {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            std::string key = "key_XXXXXXXXXXXXX_" + std::to_string(i);
            std::string value = "value_XXXXXXXXXXXXX_" + std::to_string(i)+std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');
            try {
                customerTable->put(txn, key, StringWrapper(value));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort key=" << key << std::endl;
                db->abort_txn(txn);
            }
        }

        // read 5 keys
        for (size_t i=0; i < 5; i++) {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            std::string key = "key_XXXXXXXXXXXXX_" + std::to_string(i);
            std::string value = "";
            try {
                customerTable->get(txn, key, value);
                db->commit_txn(txn);
                ASSERT_EQ(value.substr(0,("value_XXXXXXXXXXXXX_" + std::to_string(i)).length()), "value_XXXXXXXXXXXXX_" + std::to_string(i));
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort (read) key=" << key << std::endl;
                db->abort_txn(txn);
            }
        }

        auto calloc = scan_tables(db, customerTable);
        for (int i=0; i<5; i++) {
            std::cout << "scan: " << calloc[i].second << ", trim: " << calloc[i].second.substr(0,("value_XXXXXXXXXXXXX_" + std::to_string(i)).length()) << std::endl;
            ASSERT_EQ(calloc[i].second.substr(0,("value_XXXXXXXXXXXXX_" + std::to_string(i)).length()), "value_XXXXXXXXXXXXX_" + std::to_string(i));
            ASSERT_EQ(calloc[i].first, "key_XXXXXXXXXXXXX_" + std::to_string(i));
        }
    }

    void txn_scan() {
        static abstract_ordered_index *customerTable = simple_tpcc_worker::OpenTablesForTablespace(db, "customer_0") ;  // shared Masstree instance
        std::this_thread::sleep_for (std::chrono::seconds (1));
        {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            scoped_str_arena s_arena(arena);
            std::string key = "XXXXXXXXXXXX";
            std::string value = "2000000000000000" + std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');
            try {
                customerTable->put(txn, key, StringWrapper(value));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort key=" << key << std::endl;
                db->abort_txn(txn);
            }
        }

        {
            void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            scoped_str_arena s_arena(arena);
            auto tmp = s_arena.get();
            std::string key = "XXXXXXXXXXXX";
            std::string value = "1000000000000000" + std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');
            try {
                customerTable->put(txn, key, StringWrapper(value));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort key=" << key << std::endl;
                db->abort_txn(txn);
            }
        }

        {
            auto calloc = scan_tables(db, customerTable);
            ASSERT_EQ(calloc[0].second.substr(0,std::string("1000000000000000").length()), "1000000000000000");
        }

        {
            TThread::set_mode(1);
            TThread::enable_multiverison();
            scoped_str_arena s_arena(arena);
            static_limit_callback<512> c(s_arena.get(), true);
            char WS = static_cast<char>(0);
            std::string startKey(1, WS);
            char WE = static_cast<char>(255);
            std::string endKey(1, WE);
            customerTable->shard_scan(startKey, &endKey, c, s_arena.get());
            ASSERT(c.size() == 1);
            ASSERT_EQ((*c.values[0].second).substr(0,std::string("1000000000000000").length()), "1000000000000000");
        }
    }

    // mimic a behavior of participant
    void txn_participant() {
        scoped_str_arena s_arena(arena);

        // 0. load phase, load several (K,V) into table
        static abstract_ordered_index *customerTable = simple_tpcc_worker::OpenTablesForTablespace(db, "customer_0") ;  // shared Masstree instance
        std::string key = "XXXXXXXXXXXX1";
        std::string value = "10000000XXX"+std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');;
        std::string key1 = "XXXXXXXXXXXX2";
        std::string value1 = "20000000XXX"+std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');;
        std::string key2 = "XXXXXXXXXXXXXXX3";
        std::string value2 = "30000000XXX"+std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');;
        {
            void *txn = db->new_txn(0, arena, txn_buf());
            scoped_str_arena s_arena(arena);
            try {
                customerTable->insert(txn, key, StringWrapper(value));
                customerTable->insert(txn, key1, StringWrapper(value1));
                customerTable->insert(txn, key2, StringWrapper(value2));
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort key=" << key << std::endl;
                db->abort_txn(txn);
            }
        }

        // 1. worker phase
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
        db->shard_reset(); // initialize
        TThread::set_mode(1);
        TThread::enable_multiverison();
        {
            std::string needV = "";
            customerTable->shard_get(key, needV);
            //TThread::txn->print_stats();
            std::string needV2 = "";
            value = "30000XXXXXX" +std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');;
            customerTable->shard_put(key, value);
            auto valid=db->shard_validate();
            ASSERT_EQ(valid, 0);
            db->shard_install();
            db->shard_unlock(true);
        }

        {
            auto calloc = scan_tables(db, customerTable);
            ASSERT_EQ(calloc[0].second.substr(0,std::string("30000XXXXXX").length()), "30000XXXXXX");
            ASSERT_EQ(calloc.size(), 3);
        }
    }

    void parse_str() {
        stock::key k0(111, 1112);
        stock::value v0;
        v0.s_order_cnt=11111111;
        v0.s_quantity=2222;
        v0.s_remote_cnt=3333333;
        v0.s_ytd=2323.13;
        std::string sv;
        Encode(sv, v0);
        stock::value v1;
        Decode(sv, v1);
        ASSERT_EQ(v1.s_order_cnt, 11111111);
        ASSERT_EQ(v1.s_quantity, 2222);
        ASSERT_EQ(v1.s_remote_cnt, 3333333);

        static const string zeros(16, 0);
        customer_name_idx::key cv0;
        cv0.c_d_id=11;
        cv0.c_last.assign(zeros);
        std::string csv;
        Encode(csv, cv0);
        //ASSERT_EQ(csv.length(), 40); // 4 + 4 + 16 + 16
    }

    void txn_replay() {
       static abstract_ordered_index *customerTable = simple_tpcc_worker::OpenTablesForTablespace(db, "customer_0") ;
       std::this_thread::sleep_for (std::chrono::seconds (1));

       // write 5 keys:
       for (size_t i=0; i < 5; i++) {
           void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
           std::string key = "key_XXXXXXXXXXXXX_" + std::to_string(i);
           std::string value = "value_XXXXXXXXXXXXX_" + std::to_string(i)+std::string(srolis::EXTRA_BITS_FOR_VALUE,'B');
           try {
               customerTable->put_mbta(txn, key, cmpFunc2_v3, value);
               db->commit_txn(txn);
           } catch (abstract_db::abstract_abort_exception &ex) {
               std::cout << "abort key=" << key << std::endl;
               db->abort_txn(txn);
           }
       }

       // read 5 keys
       for (size_t i=0; i < 5; i++) {
           void *txn = db->new_txn(0, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
           std::string key = "key_XXXXXXXXXXXXX_" + std::to_string(i);
           std::string value = "";
           try {
               customerTable->get(txn, key, value);
               db->commit_txn(txn);
               ASSERT_EQ(value.substr(0,("value_XXXXXXXXXXXXX_" + std::to_string(i)).length()), "value_XXXXXXXXXXXXX_" + std::to_string(i));
           } catch (abstract_db::abstract_abort_exception &ex) {
               std::cout << "abort (read) key=" << key << std::endl;
               db->abort_txn(txn);
           }
       }
    }

    void init() {
        scoped_db_thread_ctx ctx(db, false);
        mbta_ordered_index::mbta_type::thread_init();
    }
    static abstract_ordered_index * OpenTablesForTablespace(abstract_db *db, const char *name) {
       return db->open_index(name, 1, false, false); // create a table instance: mbta_ordered_index
    }
protected:
    abstract_db *const db;
    str_arena arena;
    std::string txn_obj_buf;
    inline void *txn_buf() { return (void *) txn_obj_buf.data(); }
};

void runner(abstract_db *db) {
    auto worker = new simple_tpcc_worker(db) ;
    worker->init();
    worker->txn_basic();
    // worker->parse_str();
    // worker->txn_scan();
    // worker->txn_participant();
    // worker->txn_replay();
}

int main() {
    abstract_db *db = new mbta_wrapper;
    config = new transport::Configuration("./config/local-shards2-warehouses1.yml");
    shardIndex = 0;

    runner(db);
    return 0;
}