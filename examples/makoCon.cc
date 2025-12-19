#include <iostream>
#include <chrono>
#include <thread>
#include <mako.hh>
#include <examples/common.h>
#include "lib/rust_wrapper.h"

RustWrapper* g_rust_wrapper_instance = nullptr;
thread_local str_arena* RustWrapper::tl_arena = nullptr;
thread_local std::string RustWrapper::tl_txn_obj_buf;
thread_local bool RustWrapper::tl_initialized = false;
thread_local bool RustWrapper::ti_initialized = false;
thread_local std::string RustWrapper::tl_key_buf;
thread_local std::string RustWrapper::tl_val_buf;

// RustWrapper implementations
RustWrapper::RustWrapper() : running_(false), initialized_(false) {
    g_rust_wrapper_instance = this;
}

RustWrapper::~RustWrapper() {
    if (running_) running_ = false;
    if (ti_initialized) {
        ti_initialized = false;
    }
    g_rust_wrapper_instance = nullptr;
}

void* RustWrapper::txn_buf() { 
    return (void*)tl_txn_obj_buf.data();
}

bool RustWrapper::init() {
    if (initialized_) {
        return false;
    }
    size_t max_threads = 8;
    auto& config = BenchmarkConfig::getInstance();

    if (!rust_init(config.getNthreads())) {
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
        // std::cout << "DEBUG: About to call mbta_type::thread_init()" << std::endl;
        mbta_ordered_index::mbta_type::thread_init();
        ti_initialized = true;
        // std::cout << "DEBUG: Initialized thread via mbta_type::thread_init() for thread " 
        //           << std::this_thread::get_id() << std::endl;
    }
    if (!tl_initialized) {
        tl_arena = new str_arena();
        tl_txn_obj_buf.resize(db->sizeof_txn_object(0));
        tl_initialized = true;
    }
}

RustWrapper::Result RustWrapper::execute_request(OpCode op,
                             const uint8_t* key_ptr, size_t key_len,
                             const uint8_t* val_ptr, size_t val_len) {
    ensure_thread_info();
    
    // std::cout << "Thread_id: " << std::this_thread::get_id() 
    //     << "   DB Address: " << static_cast<const void*>(db) 
    //     << "   Table Address: " << static_cast<const void*>(customerTable) << std::endl;
    
    std::string result;
    bool success = true;

    if (tl_arena)
    {
        tl_arena->reset();
    }
    
    try {
        if (op == OpCode::Get) {
            void *txn = db->new_txn(0, *tl_arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            
            // Reuse thread-local buffer instead of allocating new string
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);
            
            tl_val_buf.clear();
            try {
                customerTable->get(txn, tl_key_buf, tl_val_buf);
                db->commit_txn(txn);
                result = tl_val_buf;
            } catch (abstract_db::abstract_abort_exception &ex) {
                // std::cout << "abort (read) key=" << tl_key_buf << std::endl;
                db->abort_txn(txn);
            } catch (...) {
                db->abort_txn(txn);
                success = false;
                result = "ERROR: Exception";
            }
        } else if (op == OpCode::Set) {
            void *txn = db->new_txn(0, *tl_arena, txn_buf());
            
            // Reuse thread-local key buffer
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);

            // Reuse thread-local value buffer
            tl_val_buf.clear();
            tl_val_buf.reserve(sizeof("table_value_") - 1 + val_len + mako::EXTRA_BITS_FOR_VALUE);
            tl_val_buf.append("table_value_", sizeof("table_value_") - 1);
            if (val_ptr && val_len) {
                tl_val_buf.append(reinterpret_cast<const char*>(val_ptr), val_len);
            }
            tl_val_buf.append(mako::EXTRA_BITS_FOR_VALUE, 'B');

            try {
                customerTable->put(txn, tl_key_buf, StringWrapper(tl_val_buf));
                db->commit_txn(txn);
                result = "OK";
            } catch (abstract_db::abstract_abort_exception &ex) {
                // printf("Write aborted: %s\n", tl_key_buf.c_str());
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
    if (tl_arena) { delete tl_arena; tl_arena = nullptr; }
    tl_txn_obj_buf.clear();
    tl_initialized = false;
    if (ti_initialized) {
        ti_initialized = false;
    }
}

extern "C" {
    bool cpp_execute_request_sync(uint32_t op,
                             const uint8_t* key_ptr, size_t key_len,
                             const uint8_t* val_ptr, size_t val_len,
                             uint8_t** out_ptr, size_t* out_len) {
        if (!g_rust_wrapper_instance || !key_ptr || !out_ptr || !out_len) {
            if (out_ptr) *out_ptr = nullptr;
            if (out_len) *out_len = 0;
            return false;
        }

        OpCode opcode;
        switch (op) {
            case 1: opcode = OpCode::Get; break;
            case 2: opcode = OpCode::Set; break;
            default:
            *out_ptr = nullptr;
            *out_len = 0;
            return false;
        }

        RustWrapper::Result kv = g_rust_wrapper_instance->execute_request(
            opcode, key_ptr, key_len, val_ptr, val_len);

        if (!kv.success) {
            *out_ptr = nullptr;
            *out_len = 0;
            return false;
        }

        if (opcode == OpCode::Get) {
            if (!kv.value.empty()) {
            size_t n = kv.value.size();
            auto* buf = static_cast<uint8_t*>(std::malloc(n));
            if (!buf) {
                *out_ptr = nullptr;
                *out_len = 0;
                return false;
            }
            std::memcpy(buf, kv.value.data(), n);
            *out_ptr = buf;
            *out_len = n;
            } else {
            // GET miss
            *out_ptr = nullptr;
            *out_len = 0;
            }
        } else {
            // SET → no payload
            *out_ptr = nullptr;
            *out_len = 0;
        }

        return true;
    }
    
    void cpp_free_buf(uint8_t* ptr, size_t len) {
        if (ptr) {
            std::free(ptr);
        }
    }
    
    void cpp_cleanup_thread_info() {
        RustWrapper::cleanup_thread_info();
    }
}

int main() {
    size_t num_threads = 8;
    auto& config = BenchmarkConfig::getInstance();
    config.setNthreads(num_threads);
    std::cout << "Configuration: " << config.getNthreads() << " threads" << std::endl;
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
