# Transport Backend

This document describes Mako's RPC transport.

## Overview

Mako uses the **rrr/rpc** transport: portable TCP/IP-based RPC provided by the
in-tree rrr library (`src/rrr/`). It is accessed through the `TransportBackend`
abstraction, which currently has a single implementation, `RrrRpcBackend`. The
abstraction keeps worker code transport-agnostic, so additional backends could
be added later, but rrr/rpc is the only backend today.

## Characteristics

| Property | rrr/rpc |
|----------|---------|
| **Transport** | TCP/IP sockets (event-driven, epoll) |
| **Latency** | ~10-50 μs round-trip |
| **Hardware** | Standard Ethernet, any platform |
| **Debugging** | Works with standard tools (tcpdump, wireshark) |

No special hardware or configuration is required; running `dbtest` uses rrr/rpc
by default:

```bash
./build/dbtest config/tpcc.yml
```

## Architecture

Worker threads in `src/mako/lib/server.cc` use only the abstract
`TransportRequestHandle` / `TransportBackend` interfaces:

```cpp
class TransportRequestHandle {
    virtual uint8_t GetRequestType() const = 0;
    virtual char* GetRequestBuffer() = 0;
    virtual char* GetResponseBuffer() = 0;
    virtual void* GetOpaqueHandle() = 0;
    virtual void EnqueueResponse(size_t msg_size) = 0;
};
```

The rrr/rpc implementation (`RrrRpcBackend`, `RrrRequestHandle`) lives in
`src/mako/lib/rrr_rpc_backend.{h,cc}` and uses standard TCP/IP sockets via the
rrr library.

## Reliability Features

The rrr/rpc backend includes reliability features for production deployments:

- **Connection state machine** — tracks lifecycle (NEW, CONNECTING, CONNECTED,
  DISCONNECTING, DISCONNECTED, FAILED)
- **Automatic reconnection** — configurable retry with exponential backoff and jitter
- **Circuit breaker** — fail-fast to prevent cascade failures
- **Request buffering** — queue requests during disconnection, replay after reconnect
- **Timeouts and idempotent retry** — per-request timeout with safe retry
- **Heartbeat / TCP keepalive** — liveness detection at both app and OS level
- **Graceful shutdown** — drain pending requests before stopping
- **Observability** — connection metrics, event callbacks, structured errors

For details, see [RPC Reliability Features](rpc_reliability.md).

## Troubleshooting

**"Failed to connect to address"**
- Verify the target host/port is reachable and the firewall allows TCP.

**"Address already in use"**
- Wait for `TIME_WAIT` to expire, use different ports, or kill lingering
  processes (`killall -9 dbtest`).

## References

- rrr/rpc library: `src/rrr/` (internal)
- Transport backend interface: `src/mako/lib/transport_backend.h`
- Request handle interface: `src/mako/lib/transport_request_handle.h`
- Example configurations: `config/mako_*.yml`
