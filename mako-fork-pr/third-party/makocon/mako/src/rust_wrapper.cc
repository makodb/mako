#include "rust_wrapper.h"
#include <sstream>
#include <vector>

// Global instance pointer for Rust notification
RustWrapper* g_rust_wrapper_instance = nullptr;

RustWrapper::RustWrapper() : running_(false), initialized_(false) {
    g_rust_wrapper_instance = this;
}

RustWrapper::~RustWrapper() {
    if (running_) running_ = false;
    g_rust_wrapper_instance = nullptr;
}

bool RustWrapper::init() {
    if (initialized_) {
        return false; // Already initialized
    }
    
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
        
        KVStore::Result kv_result = g_rust_wrapper_instance->kv_store_.execute_operation(op_str, key_str, val_str);
        
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


// void RustWrapper::execute_request(uint32_t id, const string& operation, const string& key, const string& value) {
//     KVStore::Result result = kv_store_.execute_operation(operation, key, value);
    
//     // Send response back to Rust
//     rust_put_response_back_queue(id, result.value.c_str(), result.success);
    
//     std::cout << "Executed " << operation << " for key '" << key << "' -> " << result.value << std::endl;
// }

// void RustWrapper::execute_batch_request(uint32_t id, const string& request_data) {
//     // Parse request_data format: "op1\r\nkey1\r\nval1\r\nop2\r\nkey2\r\nval2\r\n..."
//     vector<string> lines;
//     stringstream ss(request_data);
//     string line;
    
//     while (getline(ss, line)) {
//         // Remove \r if present (getline removes \n but not \r)
//         if (!line.empty() && line.back() == '\r') {
//             line.pop_back();
//         }
//         lines.push_back(line);  // Keep empty lines too, they represent empty values
//     }
    
    
//     string batch_result = "";
//     int operations_count = 0;
    
//     // Process operations in groups of 3 (operation, key, value)  
//     // Need at least 3 elements: i, i+1, i+2, so condition is i+2 < lines.size()
//     for (size_t i = 0; i + 2 < lines.size(); i += 3) {
//         string operation = lines[i];
//         string key = lines[i + 1];
//         string value = lines[i + 2];
        
//         std::cout << "  Batch operation " << operations_count << ": " << operation << ":" << key << ":" << value << std::endl;
        
//         KVStore::Result result = kv_store_.execute_operation(operation, key, value);
        
//         // Add result to batch (separated by \r\n)
//         if (operations_count > 0) {
//             batch_result += "\r\n";
//         }
//         batch_result += result.value;
//         operations_count++;
//     }
    
//     // Send batch response back to Rust
//     rust_put_response_back_queue(id, batch_result.c_str(), true);
    
//     std::cout << "Executed batch request " << id << " with " << operations_count << " operations" << std::endl;
// }