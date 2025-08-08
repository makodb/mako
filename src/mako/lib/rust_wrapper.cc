#include "rust_wrapper.h"
#include <sstream>
#include <vector>

// Global instance pointer for Rust notification
RustWrapper* g_rust_wrapper_instance = nullptr;

// C function implementation for Rust to call
extern "C" void cpp_notify_request_available() {
    if (g_rust_wrapper_instance) {
        g_rust_wrapper_instance->notify_request_available();
    }
}

RustWrapper::RustWrapper() : running_(false), initialized_(false) {
    g_rust_wrapper_instance = this;
}

RustWrapper::~RustWrapper() {
    stop();
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

void RustWrapper::start_polling() {
    if (!initialized_ || processing_thread_.joinable()) {
        return;
    }
    
    processing_thread_ = std::thread(&RustWrapper::poll_requests, this);
    std::cout << "Started event-driven request processing thread" << std::endl;
}

void RustWrapper::stop() {
    if (running_) {
        running_ = false;
        // Notify the processing thread to wake up and exit
        request_cv_.notify_all();
        if (processing_thread_.joinable()) {
            processing_thread_.join();
        }
    }
    g_rust_wrapper_instance = nullptr;
}

void RustWrapper::notify_request_available() {
    request_cv_.notify_one();
}

void RustWrapper::poll_requests() {
    long int counter = 0;
    
    while (running_) {
        // Wait for notification from Rust
        std::unique_lock<std::mutex> lock(request_mutex_);
        request_cv_.wait(lock);
        
        if (!running_) {
            break;
        }
        
        // Process all available requests
        uint32_t id;
        char* request_data = nullptr;
        
        while (rust_retrieve_request_from_queue(&id, &request_data)) {
            counter++;
            std::cout << "Processing request " << id << ": " << request_data << std::endl;
            
            // Execute the batch request
            execute_batch_request(id, string(request_data ? request_data : ""));
            
            // Free the string allocated by Rust
            if (request_data) rust_free_string(request_data);
        }
        
        if (counter % 100 == 0) {
            std::cout << "Processed " << counter << " requests so far" << std::endl;
        }
    }
}

void RustWrapper::execute_request(uint32_t id, const string& operation, const string& key, const string& value) {
    KVStore::Result result = kv_store_.execute_operation(operation, key, value);
    
    // Send response back to Rust
    rust_put_response_back_queue(id, result.value.c_str(), result.success);
    
    std::cout << "Executed " << operation << " for key '" << key << "' -> " << result.value << std::endl;
}

void RustWrapper::execute_batch_request(uint32_t id, const string& request_data) {
    // Parse request_data format: "op1\r\nkey1\r\nval1\r\nop2\r\nkey2\r\nval2\r\n..."
    vector<string> lines;
    stringstream ss(request_data);
    string line;
    
    while (getline(ss, line)) {
        // Remove \r if present (getline removes \n but not \r)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);  // Keep empty lines too, they represent empty values
    }
    
    
    string batch_result = "";
    int operations_count = 0;
    
    // Process operations in groups of 3 (operation, key, value)  
    // Need at least 3 elements: i, i+1, i+2, so condition is i+2 < lines.size()
    for (size_t i = 0; i + 2 < lines.size(); i += 3) {
        string operation = lines[i];
        string key = lines[i + 1];
        string value = lines[i + 2];
        
        std::cout << "  Batch operation " << operations_count << ": " << operation << ":" << key << ":" << value << std::endl;
        
        KVStore::Result result = kv_store_.execute_operation(operation, key, value);
        
        // Add result to batch (separated by \r\n)
        if (operations_count > 0) {
            batch_result += "\r\n";
        }
        batch_result += result.value;
        operations_count++;
    }
    
    // Send batch response back to Rust
    rust_put_response_back_queue(id, batch_result.c_str(), true);
    
    std::cout << "Executed batch request " << id << " with " << operations_count << " operations" << std::endl;
}