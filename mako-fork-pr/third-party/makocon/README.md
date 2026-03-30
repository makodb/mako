# Mako KV Store

A high-performance key-value store implementation with a hybrid C++/Rust architecture where C++ handles the main logic and storage while Rust manages socket communication and request queuing.

## Architecture

The Mako KV Store uses a unique hybrid architecture:

- **C++ Main Process**: Handles the main application logic, key-value storage, and request processing
- **Rust Socket Listener**: Manages TCP socket connections and maintains request/response queues
- **Inter-language Communication**: C++ polls Rust for incoming requests and sends responses back

### Architecture Flow

1. **C++ main()** initializes the KV store and starts a polling thread
2. **rust_init()** is called in c++ to start the Rust socket listener on port 6380
3. **Rust listener** accepts client connections and parses Redis protocol commands
4. **Request queuing**: Rust adds parsed requests from the client to an internal queue with unique IDs
5. **C++ polling**: C++ continuously polls Rust for new requests via `rust_retrieve_request_from_queue()`
6. **Request processing**: C++ executes the request (GET/PUT operations) on its internal storage
7. **Response handling**: C++ sends the result back to Rust via `rust_put_response_back_queue()`
8. **Client response**: Rust waits for the response and sends it back to the client

## Request Format
The system supports Redis-compatible commands with the format: `{operation}:{key}:{value}`

- **GET requests**: `get:key_name:`
- **SET requests**: `set:key_name:value_data`

## Building

### Prerequisites

- Rust (latest stable version)
- C++ compiler with C++17 support (g++/clang++)
- CMake (version 3.16 or higher)
- netcat (for testing)

### Build Steps

```bash
# Create build directory and configure
cd mako
rm -rf build
mkdir -p build
cd build
cmake ..
# (for mac) cmake -DCMAKE_OSX_ARCHITECTURES=arm64 ..
make -j$(nproc)
cd ../
```

### Start the Server

```bash
./build/mako_server
```

The server will:
- Initialize the C++ KV store
- Start the Rust socket listener on `127.0.0.1:6380`
- Begin polling for requests from rust

### Testing with Redis Clients

You can test the server using any Redis client or simple telnet/netcat:

#### Using Redis CLI
```bash
redis-cli -p 6380
> SET mykey myvalue
OK
> GET mykey
"myvalue"
```

## Cleanup

```bash
# Stop the server (Ctrl+C)

# Clean build artifacts
cd build
make clean

# Clean everything including Rust artifacts
make clean_all
```

## Notes
We have a major different implementation in makocon, where the main function is in rust, instead of c++.