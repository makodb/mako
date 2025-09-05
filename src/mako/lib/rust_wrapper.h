#ifndef _LIB_RUST_WRAPPER_H_
#define _LIB_RUST_WRAPPER_H_

#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include <mutex>
#include "benchmarks/bench.h"
#include "common.h"

using namespace std;

class abstract_db;
class abstract_ordered_index;
class str_arena;
class StringWrapper;

// C interface for Rust functions
extern "C" {
    bool rust_init();
    void rust_free_string(char* ptr);
}

class RustWrapper {
public:
    RustWrapper();
    ~RustWrapper();
    
    struct Result {
        std::string value;
        bool success;
        
        Result(const std::string& val, bool succ) : value(val), success(succ) {}
        Result(bool succ) : value(""), success(succ) {}
    };

    bool init();
    RustWrapper::Result execute_request(const string& operation, const string& key, const string& value);

    abstract_ordered_index *customerTable;
    abstract_db *db;
    
private:
    // Core storage: we will remove it later
    // std::map<std::string, std::string> store_;

    str_arena *arena;
    std::string txn_obj_buf;
    inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

    // Control flags
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
};


// Global pointer for Rust to notify C++
extern RustWrapper* g_rust_wrapper_instance;

// C function for Rust to call when new request is available
extern "C" {
    bool cpp_execute_request_sync(const char* operation, const char* key, const char* value, char** result);
    void cpp_free_string(char* ptr);
}

#endif