# RPC Benchmark Tool

## Overview

The `rpcbench` tool is a performance benchmarking utility for the RRR RPC framework. It measures throughput and latency of RPC calls under various configurations.

## Building

The RPC benchmark has been added to the CMake build system. To build it:

```bash
mkdir -p build
cd build
cmake ..
make rpcbench -j4
```

The binary will be created at `build/rpcbench`.

## Usage

The benchmark operates in client-server mode. You need to run one instance as a server and another as a client.

### Basic Usage

**Start the server:**
```bash
./rpcbench -s 0.0.0.0:8848
```

**Start the client (in another terminal):**
```bash
./rpcbench -c 127.0.0.1:8848
```

### Command Line Options

```
-c <ip:port>    Run as client, connect to specified server
-s <ip:port>    Run as server, listen on specified address
-b <size>       Set message size in bytes (default: 10)
-e <count>      Number of epoll instances (default: 2)
-f              Use fast RPC mode (execute in network thread)
-n <seconds>    Duration of benchmark in seconds (default: 10)
-o <count>      Number of outstanding requests (default: 1000)
-t <count>      Number of client threads (default: 8)
-w <count>      Number of worker threads on server (default: 16)
-v <size>       Size of vector in RPC calls (default: 0)
```

### Examples

**Basic throughput test (10 seconds):**
```bash
# Server
./rpcbench -s 0.0.0.0:8848

# Client
./rpcbench -c localhost:8848 -n 10
```

**Test with larger messages (1KB):**
```bash
# Server
./rpcbench -s 0.0.0.0:8848

# Client
./rpcbench -c localhost:8848 -b 1024
```

**High concurrency test:**
```bash
# Server with 32 worker threads
./rpcbench -s 0.0.0.0:8848 -w 32

# Client with 16 threads and 5000 outstanding requests
./rpcbench -c localhost:8848 -t 16 -o 5000
```

**Fast mode (low latency):**
```bash
# Server
./rpcbench -s 0.0.0.0:8848 -f

# Client
./rpcbench -c localhost:8848 -f
```

**Vector operations benchmark:**
```bash
# Server
./rpcbench -s 0.0.0.0:8848

# Client with vector size 100
./rpcbench -c localhost:8848 -v 100
```

## Benchmark Metrics

The client reports the following metrics:

1. **Throughput**: Requests per second (RPS)
2. **Latency**: Average, min, max response times
3. **Statistics**: Updated every second during the test

Output format:
```
client qps: 142857
client qps: 145231
client qps: 143589
...
Average: 144235 reqs/s
```

## RPC Methods Tested

The benchmark tests the following RPC methods defined in `benchmark_service.rpc`:

1. **fast_nop()**: No-operation, minimal overhead
2. **fast_nop_raw()**: Raw marshaling test
3. **fast_string_concat()**: String operations
4. **fast_vector_sum()**: Vector aggregation operations
5. **nop()**: Standard RPC call (deferred execution)

## Performance Tuning Tips

### Server Side
- Increase worker threads (`-w`) for CPU-intensive operations
- Use fast mode (`-f`) for simple, non-blocking operations
- Adjust epoll instances (`-e`) based on CPU cores

### Client Side
- Increase client threads (`-t`) to generate more load
- Adjust outstanding requests (`-o`) to control concurrency
- Use multiple client machines for distributed load generation

### Network
- Use localhost for minimal network latency tests
- Test on actual network to measure realistic performance
- Consider network tuning (TCP_NODELAY, buffer sizes)

## Typical Performance Numbers

On modern hardware with localhost connection:

- **Fast mode**: 500,000+ RPS
- **Standard mode**: 100,000-200,000 RPS
- **Latency**: < 100 microseconds (fast mode)

Actual performance depends on:
- CPU performance
- Network latency
- Message size
- Number of threads
- System load

## Troubleshooting

### Server won't start
- Check if port is already in use: `netstat -an | grep 8848`
- Ensure you have permissions to bind to the port
- Try a different port number

### Client connection fails
- Verify server is running: `ps aux | grep rpcbench`
- Check firewall settings
- Ensure correct IP and port

### Low performance
- Check CPU utilization: `top` or `htop`
- Monitor network: `iftop` or `nethogs`
- Verify no other processes competing for resources
- Try adjusting thread counts and outstanding requests

### Crashes or errors
- Check system limits: `ulimit -n` (file descriptors)
- Ensure sufficient memory available
- Review logs for error messages
- Build with debug symbols for detailed stack traces

## Integration with Build Systems

### CMake (Current)
The benchmark is now integrated into the CMake build system. See the modifications in `CMakeLists.txt`.

### WAF (Legacy)
The original WAF build configuration is commented out but can be re-enabled in `wscript`:
```python
# Uncomment in wscript to build with WAF
_depend("test/benchmark_service.h", "test/benchmark_service.rpc",
        "bin/rpcgen --cpp test/benchmark_service.rpc")
```

## Source Files

- `test/rpcbench.cc` - Main benchmark implementation
- `test/benchmark_service.cc` - Service implementation
- `test/benchmark_service.rpc` - RPC service definition
- `test/benchmark_service.h` - Generated header (from .rpc file)

## Related Documentation

- [RRR RPC Guide](rrr-rpc.md) - Complete RPC framework documentation
- [Performance Profiling](profile.md) - General profiling guide