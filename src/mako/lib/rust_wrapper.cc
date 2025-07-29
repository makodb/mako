#include "lib/rust_wrapper.h"


RustWrapper::RustWrapper() : running_(false), initialized_(false) {
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
    if (!initialized_ || polling_thread_.joinable()) {
        return;
    }
    
    polling_thread_ = std::thread(&RustWrapper::poll_requests, this);
    std::cout << "Started polling thread for request processing" << std::endl;
}

void RustWrapper::stop() {
    if (running_) {
        running_ = false;
        if (polling_thread_.joinable()) {
            polling_thread_.join();
        }
    }
}

void RustWrapper::poll_requests() {
    long int counter = 0;
    
    while (running_) {
        counter++;
        
        uint32_t id;
        char* operation = nullptr;
        char* key = nullptr;
        char* value = nullptr;
        
        // Poll Rust for requests
        if (rust_retrieve_request_from_queue(&id, &operation, &key, &value)) {
            std::cout << "Processing request " << id << ": " << operation << ":" << key << ":" << value << std::endl;
            
            // Execute the request
            execute_request(id, 
                           operation ? operation : "", 
                           key ? key : "", 
                           value ? value : "");
            
            // Free the C strings allocated by Rust
            if (operation) rust_free_string(operation);
            if (key) rust_free_string(key);
            if (value) rust_free_string(value);
        }
        
        // Sleep for a short time to avoid busy polling
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        if (counter % 100 == 0) {
            std::cout << "Polling loop: " << counter << " iterations" << std::endl;
        }
    }
}

void RustWrapper::execute_request(uint32_t id, const string& operation, const string& key, const string& value) {
    string result;
    bool success = true;
    
    if (operation == "get") {
        auto it = store_.find(key);
        if (it != store_.end()) {
            result = it->second;
        } else {
            result = ""; // Key not found
            success = false;
        }
    } else if (operation == "set") {
        store_[key] = value;
        result = "OK";
    } else {
        result = "ERROR: Invalid operation";
        success = false;
    }
    
    // Send response back to Rust
    rust_put_response_back_queue(id, result.c_str(), success);
    
    std::cout << "Executed " << operation << " for key '" << key << "' -> " << result << std::endl;
}