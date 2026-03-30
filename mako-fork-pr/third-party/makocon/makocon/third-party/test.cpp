#include "kv_store.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

int main() {
    KVStore store;
    
    // Test initialization
    std::cout << "Testing initialization..." << std::endl;
    assert(store.init() == true);
    assert(store.init() == false); // Should fail on second init
    
    // Test PUT operation
    std::cout << "Testing PUT operation..." << std::endl;
    int put_id = store.sendtoqueue("put:name:John");
    assert(put_id > 0);
    
    // Retry until result is available
    std::string put_result;
    for (int i = 0; i < 100; i++) {
        put_result = store.recvfromqueue(put_id);
        if (!put_result.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "PUT result: '" << put_result << "'" << std::endl;
    assert(put_result == "OK");
    
    // Test GET operation
    std::cout << "Testing GET operation..." << std::endl;
    int get_id = store.sendtoqueue("get:name:");
    assert(get_id > 0);
    
    // Retry until result is available
    std::string get_result;
    for (int i = 0; i < 100; i++) {
        get_result = store.recvfromqueue(get_id);
        if (!get_result.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "GET result: " << get_result << std::endl;
    assert(get_result == "John");
    
    // Test GET non-existent key
    std::cout << "Testing GET non-existent key..." << std::endl;
    int get_id2 = store.sendtoqueue("get:nonexistent:");
    
    // Wait for processing (non-existent keys return empty string, so we need to wait)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::string get_result2 = store.recvfromqueue(get_id2);
    std::cout << "GET non-existent result: '" << get_result2 << "'" << std::endl;
    // Note: For non-existent keys, we expect empty string but the response should exist
    
    // Test invalid operation
    std::cout << "Testing invalid operation..." << std::endl;
    int invalid_id = store.sendtoqueue("delete:key:value");
    assert(invalid_id == -1);
    
    // Test invalid format
    std::cout << "Testing invalid format..." << std::endl;
    int invalid_format_id = store.sendtoqueue("get");
    assert(invalid_format_id == -1);
    
    // Test PUT without value
    std::cout << "Testing PUT without value..." << std::endl;
    int put_no_value_id = store.sendtoqueue("put:key:");
    assert(put_no_value_id == -1);
    
    std::cout << "All tests passed!" << std::endl;
    return 0;
}