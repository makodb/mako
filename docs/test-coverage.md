# Rust and generated C++ test coverage

The canonical implementation is Rust. Cargo compiles `src/lib.rs`, whose module
declarations point to the production files in `base/`, `misc/`, `reactor/` and
`rpc/`. CMake builds their generated C++ equivalents. Both use the shared native
C/assembly kernels.

The September 2026 audit found 62 of 83 C++ files under `tests/` absent from CMake.
That measured build registration, not Rust coverage or the percentage of broken
tests. Using the existing C++ build, 31 omitted files passed syntax checks and 31
failed. Some required upstream Mako headers; others had obsolete signatures or
fixtures. Several nominal tests were benchmarks.

## Source inventory

[`tests/test-inventory.json`](../tests/test-inventory.json) accounts for every
`tests/**/*.cc` file. It lists built runtime tests, documentation tests, support
sources and benchmarks. Excluded groups record their purpose, exact Rust test
references and retained C++ replacements where applicable. The inventory keeps
the historical source files available for review; an exclusion does not mean a
successful C++ compile or execution.

Seventeen additional C++ executables now cover error names, circuit-breaker and
connection-state behavior, framing, heartbeat and reconnect policy, queues,
completion tracking, idempotency, clocks, serialization, callbacks, shutdown,
restart detection, and C++ pollable/service adapters. The callback fixture uses
the current `std::string_view` signature; the AnyMessage fixture transfers its
move-only value explicitly.

The resulting inventory assigns 38 C++ source files to targets and records 45
exclusions. It does not claim every old C++ assertion has an identical Rust
assertion. The named Rust cases establish the behavioral coverage described
below. Original long-duration crash/partition schedules and disabled reconnect
or buffering tests are not counted as current acceptance.

## Behavioral mapping

The JSON inventory gives individual filenames and executable Rust case names.
This table explains the decisions behind those mappings.

| Historical C++ behavior | Canonical Rust acceptance | Generated C++ acceptance |
| --- | --- | --- |
| Error categories, state transitions, circuit-breaker probes | `errors_rust`, `connection_state_rust`, `circuit_breaker_rust`, `manager_concurrency_rust`; live request gating in `rpc_reliability_rust` | Restored error, state and breaker suites |
| Pool minimum population, reuse, selection and health/idle pruning | New `client_pool_rust` uses real TCP servers, verifies reused client identities, per-address floors and failed population; `load_balancer_rust` checks selection algorithms | Existing real transport and metrics suites; pool policy assertions belong to Rust |
| Server shutdown, pending requests, hooks and instance IDs | New `server_lifecycle_rust` checks hook order and panic isolation, drain timeout, a real in-flight TCP handler, and instance IDs | Restored shutdown and restart suites |
| Client response ordering, unknown/duplicate XIDs, errors and restart IDs | New `rpc_protocol_rust` checks out-of-order completion and unchanged completed futures; `client_runtime_ownership_rust` checks replacement-binding isolation | Transport matrix, runtime parity and restart suites |
| Server request decoding, malformed/unknown RPCs, heartbeat replies | New `rpc_protocol_rust` checks emitted response headers and balanced pending counts; existing in-memory round trips exercise actual service dispatch | Transport matrix and service adapter suites |
| Deferred replies and abandoned requests | New `rpc_reliability_rust` checks once-only reply, inline async callback, cleanup, request release and close completion | Transport matrix; historical benchmark-service fixture retained for reference |
| Non-idempotent retry suppression and total retry budget | New `rpc_reliability_rust`; existing `client_retry_rust`, `request_options_rust` and `timeout_conformance_rust` | Timeout race and runtime parity suites |
| Buffering, owned replay, TTL, overflow and callback reentry | `client_replay_rust`, `request_queue_rust`, `manager_concurrency_rust` | Client replay and restored request queue suites |
| Channel forwarding, factory/listener lifecycle, frame ownership, close and faults | `channel_rust`, `inmemory_channel_rust`, `fiber_channel_rust`, `tcp_channel_rust`, client/server concurrency and ownership suites | TCP, transport matrix, fiber, runtime parity and adapter suites |
| Keepalive, validation and live counters | `tcp_keepalive_rust` checks OS socket options; `client_surface_rust`, pool and reconnect tests check live behavior | TCP and metrics suites |
| Archives, containers, envelopes, erased payloads and descriptor I/O | `serializable_rust`, `serializable_envelope_rust`, `any_message_rust`, `fd_io_rust`, wire goldens and property tests | Serialization parity plus restored marshal and AnyMessage suites |
| Mako/RocksDB logs and application-specific payloads | Outside standalone SRPC; five fixtures require upstream application headers | Explicit upstream exclusions |
| Removed chaos controller and old copy/throughput microbenchmarks | Removed API is not advertised; actual channel fault injection has native tests | Explicit retired-API or historical-benchmark exclusions |

Fault-injection fixtures operate at channel boundaries and call canonical dispatch
code. They do not replace the reactor, clock, native I/O or serialization logic.
Separate TCP tests exercise the real transports and scheduler.

## Checks

```sh
cargo test --locked --workspace --all-targets
cargo test --locked --workspace --doc
python3 scripts/check_test_inventory.py
python3 scripts/tests/test_test_inventory.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build -L srpc --output-on-failure
```

CMake exports its actual source-to-target assignments and CTest registrations to
`build/srpc-test-targets.txt`. Configuration checks those against the inventory.
The source gate checks source dispositions and Rust references; CTest also checks
the configured target report. Adding a C++ source triggers reconfiguration and
fails until it has an explicit disposition. Removing a target, excluding an
active test from the default build, losing CTest registration, duplicating a
disposition, or deleting a referenced Rust test also fails the checks.

`BUILD_TESTING=ON` requires GoogleTest. Missing GoogleTest is a configuration
error. To configure without GoogleTest, explicitly set `-DBUILD_TESTING=OFF`.
Benchmarks remain outside correctness acceptance.

These checks prevent missing registration from looking like passing coverage.
They cannot prove semantic equivalence between arbitrary Rust and C++ tests.
Review changes to exclusions and replacement references as coverage changes.
