//
// Simple Transaction Tests for MakoCon Server
//

#include <iostream>
#include <chrono>
#include <thread>
#include <mako.hh>
#include <examples/common.h>
#include "lib/rust_wrapper.h"

RustWrapper* g_rust_wrapper_instance = nullptr;
thread_local bool RustWrapper::ti_initialized = false;

// RustWrapper implementations
RustWrapper::RustWrapper() : running_(false), initialized_(false) {
    g_rust_wrapper_instance = this;
    txn_obj_buf.reserve(str_arena::MinStrReserveLength);
}

RustWrapper::~RustWrapper() {
    if (running_) running_ = false;
    if (ti_initialized) {
        ti_initialized = false;
    }
    g_rust_wrapper_instance = nullptr;
    delete arena;
}

void* RustWrapper::txn_buf() { 
    return (void*)txn_obj_buf.data(); 
}

bool RustWrapper::init() {
    if (initialized_) {
        return false;
    }
    
    arena = new str_arena();
    txn_obj_buf.resize(db->sizeof_txn_object(0));

    if (!rust_init()) {
        std::cerr << "Failed to initialize Rust socket listener" << std::endl;
        return false;
    }
    
    running_ = true;
    initialized_ = true;
    
    std::cout << "RustWrapper initialized successfully" << std::endl;
    return true;
}

void RustWrapper::ensure_thread_info() {
    if (!ti_initialized) {
        std::cout << "DEBUG: About to call mbta_type::thread_init()" << std::endl;
        mbta_ordered_index::mbta_type::thread_init();
        ti_initialized = true;
        std::cout << "DEBUG: Initialized thread via mbta_type::thread_init() for thread " 
                  << std::this_thread::get_id() << std::endl;
    }
}

RustWrapper::Result RustWrapper::execute_request(const std::string& operation, const std::string& key, const std::string& value) {
    ensure_thread_info();
    
    std::cout << "DEBUG: Thread " << std::this_thread::get_id() 
              << " executing " << operation << std::endl;
    
    std::string result;
    bool success = true;
    
    try {
        if (operation == "get") {
            void *txn = db->new_txn(0, *arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            std::string table_key = "table_key_" + key;
            std::string table_value = "";
            try {
                customerTable->get(txn, table_key, table_value);
                db->commit_txn(txn);
                result = table_value;
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort (read) key=" << table_key << std::endl;
                db->abort_txn(txn);
            } catch (...) {
                db->abort_txn(txn);
                success = false;
                result = "ERROR: Exception";
            }
        } else if (operation == "set") {
            void *txn = db->new_txn(0, *arena, txn_buf());
            std::string table_key = "table_key_" + key;
            std::string table_value = "table_value_" + value + 
                                std::string(mako::EXTRA_BITS_FOR_VALUE, 'B');
            try {
                customerTable->put(txn, table_key, StringWrapper(table_value));
                db->commit_txn(txn);
                result = "OK";
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Write aborted: %s\n", table_key.c_str());
                db->abort_txn(txn);
                success = false;
                result = "ERROR: Transaction aborted";
            } catch (...) {
                db->abort_txn(txn);
                success = false;
                result = "ERROR: Exception";
            }
        } else {
            result = "ERROR: Invalid operation";
            success = false;
        }
    } catch (...) {
        success = false;
        result = "ERROR: Unexpected exception";
    }
    return Result(result, success);
}

void RustWrapper::cleanup_thread_info() {
    if (ti_initialized) {
        ti_initialized = false;
    }
}

extern "C" {
    bool cpp_execute_request_sync(const char* operation, const char* key, const char* value, char** result) {
        if (!g_rust_wrapper_instance) {
            *result = nullptr;
            return false;
        }
        
        try {
            RustWrapper::Result kv_result = g_rust_wrapper_instance->execute_request(
                std::string(operation), 
                std::string(key), 
                std::string(value ? value : "")
            );
            
            if (kv_result.success && !kv_result.value.empty()) {
                *result = strdup(kv_result.value.c_str());
            } else {
                *result = nullptr;
            }
            
            return kv_result.success;
        } catch (...) {
            *result = nullptr;
            return false;
        }
    }
    
    void cpp_free_string(char* ptr) {
        if (ptr) free(ptr);
    }
    
    void cpp_cleanup_thread_info() {
        RustWrapper::cleanup_thread_info();
    }
}

int main() {
    abstract_db *db = new mbta_wrapper;
    db->init();
    RustWrapper* g_rust_wrapper = new RustWrapper();
    abstract_ordered_index *customerTable = db->open_index("customer_0");

    g_rust_wrapper->db = db;
    g_rust_wrapper->customerTable = customerTable;

    if (!g_rust_wrapper->init()) {
        std::cerr << "Failed to initialize rust wrapper!" << std::endl;
        delete g_rust_wrapper;
        delete db;
        return 1;
    } else {
        std::cout << "Successfully initialized rust wrapper!" << std::endl;
    }

    std::cout << "RustWrapper test server running on 127.0.0.1:6380" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    delete g_rust_wrapper;
    delete db;
    return 0;
}