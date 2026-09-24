# sRPC

sRPC is a Rust RPC library with TCP and in-memory transports, an epoll reactor,
stackful fibers, async tasks, timeouts, retries, reconnection and connection
pooling. It runs on Rust std and a small C/assembly kernel.

The **[sRPC book](docs/srpc-book.md)** starts with a working Rust service and
client, then covers the runtime, APIs, configuration and troubleshooting.

## Build and test

sRPC targets Linux on x86_64 and aarch64. Install a stable Rust toolchain,
a C compiler and an archiver, then run:

```sh
git clone https://github.com/stonysystems/srpc
cd srpc
cargo test --locked --workspace --all-targets
cargo test --locked --workspace --doc
```

Cargo needs no submodules or C++ toolchain. See the book for
[setup and dependencies](docs/srpc-book.md#building-and-testing-srpc) and
[focused checks](docs/srpc-book.md#use-the-cargo-checks).
The [test coverage inventory](docs/test-coverage.md) maps historical C++ tests to
canonical Rust coverage and records which generated C++ suites run under CTest.

## Using sRPC

- [Rust service and client](docs/srpc-book.md#the-shape-of-a-service)
- [Timeouts, retries and reconnection](docs/srpc-book.md#11-reliability-features)
- [Rust API reference](docs/srpc-book.md#16-rust-api-and-verification)
- [Performance and benchmarks](docs/srpc-book.md#13-performance-tuning)
- [Verus verification](docs/srpc-book.md#checking-rust-contracts)

sRPC also generates a C++23 library from the same Rust sources. The
**[C++ companion](docs/srpc-cpp-book.md)** covers translation, CMake builds,
`.rpc` service generation and C++ APIs, including a complete
[service and client walkthrough](docs/srpc-cpp-book.md#3-c-service-and-client-walkthrough).
