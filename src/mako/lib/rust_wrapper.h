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
    bool rust_retrieve_request_from_queue(uint32_t* id, char** request_data);
    bool rust_put_response_back_queue(uint32_t id, const char* result, bool success);
    void rust_free_string(char* ptr);
}

class RustWrapper {
public:
    RustWrapper();
    ~RustWrapper();
    
    bool init();
    void start_polling();
    void stop();
    void notify_request_available();
    
private:
    void poll_requests();
    void execute_request(uint32_t id, const string& operation, const string& key, const string& value);
    void execute_batch_request(uint32_t id, const string& request_data);
    
    // Core storage
    KVStore kv_store_;
    
    // Control flags
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    
    // Event-driven synchronization
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    
    // Processing thread
    std::thread processing_thread_;
};

// Global pointer for Rust to notify C++
extern RustWrapper* g_rust_wrapper_instance;

// C function for Rust to call when new request is available
extern "C" void cpp_notify_request_available();

#endif