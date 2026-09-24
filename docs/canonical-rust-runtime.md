# Canonical Rust runtime and migration notes

SRPC's Rust files own its runtime and protocol behavior. Cargo compiles those sources directly, and
rusty-cpp generates the C++ named-module providers from the same files. The
[translation audit](translation-parity-audit.md) records the earlier substitutes that prompted this
repair. This document describes the changed implementation and public contracts. The historical
validation below covers the earlier runtime repair based on SRPC `9bba8a7`, including the subsequent
ThreadSanitizer and UndefinedBehaviorSanitizer passes recorded with `624c083` on 2026-09-11.
Current facade-removal acceptance is tracked separately in
[Rust lane independence](dev/facade-and-runtime-remaining.md).

## Implementation owners

| Behavior | Canonical owner |
| --- | --- |
| Poll worker commands, jobs, fibers, timer/event waits, stackless task scheduling, and wake admission | [reactor/reactor.rs](../reactor/reactor.rs) |
| Epoll interest flags, registration/error policy, and readiness dispatch | [reactor/epoll_wrapper.rs](../reactor/epoll_wrapper.rs) |
| Public fiber helpers and promise/future event behavior | [reactor/fiber.rs](../reactor/fiber.rs), [reactor/future.rs](../reactor/future.rs) |
| Callback delivery converted into a suspended fiber receive | [rpc/fiber_channel.rs](../rpc/fiber_channel.rs) |
| TCP connect policy, send buffering, framing, close, and accept policy | [rpc/tcp_channel.rs](../rpc/tcp_channel.rs) |
| RPC dispatch, request/reply ownership, completion, and teardown | [rpc/server.rs](../rpc/server.rs), [rpc/client.rs](../rpc/client.rs) |
| Connection state, heartbeat, circuit breaker, and offline queue policy | [rpc/connection_state.rs](../rpc/connection_state.rs), [rpc/heartbeat.rs](../rpc/heartbeat.rs), [rpc/circuit_breaker.rs](../rpc/circuit_breaker.rs), [rpc/request_queue.rs](../rpc/request_queue.rs) |
| Archives, serialization loops, payload holders, registry dispatch, and unpacking | [misc/serializable.rs](../misc/serializable.rs), [misc/any_message.rs](../misc/any_message.rs), [misc/serializable_envelope.rs](../misc/serializable_envelope.rs) |
| Port search, I/O retry policy, and random range/selection policy | [rpc/utils.rs](../rpc/utils.rs), [misc/serializable.rs](../misc/serializable.rs), [misc/rand.rs](../misc/rand.rs) |

Client, server, TCP, fiber helpers, and fiber futures use `crate::reactor` types and functions. Explicit
canonical dependency anchors preserve the generated imports where the compiler needs them. There is
no separate `rusty-rustc::srpc` scheduler, sleep recorder, yield counter, event implementation, RNG,
archive implementation, or payload holder.

The reactor and its fiber/event state belong to one thread in both languages. Real native context
switches suspend stackful handlers. The poll worker also pumps canonical stackless tasks. A foreign
wake publishes through synchronized ingress and tickets; the owner thread polls the task and runs its
completion. A retained waker owns its callback. Reactor teardown closes admission, so a later wake
cannot dereference a destroyed context or resume a retired task.

`FiberChannel` callbacks own a synchronized frame queue and closed flag. They do not retain a pointer
to the wrapper or mutate its reactor event from another thread. Each receive creates its waiter on the
owner thread, whose readiness predicate checks the shared state. Only one fiber may receive through a
wrapper at a time.

## Permitted adapters and native kernels

The canonical Cargo package has no production Rust dependencies. It uses Rust std
and the C/assembly kernel described below. The `rusty-rustc` and `rusty-cpp-markers`
packages have been removed. Standard Rust futures, contexts and wakers are polled
by the canonical reactor; generated C++ uses the compiler's coroutine runtime.
Scheduling and wake admission remain in canonical Rust.

C++ type mappings, native declarations, module preambles and serialization
forwarders remain build inputs for the generated C++ lane. Cargo does not compile
or link the C++ runtime. The
[Cargo independence check](../scripts/check_rust_independence.py) builds and tests
a copied tree containing only Cargo inputs, canonical Rust and native C/assembly
sources, with no C++ tool on its restricted tool path.

The repaired C++ `Arc` adapter follows Rust's ownership contract: `get_mut` requires one strong
owner and no `Weak` owners. If `new_cyclic` construction fails, it releases its temporary ownership;
any escaped `Weak` remains expired and keeps the allocation alive until its last owner drops. `Weak::weak_count`
returns zero before cyclic construction publishes a strong owner and after all strong owners drop,
matching Rust.

The [canonical AST audit](../scripts/rust_source_audit.py) checks functions against
the reviewed
[constant-function inventory](../scripts/canonical-constant-functions.json). Constant results and empty
bodies require a recorded behavioral reason and exact signature/body hash. This covers legitimate
cases such as `NeverEvent`, a listener with no output buffer, and a consumed `Arc` released by
ordinary destruction. It rejects new default-returning runtime functions and production
`todo!`/`unimplemented!` calls, including conditional calls. Runtime parity tests still carry the burden
of checking behavior that cannot be inferred from a function body alone.

[build.rs](../build.rs) and [CMakeLists.txt](../CMakeLists.txt) consume the same
[native source manifest](../scripts/native-kernel-sources.txt). It lists nine C sources:

- `base/srpc_base.c` provides basic platform operations.
- `misc/srpc_timing.c`, `misc/srpc_rand.c`, and `misc/srpc_io.c` provide clock/calendar fields,
  entropy, individual reads/writes, and errno access.
- `rpc/srpc_net.c`, `rpc/srpc_connect.c`, and `rpc/srpc_server.c` provide individual network and
  server platform operations.
- `reactor/srpc_fiber.c` owns native stack/context resources and thread-local active context.
- `reactor/srpc_epoll.c` performs epoll syscalls and copies platform event records into the fixed ABI.

The manifest selects `reactor/fiber_context_x86_64.S` or `reactor/fiber_context_aarch64.S` for the target
architecture. The Rust build requires Linux, a supported architecture, a C compiler, and an archiver.
Both build paths use the same fiber reuse configuration.

OS layouts, errno capture, resource allocation, and context switching stay native. SRPC retry loops,
port search, connect timeout/self-connect decisions, and epoll dispatch stay in canonical Rust. The
small native event-record copy is ABI marshalling, with its layout declared in
[reactor/srpc_epoll.h](../reactor/srpc_epoll.h). The old `epoll_platform_linux.cc` provider has been
removed. No production inline-Rust DSL carrier remains.

[check_native_kernels.py](../scripts/check_native_kernels.py) checks the shared compilation manifest,
its consumers, and the reviewed [native source/header inventory](../scripts/native-kernels.json).
It rejects added handwritten C++ implementation carriers under the canonical directories. Native
body/header changes require review; the inventory is not an automatic approval mechanism.

## Public API migration

`Service` requires `Send + Sync`, and `__dispatch__` takes `&self`. Registration still takes `&mut self`
before publication. Generated C++ dispatch methods and service handler wrappers are const-callable,
so custom overrides must add matching `const` qualifiers. Keep request-local mutable state in locals;
shared service state needs atomics or locks. A suspended handler can coexist with another request to
the same service without holding an exclusive borrow of the whole service.
`Server::for_each_service` likewise passes `&dyn Service`, replacing a mutable boxed-service borrow.
Configure a service before registration; synchronize later updates through the service's own state.

`ChannelConnectionBase` is a safe trait with `Send + Sync` bounds. Its `send_frame`, `flush`, and `close`
methods take `&self`; generated C++ overrides are correspondingly const. Sending a raw `ChannelFrame`
remains unsafe in Rust because its payload must stay readable and unchanged for the synchronous call.
Callback installation still takes a mutable receiver through the trait. `ChannelFactoryBase` requires
`Send`, and `ChannelListenerBase` requires `Send + Sync` alongside its documented unsafe ownership
contract.

Client and server connection slots hold shared ownership of their channel proxies. Send and close
clone that ownership under the slot lock, release the lock, and then call the channel. An in-flight
operation keeps its channel alive through concurrent slot removal, and callback reentry does not need
to reacquire a lock held by its caller.

Client channel replacement and callback admission share a lifecycle lock. Each installed callback
belongs to one binding generation. A retired channel cannot complete a replacement's requests or
change its connection state, heartbeat, or server identity. Close detaches all pending completion
owners before notifying callbacks, so a callback that reconnects cannot have its new requests drained
by the old close. User writers and channel, factory, and completion callbacks run outside that lock.

An overlapping `reconnect` returns `CLIENT_ERR_BUSY` while another attempt owns the operation.
This replaces the previous implicit wait, which could deadlock when a reconnect callback reentered
the client. A successful attempt releases ownership before its connected callback runs. If that
callback replaces the binding, the superseded attempt reports `CLIENT_ERR_CANCELED`.

RPC `Future` synchronizes completion state and reply bytes. `Future::get_reply()` now returns a
`MutexGuard<ReplyBuffer>`, replacing the former `RefMut` guard. `deserialize_from` accepts that mutex
guard. Existing expressions that pass the temporary directly keep their shape:

```rust
srpc::client::deserialize_from(future.get_reply(), &mut value);
```

```cpp
srpc::deserialize_from(future->get_reply(), value);
```

Update explicitly named guard types, or use type inference. A named guard remains an owner of the
reply lock and must be moved into `deserialize_from` when passed by value. Finish decoding and release
the guard before invoking user callbacks or waiting for another operation that needs the same reply.

Transport callbacks use `Fn + Send + Sync`. Shared future callbacks and connection-state callbacks
also require `Send + Sync`; queued, asynchronous reply, restart, heartbeat, and completion callbacks
that are invoked mutably require `Send`. Their owning mutexes serialize mutation. Captures that cross
threads must use thread-safe ownership rather than `Rc<Cell<_>>` or `Rc<RefCell<_>>`. Owner-thread
fiber/task closures can still use local `Rc` state where their contracts do not require `Send`.

Reliability managers synchronize shared configuration and transitions. The canonical
[SharedCell](../base/threading.rs) returns a cloned snapshot after releasing its mutex. Separate
transition locks protect operations such as admitting exactly one half-open circuit-breaker probe or
firing one heartbeat timeout. Callbacks run after releasing transition locks. Heartbeat callback
ownership also permits a callback to reset the manager or replace itself.

### Retired compatibility methods and ownership helpers

Some Rust-private methods were still emitted into the public C++ ABI. The following removals are
intentional source and ABI changes; callers must rebuild against the generated interfaces.

| Removed entry points | Migration and preserved behavior |
| --- | --- |
| `Client::set_valid` | This method did nothing. Use `connect`, `close`, and the connection-state queries to change or inspect the actual lifecycle. |
| `ClientConnection::fd`, `content_size`, `poll_mode`, `handle_read`, `handle_write` | These were constant-returning compatibility hooks after TCP polling moved to `TcpConnection`. The canonical transport still performs descriptor polling, buffering, reads, and writes through its `Pollable` implementation. RPC clients should use the channel/request APIs. |
| `ClientConnection::apply_keepalive_options` | The unused empty hook is removed. Keepalive configuration now reaches the real TCP socket through `Client::set_keepalive` and `ChannelConnectionBase::set_keepalive`, as described below. |
| `clientconn_fiber_channel_ptr`, `sconn_proxy_ptr` | Unlocked raw-pointer access to replaceable channel slots is removed. Connection operations retain shared ownership through each call. `clientconn_run_recv_loop` snapshots its channel owner; `clientconn_recv_job_entry` now receives an owned `Arc<Box<FiberChannel>>` explicitly. |
| `FiberChannel::on_inbound_frame`, `on_inbound_closed`, `signal_pending_recv`, `wait_for_signal` | Their raw-receiver callback/wait plumbing is replaced by owned queue/closed state and an owner-local waiter. Public `recv_frame`, `send_frame`, and `close` remain and take shared receivers. |
| `Job::rustc_job_ready`, `rustc_job_work`, and `PollableBase::rustc_*` forwarding methods | These existed for the removed Rust runtime substitute. Both lanes now invoke the canonical `Job` and `PollableBase` contracts directly. |

`replay_pending_requests` is retained and restored to a real replay loop. It is not one of the retired
hooks. The former zero-result implementation, empty client metrics, and unapplied keepalive settings
are repaired behaviors, with tests using actual channels and sockets.

The removed serialization `rusty_ext` wrappers were duplicate extension adapters. Sparse-integer wire
algorithms remain in canonical `misc/serializable.rs` and its `Serialize_`/`Deserialize_` dispatch.
Standard-container adapters forward to those canonical traits; removing the wrappers does not remove
payload serialization or move it into the defining integer type.

A custom `SerializableRegistryFactory` must return a payload with exclusive `Arc` ownership when
mutable loading begins. Retaining another strong owner or a `Weak` prevents that load. Rust already
rejects this access, and the repaired C++ adapter applies the same check. Dropping the retained `Weak`
allows loading once the payload has exactly one strong owner. The
[Rust factory regression](../tests/serialization_weak_factory_rust.rs) and
[C++ serialization fixture](../tests/serialization_parity_test.cc) check rejection with a retained
`Weak` and successful loading after releasing it.

## Restored client behavior

`Client::metrics()` and its connections share one `Arc<ConnectionMetrics>`. The client
keeps references valid through close and reconnect. Pool health checks and selection by
in-flight requests now read live counters. Automatic completion still does not record
latency samples, so latency-based selection requires explicit instrumentation.

Disconnected requests retain their encoded RPC bodies and futures. Successful reconnect
replays unexpired bodies FIFO through the active channel without rerunning user writers.
Expiry, overflow, and teardown resolve the original futures with their respective errors.
Replay transfers ownership into the pending-reply table before sending, including for
inline replies. Queue callbacks run after releasing the queue lock and can reenter it.
The legacy enabled queue with `max_size = 0` retains one request under `DROP_OLDEST`,
evicting it when the next request arrives. `DROP_NEWEST` and `FAIL_FAST` reject admission
at zero capacity. Use `RequestQueueConfig::disabled()` to disable buffering explicitly.

`Client::set_keepalive` applies the configuration through the channel capability and
stages it for future connections. Canonical TCP policy sets Linux `SO_KEEPALIVE`,
`TCP_KEEPIDLE`, `TCP_KEEPINTVL`, and `TCP_KEEPCNT`; native leaves perform individual
socket operations. Disabling keepalive leaves the tuning values unchanged. Non-TCP
channels report that this capability is unsupported.

`PollThread::get_remove_count()` counts accepted `remove_fd` commands, including commands
for an fd that was not registered. Rejected commands after shutdown do not increment it.
This is a synchronized request counter, not a count of successful epoll removals.

TCP connection and listener registrations acquire a shared socket owner before enqueue and
retain it through epoll removal. Logical close immediately clears the transport's socket slot
and shuts down the socket; the registration prevents physical descriptor reuse until unregister.
The worker rejects already-closed queued registrations and cancels pending removals when retiring
an old registration. Rust and C++ regressions check these ownership boundaries and require actual
epoll readiness and frame delivery after reuse. TCP write-interest updates stay on the connection's
atomic pending flag so a delayed raw-descriptor command cannot target a replacement socket.

Close notification has its own exactly-once latch. A flush failure can mark a connection closed
before teardown; the later close still delivers its callback and permits callback reentry.

## Checks and verification record

The Rust runtime tests link the actual native kernels. They exercise timer suspension, stackful
handler overlap, owner-thread wake completion, real TCP success/failure, callback reentry, concurrent
close, and retained wake lifetime. Useful focused tests include `rpc_runtime_rust`,
`stackless_wake_pollthread_rust`, `task_waker_rust`, `fiber_channel_rust`, `epoll_wrapper_rust`,
`server_concurrency_rust`, `manager_concurrency_rust`, `client_replay_rust`,
`client_runtime_ownership_rust`, `tcp_keepalive_rust`, and `pollthread_remove_count_rust`. The client concurrency tests cover cloned
channel ownership and synchronized future state. Serialization tests require recovered payloads and
successful factory/container operations.

`*_at` policy helpers accept explicit timestamps for boundary tests while production entry points
obtain native time and call the same algorithms. Ordinary runtime tests do not override the process
clock. Any isolated native fault injection must have a stated contract and accompanying real-kernel
coverage.

The isolated `rand_rust`, `request_options_rust`, and `reconnect_policy_rust` policy fixtures supply
deterministic raw entropy to check exact range, jitter, and draw-count boundaries. Their symbol
overrides are confined to those test executables. `rand_native_rust`, the native random smoke test,
and ordinary runtime integration tests exercise the production entropy kernel.

The [paired runtime driver](../scripts/check_runtime_parity.py) runs
[the Rust fixture](../tests/runtime_parity_rust.rs) and
[the C++ fixture](../tests/runtime_parity_test.cc). Both use real runtime and TCP paths. It checks an
independently specified transcript for timer order, actual suspension/deadline, foreign wake admission,
owner-thread completion, successful reply value, and missing-RPC error. It rejects missing/duplicate
records, wrong values or types, process failures, and timeouts. Its
[negative controls](../scripts/tests/test_runtime_parity.py) also reject matching but incorrect results.
This bounded comparison complements broader runtime suites; it does not establish equivalence for
all inputs or interleavings.

Run the relevant checks from the repository root:

```sh
cargo test --locked --workspace --all-targets
cargo test --locked --workspace --doc
cargo clippy --locked --workspace --all-targets -- -D warnings
python3 scripts/rust_source_audit.py
python3 scripts/check_rust_independence.py
python3 scripts/check_native_kernels.py
python3 scripts/check_native_abi_bindings.py
python3 scripts/tests/test_rust_source_audit.py
python3 scripts/tests/test_native_kernels.py
python3 scripts/tests/test_native_abi_bindings.py
python3 scripts/tests/test_rust_independence.py
python3 scripts/tests/test_runtime_parity.py
cmake --build build --parallel 4
ctest --test-dir build -N -L srpc
ctest --test-dir build -L srpc --output-on-failure
```

The normal build and tests passed on Linux x86_64 with Clang 22, libc++, and Release C++ settings.
The validated SRPC working tree is based on `9bba8a7`; the compiler is the clean release build of
`3e1d95059839e4bf1968891047ff1563a2f08c17`. The results below are historical acceptance for that tree and toolchain. Current
facade-removal acceptance is tracked in
[facade-and-runtime-remaining.md](dev/facade-and-runtime-remaining.md).

| Check | Recorded result |
| --- | --- |
| Rust workspace and documentation tests | 261 passed: 258 workspace tests and 3 documentation tests. One Rust layout test remains ignored because generated C++ layouts are checked by the C++ ABI oracle. Clippy passed with warnings denied. |
| Compiler tests | 2,444 passed, one ignored. |
| Fresh generation and compilation | 38 generated files: 37 canonical providers plus the umbrella module, zero generation errors, and zero handwritten implementation slots. Normal CMake ALL and independent provider compilation passed. |
| ABI and importer checks | Both the production-archive and explicit-provider importer lanes passed. The measured inventory contains 2,045 unique strong symbols and 2,298 raw entries; all 145 ABI negative controls passed. |
| Configured SRPC CTests | All 26 passed in one serial run, including the 17 runtime checks and paired Rust/C++ comparison. |
| Additional runtime checks | All 25 metrics tests passed; retry and reconnect each passed 20 repetitions. The seven Arc ownership cases and all six serialization cases passed, including rejection with a retained Weak and successful loading after its release. |
| Documentation examples | All seven tagged C++ snippets compiled; all 63 C++ fences passed lint. |
| AddressSanitizer and LeakSanitizer | Build and all 17 runtime checks passed, script exit 0. Test time: 22.75 seconds. No sanitizer error report appeared in the direct-fixture output; stack warnings and leak suppressions apply as described below. |
| ThreadSanitizer | After descriptor-lifetime and test-helper repairs, all 17 runtime checks passed on 2026-09-11. The retained console and CTest logs record that pass; commit `624c083` records zero data races. This supersedes the initial 12-of-17 result. |
| UndefinedBehaviorSanitizer | All 17 runtime checks passed on 2026-09-11. The retained console log records the pass; commit `624c083` records zero findings. |

The follow-up repairs and verification are recorded in `624c083`, an ancestor ten commits before
the facade-removal starting tree `c591960`. TCP registrations retain descriptor owners through epoll
removal. Test pollables use atomic mode fields, publish descriptor retirement with release/acquire
ordering, and join workers before caller-side close. The commit records an initial ten race reports,
followed by the passing ThreadSanitizer and UndefinedBehaviorSanitizer runs.

The retained machine-local console logs are
`/var/tmp/srpc-san-thread-2071411.log` and `/var/tmp/srpc-san-undefined-2071411.log`.
They end with all 17 tests passing and the corresponding sanitizer battery success message.
`build-san-thread/Testing/Temporary/LastTest.log`, dated 2026-09-11, also records all 17 passes.
The older `LastTestsFailed.log`, dated 2026-09-09, still lists `test_reactor`,
`test_rpc_pollthread_proxy_storage`, `test_rpc_transport_matrix`, `test_rpc_metrics` and
`test_native_policy`. That stale summary does not describe the later run. The original warning
stacks were not found in the preserved SRPC logs checked during the facade-removal investigation;
the ten-report count comes from the commit's verification record.

The sanitizer script uses the five existing fiber-allocation LeakSanitizer suppressions in
[scripts/lsan_suppressions.txt](../scripts/lsan_suppressions.txt). The address run printed suppression
summaries in nine direct fixtures, totaling 272 allocations and 54,400 bytes in the visible output.
These matched the `fiber_install_task` and `reactor_get_or_create_fiber_impl` patterns. A passing run
does not establish that suppressed allocation paths are leak-free.

Ten direct fixtures also printed ASan's warning that it was ignoring `__asan_handle_no_return`,
followed by its warning that false-positive reports may follow. The native context switch has no
sanitizer fiber-switch hooks, so this run does not fully validate custom fiber stacks. No
AddressSanitizer or LeakSanitizer error was reported in those logs.

The paired driver rejects nonzero child exits but captures successful child stderr. Its C++ fixture
also runs directly in the battery with visible diagnostics. The paired Rust process uses ordinary
Cargo and is not instrumented by the C++ sanitizer configuration.

The paired driver checks independently specified timer, wake-owner, and RPC outcomes for its bounded
fixtures. Retry tests also observe detached worker completion by requiring a retained Weak to expire
after user ownership is released. These checks cover the exercised operations and schedules; they do
not prove equivalence for every input, interleaving, platform, or shutdown path. These historical
sanitizer passes do not validate the current facade-removal changes; their acceptance remains tracked
in [Rust lane independence](dev/facade-and-runtime-remaining.md). The gitlink and executable gate
files remain the authoritative pin and inventory definitions.
