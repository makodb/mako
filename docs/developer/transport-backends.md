# Native transport

Production Mako uses the Rust `src/srpc` crate directly through Cargo.
SRPC provides TCP and in-memory transports. Rust owns service dispatch,
framing, request identity, deadlines, reconnection and connection pooling.
C platform bindings and architecture assembly implement OS access and fibers.

The retired C++ application transports, DPDK/RDMA application adapters and
Rust-to-C++ SRPC provider are not selectable backends. There is no transport
fallback to legacy files. See [the SRPC book](../../src/srpc/docs/srpc-book.md)
and [native development](development.md).
