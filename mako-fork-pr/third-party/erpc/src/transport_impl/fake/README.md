# Socket-Based Transport Implementation (FakeTransport)

## Overview

The FakeTransport implementation provides a UDP socket-based transport layer for eRPC that enables deployment on standard commodity hardware without specialized network equipment. Unlike DPDK or InfiniBand transports that require specific hardware and bypass the kernel, this implementation uses standard BSD sockets through the kernel networking stack.

## Socket Transport vs Hardware Transports

### Architecture Differences

| Aspect | Socket Transport | DPDK/InfiniBand |
|--------|------------------|-----------------|
| **Kernel Bypass** | Uses kernel networking stack | Bypasses kernel for direct hardware access |
| **Memory Model** | Standard malloc/free with copies | Zero-copy with pre-registered memory regions |
| **Threading** | Background receive thread + kernel scheduling | Polling threads with user-space scheduling |
| **Hardware Requirements** | Any network interface | Specialized NICs (DPDK-compatible or IB) |
| **Deployment** | Standard Linux servers | Requires hardware-specific drivers |

### Performance Trade-offs

**Socket Transport Characteristics:**
- **Higher Latency**: Kernel context switches add ~1-5μs overhead per operation
- **Memory Copies**: Packets copied between kernel and user space
- **CPU Overhead**: Kernel processing and thread context switches
- **Bandwidth**: Limited by kernel networking stack efficiency
- **Portability**: Runs on any Linux system with standard networking

**Hardware Transport Benefits:**
- **Lower Latency**: Sub-microsecond latencies with kernel bypass
- **Zero-Copy**: Direct memory access without kernel involvement  
- **Higher Throughput**: 10-100Gbps+ with optimal CPU utilization
- **Deterministic Performance**: Predictable timing without kernel interference

## Implementation Architecture

### Core Components

1. **UDP Socket Management**: Creates and configures UDP sockets with appropriate options (`SO_REUSEADDR`, non-blocking I/O)
2. **Dedicated Receive Thread**: Background thread continuously polls the socket for incoming packets
3. **Packet Queue**: Thread-safe queue buffers received packets between the receive thread and eRPC's processing
4. **Direct Ring Integration**: Places received packets directly into eRPC's RX ring buffer for zero-copy handoff to eRPC
5. **Automatic IP Resolution**: Determines the best local IP address by connecting to a remote endpoint and inspecting the chosen interface

### Socket-Specific Design Patterns

**Asynchronous Reception Model:**
Unlike hardware transports that use polling loops, the socket transport employs a dedicated receive thread that blocks on `recvfrom()`. This design accommodates the kernel's asynchronous nature while maintaining compatibility with eRPC's synchronous RX interface.

**Memory Management Integration:**
```cpp
// Hardware transports: Pre-registered memory regions
Transport::mem_reg_info reg_mr_func_(void* buf, size_t size);

// Socket transport: Dummy registration (no hardware memory registration needed)
reg_mr_func_ = [](void*, size_t) -> Transport::mem_reg_info {
    return Transport::mem_reg_info(nullptr, 0);
};
```

**Routing Information:**
Socket transport uses a simple addressing scheme with IPv4 address and UDP port, embedded in eRPC's generic routing structure:
```cpp
struct socket_routing_info_t {
    uint32_t ipv4_addr;  // Network byte order
    uint16_t udp_port;   // Host byte order  
    uint16_t padding;    // Alignment
};
```

## How It Works

### Packet Reception Flow
```
Kernel UDP Stack → recvfrom() → rx_thread → malloc + copy → rx_queue → rx_burst() → eRPC RX Ring
```

The socket transport's reception model differs fundamentally from hardware transports:

**Socket Transport (Kernel-mediated):**
1. **rx_thread_func()**: Dedicated thread blocks on `recvfrom()` system call
2. **Memory Allocation**: Each received packet requires `malloc()` and `memcpy()` from kernel buffer
3. **Queue Buffering**: Thread-safe queue bridges asynchronous reception with eRPC's synchronous processing
4. **Ring Integration**: `rx_burst()` transfers queued packets directly into eRPC's RX ring

**Hardware Transport (Kernel-bypass):**
1. **Polling Loop**: User-space thread continuously polls hardware receive queues
2. **Pre-registered Memory**: Packets arrive directly in pre-allocated, hardware-mapped memory regions
3. **Zero-copy**: No memory allocation or copying required
4. **Direct Ring Access**: Hardware writes packets directly to RX ring buffers

### Transmission Flow
```
eRPC TX → tx_burst() → extract from MsgBuffer → sendto() → Kernel UDP Stack → Network
```

**Key Differences:**
- **Socket**: Single `sendto()` system call per packet with kernel handling
- **Hardware**: Direct memory writes to hardware queues with batched doorbell rings

### Memory Model Implications

**Socket Transport Memory Copies:**
```cpp
// RX: Kernel buffer → malloc'd buffer → eRPC ring
uint8_t *pkt_copy = malloc(bytes_received);
memcpy(pkt_copy, kernel_buffer, bytes_received);
rx_ring_[index] = pkt_copy;

// TX: eRPC buffer → kernel buffer → network
sendto(socket_fd_, pkt_buf, pkt_size, MSG_DONTWAIT, ...);
```

**Hardware Transport Zero-Copy:**
```cpp
// RX: Hardware DMA → pre-registered memory → eRPC ring (no copies)
// TX: eRPC buffer → hardware queue → network (no copies)
```

## When to Use Socket Transport

**Ideal Use Cases:**
- **Development and Testing**: Rapid prototyping without specialized hardware
- **Cloud Deployments**: Standard VM instances without SR-IOV or DPDK support
- **Heterogeneous Environments**: Mixed hardware where not all nodes have high-performance NICs
- **Small-Scale Applications**: Workloads where portability trumps maximum performance

**Avoid When:**
- **High-Frequency Trading**: Sub-microsecond latency requirements
- **HPC Workloads**: Maximum bandwidth and minimum CPU overhead needed
- **Large-Scale Data Processing**: High packet rates (>1M pps) required

## Configuration and Usage

```bash
# Enable socket transport during build
cmake -DERPC_FAKE=ON ...

# Runtime selection (automatic when TransportType::kFake specified)
# Transport constants
static constexpr size_t kMTU = 1024;           // Matches standard Ethernet
static constexpr size_t kPostlist = 16;        // Batch size for rx_burst()
static constexpr size_t kMaxDataPerPkt = 
    (kMTU - sizeof(pkthdr_t));                 // Payload after eRPC header
```

## Performance Expectations

**Typical Performance (compared to InfiniBand/DPDK):**
- **Latency**: 10-50x higher (microseconds vs sub-microsecond)
- **Throughput**: 10-100x lower (limited by kernel networking stack)
- **CPU Efficiency**: Higher CPU overhead due to system calls and memory copies
- **Jitter**: Less predictable due to kernel scheduling and interrupts

**Benefits:**
- **Universal Compatibility**: Runs on any Linux system
- **Easy Debugging**: Standard network tools (tcpdump, netstat, ss) work
- **No Special Privileges**: No need for hugepages or root access
- **Firewall Friendly**: Standard UDP traffic, easily configured