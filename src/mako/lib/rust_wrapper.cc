#include "lib/rust_wrapper.h"
#include <sstream>
#include <vector>

// Global instance pointer for Rust notification
RustWrapper* g_rust_wrapper_instance = nullptr;
RustWrapper::RustWrapper() : running_(false), initialized_(false) {
    g_rust_wrapper_instance = this;
    txn_obj_buf.reserve(str_arena::MinStrReserveLength);
}

RustWrapper::~RustWrapper() {
    if (running_) running_ = false;
    g_rust_wrapper_instance = nullptr;
    delete arena;
}

bool RustWrapper::init() {
    if (initialized_) {
        return false; // Already initialized
    }
    
    arena = new str_arena();
    txn_obj_buf.resize(db->sizeof_txn_object(0));

    // Initialize Rust socket listener
    if (!rust_init()) {
        std::cerr << "Failed to initialize Rust socket listener" << std::endl;
        return false;
    }
    
    running_ = true;
    initialized_ = true;
    
    std::cout << "RustWrapper initialized successfully" << std::endl;
    return true;
}

extern "C" {
    bool cpp_execute_request_sync(const char* operation, const char* key, const char* value, char** result) {
        std::string op_str(operation);
        std::string key_str(key);
        std::string val_str(value ? value : "");
        
        RustWrapper::Result kv_result = g_rust_wrapper_instance->execute_request(op_str, key_str, val_str);
        
        if (kv_result.success && !kv_result.value.empty()) {
            // Allocate C string for result
            *result = strdup(kv_result.value.c_str());
        } else {
            *result = nullptr;
        }
        
        std::cout << "Executed " << op_str << " for key '" << key_str << "' -> " << kv_result.value << std::endl;
        
        return kv_result.success;
    }
    
    void cpp_free_string(char* ptr) {
        if (ptr) {
            free(ptr);
        }
    }
}


RustWrapper::Result RustWrapper::execute_request(const string& operation, const string& key, const string& value) {
    string result;
    bool success = true;
    
    if (operation == "get") {
        void *txn = db->new_txn(0, *arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
        std::string table_value = "";
        try {
            customerTable->get(txn, key, table_value);
            db->commit_txn(txn);
        } catch (abstract_db::abstract_abort_exception &ex) {
            std::cout << "abort (read) key=" << key << std::endl;
            db->abort_txn(txn);
        }
        result = table_value;
    } else if (operation == "set") {
        std::cout << "DEBUG: Performing SET operation" << std::endl;
        void *txn = db->new_txn(0, *arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
        std::cout << "DEBUG: Transaction created" << std::endl;
            try {
                customerTable->put(txn, key, value);
                std::cout << "DEBUG: Put operation completed" << std::endl;
                db->commit_txn(txn);
                std::cout << "DEBUG: Transaction committed" << std::endl;
            } catch (abstract_db::abstract_abort_exception &ex) {
                std::cout << "abort key=" << key << std::endl;
                db->abort_txn(txn);
            }
        result = "OK";
    } else {
        result = "ERROR: Invalid operation";
        success = false;
    }
    
    return Result(result, success);
}