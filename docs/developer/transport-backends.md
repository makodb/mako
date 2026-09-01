# Transport Layer

Mako uses a single RPC transport: **srpc/rpc**, the portable TCP/IP RPC provided
by the in-tree srpc library (`src/srpc/`). It is implemented by the concrete
`SrpcRpcBackend` class in `src/mako/lib/srpc_rpc_backend.{h,cc}`.

There is no transport abstraction and no transport selection. The former
`TransportBackend` interface, its second implementation (the eRPC/RDMA backend),
and the `MAKO_TRANSPORT` environment variable were all removed: a
one-implementation interface only added a virtual dispatch on the RPC hot path
and a runtime switch that could not select anything.

## Characteristics

| Property | srpc/rpc |
|----------|---------|
| **Transport** | TCP/IP sockets (event-driven) |
| **Latency** | ~10-50 μs round-trip |
| **Hardware** | Standard Ethernet, any platform |
| **Debugging** | Works with standard tools (`tcpdump`, Wireshark) |

No special hardware or configuration is required:

```bash
./build/dbtest config/tpcc.yml
```

## Architecture

`FastTransport` (`src/mako/lib/fasttransport.{h,cc}`) owns an `SrpcRpcBackend`
directly and forwards `Transport`'s virtual API to it.

Worker threads in `src/mako/lib/server.cc` never see the backend type: the
backend hands each in-flight request to the helper queue as an opaque token and
the worker accesses it through the `TransportRequestHandle` interface
(`src/mako/lib/transport_request_handle.h`):

```cpp
class TransportRequestHandle {
    virtual uint8_t GetRequestType() const = 0;
    virtual char* GetRequestBuffer() = 0;
    virtual char* GetResponseBuffer() = 0;
    virtual void* GetOpaqueHandle() = 0;
    virtual void EnqueueResponse(size_t msg_size) = 0;
};
```

`SrpcRequestHandle` is its only implementation.

## Reliability features

The srpc/rpc client/server carries the reliability machinery used in production
deployments:

- **Connection state machine** — NEW, CONNECTING, CONNECTED, DISCONNECTING,
  DISCONNECTED, FAILED
- **Automatic reconnection** — configurable retry with exponential backoff and
  jitter
- **Circuit breaker** — fail fast instead of cascading
- **Request buffering** — queue during disconnection, replay after reconnect
- **Timeouts and idempotent retry**
- **Heartbeat / TCP keepalive**
- **Graceful shutdown** — drain in-flight requests before stopping
- **Observability** — connection metrics, event callbacks, structured errors

## Testing

```bash
# Focused srpc/rpc transport coverage (real network I/O)
ctest -R test_transport_integration

# Full unit-test suite
./ci/ci.sh srpcTests

# End-to-end suites
./ci/ci.sh simpleTransaction
./ci/ci.sh shardNoReplication
```

## Troubleshooting

**"Failed to connect to address"**
- Verify the target host/port is reachable and the firewall allows TCP.

**"Address already in use"**
- Wait for `TIME_WAIT` to expire, pick different ports, or kill lingering
  processes (`killall -9 dbtest`).
