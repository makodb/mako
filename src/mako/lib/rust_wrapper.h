#ifndef _LIB_RUST_WRAPPER_H_
#define _LIB_RUST_WRAPPER_H_

#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <vector>
#include <cstdint>
#include "benchmarks/mbta_wrapper.hh"
#include <examples/common.h>
#include "benchmarks/abstract_db.h"
#include "benchmarks/abstract_ordered_index.h"
#include "benchmarks/sto/StringWrapper.hh"

class abstract_db;
class abstract_ordered_index;
class str_arena;

extern "C" {
    bool rust_init(size_t new_max);
    void rust_free_string(char* ptr);
}

enum class OpCode : uint32_t {
    Invalid = 0,
    Get     = 1,
    Set     = 2,
    Del     = 3,
    Exists  = 4,
    Append  = 5,
    StrLen  = 6,
    IncrBy  = 7,
    IncrByFloat = 8,
    Expire = 9,
    Ttl = 10,
    Persist = 11,
    Scan = 12,
    SAdd = 13,
    SRem = 14,
    SIsMember = 15,
    SCard = 16,
    SMembers = 17,
    SPop = 18,
    SRandMember = 19,
    SMove = 20,
    SetAlgebra = 21,
    Type = 22,
    LPush = 23,
    RPush = 24,
    LPop = 25,
    RPop = 26,
    LLen = 27,
    LIndex = 28,
    LRange = 29,
    LSet = 30,
    LRem = 31,
    LTrim = 32,
    LInsert = 33,
    LMove = 34,
    LPos = 35,
    ZAdd = 36,
    ZScore = 37,
    ZRem = 38,
    ZCard = 39,
    ZRange = 40,
    ZRank = 41,
    ZPopMin = 42,
    ZCount = 43,
    ZScan = 44
};

class RustWrapper {
public:
    struct Result {
        std::string value;
        bool success;
        
        Result(const std::string& val, bool succ) : value(val), success(succ) {}
        Result(bool succ) : value(""), success(succ) {}
    };

    RustWrapper();
    ~RustWrapper();
    bool init();
    Result execute_request(OpCode op,
                           const uint8_t* key_ptr, size_t key_len,
                           const uint8_t* val_ptr, size_t val_len);

    abstract_ordered_index *customerTable;
    abstract_db *db;
    static void cleanup_thread_info();
    void ensure_thread_info();  // Called by cpp_worker_thread_init

private:
    static thread_local str_arena* tl_arena;
    static thread_local std::string tl_txn_obj_buf;
    static thread_local bool tl_initialized;
    void *txn_buf();
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    static thread_local bool ti_initialized;
    static thread_local std::string tl_key_buf;
    static thread_local std::string tl_val_buf;
};

extern RustWrapper* g_rust_wrapper_instance;

extern "C" {
    // Called by Rust when each worker thread starts
    void cpp_worker_thread_init(size_t thread_id);

    bool cpp_execute_request_sync(uint32_t op,
                             const uint8_t* key_ptr, size_t key_len,
                             const uint8_t* val_ptr, size_t val_len,
                             uint8_t** out_ptr, size_t* out_len);
    void cpp_free_buf(uint8_t* ptr, size_t len);
    void cpp_cleanup_thread_info();
}

#endif
