# Transport Backend Configuration Guide

This document describes how to switch between the two RPC transport backends available in the Mako system.

## Overview

The Mako distributed transaction system supports two transport backends:

1. **rrr/rpc** (default) - Portable TCP/IP-based RPC
2. **eRPC** - High-performance RDMA-based RPC

Both backends implement the same `TransportBackend` interface, providing transport-agnostic request/response handling.

## Backend Comparison

| Feature | eRPC | rrr/rpc |
|---------|------|---------|
| **Latency** | ~1-2 μs (RDMA) | ~10-50 μs (TCP/IP) |
| **Hardware** | Requires RDMA-capable NICs | Standard Ethernet |
| **Portability** | Linux with RDMA drivers | Any platform |
| **Scalability** | Excellent (kernel bypass) | Good |
| **Use Case** | Production clusters, benchmarks | Development, testing, portability |

## Switching Between Backends

### Method 1: Environment Variable (Recommended)

Set the `MAKO_TRANSPORT` environment variable before running:

```bash
# Use rrr/rpc backend (default)
./build/dbtest config/mako_tpcc.yml

# Use eRPC backend
export MAKO_TRANSPORT=erpc
./build/dbtest config/mako_tpcc.yml

# Or inline
MAKO_TRANSPORT=erpc ./build/dbtest config/mako_tpcc.yml
```

### Method 2: Configuration File

Add the transport type to your YAML configuration file:

```yaml
# config/mako_tpcc.yml
transport:
  type: erpc  # Options: rrr (default), erpc
  # ... other transport settings
```

### Method 3: Build-Time Configuration

Set the default backend at build time (CMake):

```bash
# Build with rrr/rpc as default (standard)
cmake -DMAKO_DEFAULT_TRANSPORT=rrr ..
make -j32

# Build with eRPC as default
cmake -DMAKO_DEFAULT_TRANSPORT=erpc ..
make -j32
```

## Testing Both Backends

### Single-Shard Tests

```bash
# Test rrr/rpc (default)
./ci/ci.sh simpleTransaction

# Test eRPC
MAKO_TRANSPORT=erpc ./ci/ci.sh simpleTransaction
```

### Multi-Shard Tests

```bash
# Test rrr/rpc with 2 shards, no replication (default)
./ci/ci.sh shardNoReplication

# Test eRPC with 2 shards, no replication
MAKO_TRANSPORT=erpc ./ci/ci.sh shardNoReplication

# Test with replication
./ci/ci.sh shard1Replication
```

## Architecture

### Transport-Agnostic Design

The system uses an abstract `TransportRequestHandle` interface that both backends implement:

```cpp
class TransportRequestHandle {
    virtual uint8_t GetRequestType() const = 0;
    virtual char* GetRequestBuffer() = 0;
    virtual char* GetResponseBuffer() = 0;
    virtual void* GetOpaqueHandle() = 0;
    virtual void EnqueueResponse(size_t msg_size) = 0;
};
```

### Backend Implementations

**eRPC Backend** (`ErpcBackend`, `ErpcRequestHandle`):
- Located in `src/mako/lib/erpc_backend.{h,cc}`
- Uses RDMA for zero-copy, kernel-bypass networking
- Optimal for production clusters with RDMA hardware

**rrr/rpc Backend** (`RrrRpcBackend`, `RrrRequestHandle`):
- Located in `src/mako/lib/rrr_rpc_backend.{h,cc}`
- Uses standard TCP/IP sockets via rrr library
- Portable across all platforms, no special hardware required

### Worker Thread Independence

Worker threads in `src/mako/lib/server.cc` use only the abstract interface:

```cpp
// Transport-agnostic request processing
size_t msgLen = shardReceiver->ReceiveRequest(
    req_handle->GetRequestType(),
    req_handle->GetRequestBuffer(),
    req_handle->GetResponseBuffer());

// Transport-agnostic response enqueueing
req_handle->EnqueueResponse(msgLen);
```

This design allows:
- Easy switching between backends at runtime
- No code changes required when switching transports
- Ability to add new transport backends without modifying worker code

## Performance Characteristics

### eRPC Backend

**Advantages:**
- Ultra-low latency (~1-2 μs round-trip)
- High throughput (millions of requests/second)
- Zero-copy, kernel-bypass networking
- Excellent for CPU-bound workloads

**Requirements:**
- RDMA-capable network interface cards (Mellanox, Intel)
- RDMA drivers (libibverbs, librdmacm)
- Privileged access for huge pages

**Typical Use Cases:**
- Production datacenter deployments
- Performance benchmarking
- Research experiments requiring low latency

### rrr/rpc Backend

**Advantages:**
- No special hardware requirements
- Works on any Linux/Unix system
- Easy to debug with standard network tools (tcpdump, wireshark)
- Suitable for development and testing

**Characteristics:**
- Moderate latency (~10-50 μs round-trip)
- Good throughput (~100k-500k requests/second)
- TCP/IP with event-driven I/O (epoll)

**Typical Use Cases:**
- Development and debugging
- Testing on laptops/VMs without RDMA
- Deployment on cloud instances without RDMA
- Cross-platform compatibility

## Verified Performance

Both backends have been tested and verified:

| Test | eRPC | rrr/rpc |
|------|------|---------|
| **Single-shard transactions** | ✅ PASSED | ✅ PASSED |
| **Multi-shard (2 shards)** | ✅ PASSED | ✅ PASSED (387k req/30s) |
| **With replication** | ✅ PASSED | ✅ TESTED |
| **Cross-partition txns** | ✅ PASSED | ✅ PASSED |
| **Helper queue integration** | ✅ PASSED | ✅ PASSED |

## Troubleshooting

### eRPC Issues

**Problem**: "eRPC Nexus: Failed to create"
- **Solution**: Check RDMA drivers are installed (`ibstat`, `ibv_devices`)
- **Solution**: Ensure huge pages are configured (`echo 2048 > /proc/sys/vm/nr_hugepages`)

**Problem**: "Failed to create session"
- **Solution**: Verify network connectivity and firewall rules
- **Solution**: Check that ports are not already in use

### rrr/rpc Issues

**Problem**: "Failed to connect to address"
- **Solution**: Verify target host/port is reachable
- **Solution**: Check firewall allows TCP connections

**Problem**: "Address already in use"
- **Solution**: Wait for TIME_WAIT to expire or use different ports
- **Solution**: Kill lingering processes (`killall -9 dbtest`)

### General Debugging

Enable verbose logging:
```bash
# Set log level
export MAKO_LOG_LEVEL=debug

# Run with logging
MAKO_TRANSPORT=rrr ./build/dbtest config/mako_tpcc.yml 2>&1 | tee test.log
```

Check statistics:
- Both backends print request/response statistics at shutdown
- Look for "msg_size_req_sent", "msg_counter_req_sent" in logs
- Verify non-zero response counts

## Adding New Transport Backends

To add a new transport backend:

1. **Create backend class** inheriting from `TransportBackend`:
   ```cpp
   class MyBackend : public TransportBackend {
       // Implement all virtual methods
   };
   ```

2. **Create request handle** inheriting from `TransportRequestHandle`:
   ```cpp
   class MyRequestHandle : public TransportRequestHandle {
       // Implement all virtual methods including EnqueueResponse()
   };
   ```

3. **Register in configuration**:
   - Add new enum value to `TransportType`
   - Update `FastTransport` to instantiate your backend

4. **Test thoroughly**:
   - Single-shard transactions
   - Multi-shard distributed transactions
   - Cross-partition transactions
   - With/without replication

## References

- eRPC library: https://github.com/erpc-io/eRPC
- rrr/rpc library: `src/rrr/` (internal)
- Transport backend interface: `src/mako/lib/transport_backend.h`
- Request handle interface: `src/mako/lib/transport_request_handle.h`
- Example configurations: `config/mako_*.yml`

## Summary

The dual transport backend architecture provides:
- **Flexibility**: Choose the right backend for your deployment
- **Portability**: Run on any hardware (rrr/rpc) or optimize for performance (eRPC)
- **Maintainability**: Transport-agnostic worker code simplifies development
- **Extensibility**: Easy to add new transport backends

By default, the system uses **rrr/rpc** for maximum portability and ease of development.
For production deployments with RDMA hardware, switch to **eRPC** for best performance.
