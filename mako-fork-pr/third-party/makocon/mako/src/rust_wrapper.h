#ifndef _LIB_RUST_WRAPPER_H_
#define _LIB_RUST_WRAPPER_H_

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "kv_store.h"

using namespace std;

// C interface for Rust functions
extern "C" {
    bool rust_init();
    void rust_free_string(char* ptr);
}

class RustWrapper {
public:
    RustWrapper();
    ~RustWrapper();
    
    KVStore kv_store_;

    bool init();
    
private:
    // void execute_request(uint32_t id, const string& operation, const string& key, const string& value);
    // void execute_batch_request(uint32_t id, const string& request_data);
    
    // Core storage
    
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