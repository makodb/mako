# KVStore C++ Dynamic Library

A simple key-value store library with queue-based request/response interface.

## Features

- Simple key-value storage using std::map
- Request format: `{operation}:{key}:{value}`
- Supports `get` and `put` operations
- Unique request IDs for tracking operations
- Single-threaded design (no mutex overhead)

## API

### Methods

- `bool init()` - Initialize the store (must call before use)
- `int sendtoqueue(string request)` - Send request, returns unique ID
- `string recvfromqueue(int reqId)` - Get result for request ID

### Request Format

- **GET**: `get:key:`
- **PUT**: `put:key:value`

### Response Format

- **GET success**: Returns the value
- **GET failure**: Returns empty string
- **PUT success**: Returns "OK"
- **Error**: Returns "ERROR: ..." message

## Compilation

### Option 1: Using Makefile

```bash
# Build both shared and static libraries
make

# Build only shared library
make libkv_store.so

# Build only static library  
make libkv_store.a

# Clean build artifacts
make clean
```

### Option 2: Manual compilation

```bash
# Compile shared library
g++ -std=c++11 -fPIC -shared -o libkv_store.so kv_store.cpp

# Compile static library
g++ -std=c++11 -c kv_store.cpp -o kv_store.o
ar rcs libkv_store.a kv_store.o
```

### Option 3: Using with your project

```bash
# Include in your C++ project
g++ -std=c++11 your_code.cpp kv_store.cpp -o your_program

# Or link against the library
g++ -std=c++11 your_code.cpp -L. -lkv_store -o your_program
```

## Example Usage

```cpp
#include "kv_store.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    KVStore store;
    
    // Initialize
    if (!store.init()) {
        std::cerr << "Failed to initialize store" << std::endl;
        return 1;
    }
    
    // Put a value
    int put_id = store.sendtoqueue("put:name:John");
    
    // Retry until result is available (polling thread processes asynchronously)
    std::string put_result;
    for (int i = 0; i < 100; i++) {
        put_result = store.recvfromqueue(put_id);
        if (!put_result.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "PUT result: " << put_result << std::endl;  // "OK"
    
    // Get the value
    int get_id = store.sendtoqueue("get:name:");
    
    // Retry until result is available
    std::string get_result;
    for (int i = 0; i < 100; i++) {
        get_result = store.recvfromqueue(get_id);
        if (!get_result.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "GET result: " << get_result << std::endl;  // "John"
    
    return 0;
}
```

### Important Note on Asynchronous Operation

Since the KVStore uses a polling background thread, `recvfromqueue()` may return an empty string if the request hasn't been processed yet. Always implement a retry loop as shown above to ensure you get the result when it's ready.

## Files

- `kv_store.h` - Header file with class definition
- `kv_store.cpp` - Implementation file
- `Makefile` - Build configuration
- `README.md` - This documentation

## Requirements

- C++11 or later
- Standard C++ library (STL)
- GCC/Clang compiler