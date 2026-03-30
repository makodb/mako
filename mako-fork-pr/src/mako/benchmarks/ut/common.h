#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include "benchmarks/bench.h"
#include "benchmarks/mbta_wrapper.hh"
#include "benchmarks/tpcc.h"
using namespace std;

typedef std::pair<std::string, std::string> kv_pair;

class new_scan_callback_bench : public abstract_ordered_index::scan_callback {
public:
    new_scan_callback_bench() {} ;
    virtual bool invoke(
            const char *keyp, size_t keylen,
            const string &value)
    {
        values.emplace_back(std::string(keyp, keylen), value);
        return true;
    }
    
    std::vector<kv_pair> values;
};

std::vector<kv_pair> scan_tables(abstract_db *db, abstract_ordered_index* table) {
    str_arena arena;
    void *buf = NULL ;
    char WS = static_cast<char>(0);
    std::string startKey(1, WS);
    char WE = static_cast<char>(255);
    std::string endKey(1, WE);
    new_scan_callback_bench calloc;
    void *txn0 = db->new_txn(0, arena, buf, abstract_db::HINT_DEFAULT);
    table->scan(txn0, startKey, &endKey, calloc) ;
    db->commit_txn(txn0);
    return calloc.values;
}