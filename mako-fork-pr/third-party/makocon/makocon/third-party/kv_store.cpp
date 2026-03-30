#include "kv_store.h"
#include <sstream>

KVStore::KVStore() : running_(false), next_id_(1), initialized_(false) {
}

KVStore::~KVStore() {
    if (running_) {
        running_ = false;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

bool KVStore::init() {
    if (initialized_) {
        return false; // Already initialized
    }
    
    running_ = true;
    initialized_ = true;
    
    // Start the long-running thread that polls the request queue
    worker_thread_ = boost::thread(&KVStore::processRequests, this);
    
    return true;
}

int KVStore::sendtoqueue(string request) {
    if (!initialized_) {
        return -1; // Not initialized
    }
    
    // Parse request format: {op}:{key}:{value}
    std::istringstream iss(request);
    std::string operation, key, value;
    
    if (!std::getline(iss, operation, ':') || 
        !std::getline(iss, key, ':')) {
        return -1; // Invalid format
    }
    
    // For PUT operations, get the value; for GET operations, value can be empty
    std::getline(iss, value);
    
    if (operation != "get" && operation != "put") {
        return -1; // Invalid operation
    }
    
    if (operation == "put" && value.empty()) {
        return -1; // PUT requires a value
    }
    
    // Create request with unique ID
    Request req;
    req.id = next_id_.fetch_add(1);
    req.operation = operation;
    req.key = key;
    req.value = value;
    
    // Add to request queue (thread-safe)
    {
        boost::lock_guard<boost::mutex> lock(queue_mutex_);
        request_queue_.push(req);
    }
    
    // No need to notify - worker thread is polling
    
    return req.id;
}

string KVStore::recvfromqueue(int reqId) {
    // Block until the response is available
    while (true) {
        {
            boost::lock_guard<boost::mutex> lock(response_mutex_);
            
            auto it = response_map_.find(reqId);
            if (it != response_map_.end()) {
                string result = it->second.result;
                response_map_.erase(it); // Remove processed response
                return result;
            }
        }
        
        // Sleep briefly before checking again to avoid busy waiting
        boost::this_thread::sleep_for(boost::chrono::milliseconds(1));
    }
}

void KVStore::processRequests() {
    long int counter = 0;
    while (running_) {
        // Poll the request queue
        {
            counter += 1;
            boost::lock_guard<boost::mutex> lock(queue_mutex_);
            
            // Process all pending requests
            while (!request_queue_.empty()) {
                std::cout<<"get a request from the request queue "<<std::endl;
                Request req = request_queue_.front();
                request_queue_.pop();
                
                // Execute the request (unlock not needed since executeRequest is thread-safe)
                executeRequest(req);
            }
        }
        
        // Sleep for a short time to avoid busy polling
        boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
        if (counter%100==0)
            std::cout<<"loop: "<<counter<<" times on the queue"<<std::endl;
    }
}

void KVStore::executeRequest(const Request& req) {
    Response response;
    response.id = req.id;
    response.success = true;
    
    if (req.operation == "get") {
        auto it = store_.find(req.key);
        if (it != store_.end()) {
            response.result = it->second;
        } else {
            response.result = ""; // Key not found
            response.success = false;
        }
    } else if (req.operation == "put") {
        store_[req.key] = req.value;
        response.result = "OK";
    } else {
        response.result = "ERROR: Invalid operation";
        response.success = false;
    }
    
    // Store response (thread-safe)
    {
        boost::lock_guard<boost::mutex> lock(response_mutex_);
        response_map_[req.id] = response;
    }
}