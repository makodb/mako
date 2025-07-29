#ifndef _LIB_RUST_WRAPPER_H_
#define _LIB_RUST_WRAPPER_H_

#include <iostream>
#include <string>
#include <map>
#include <thread>
#include <chrono>
#include <atomic>

using namespace std;

// C interface for Rust functions
// All code are implemented in ./rust-lib
extern "C" {
    bool rust_init();
    bool rust_retrieve_request_from_queue(uint32_t* id, char** operation, char** key, char** value);
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
    
private:
    void poll_requests();
    void execute_request(uint32_t id, const string& operation, const string& key, const string& value);
    
    // Core storage: we will remove it later
    std::map<std::string, std::string> store_;
    
    // Control flags
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    
    // Polling thread
    std::thread polling_thread_;
};

#endif