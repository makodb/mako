# The Raft RPC path on the Rust lane

On the Rust lane (`MAKO_RAFT_LANE=rust`, the default since plan step T5),
every Raft RPC runs on the **Rust srpc runtime**. The Rust srpc client and
server send it, the Rust `PollThread` carries it, and Raft's own Rust message
types (generated into `rt/src/rpc.rs`) encode it. The C++ srpc runtime is not
on Raft's path. It still runs in the same process, because Mako and Paxos use
it.

This file follows one AppendEntries end to end: the leader sends it, the
follower answers, and the leader reads the reply. At every step it shows which
language runs and where the code is. It is wired and exercised today:
RaftLabTest 25/25 and the four Raft replication suites pass on this lane, a
cluster mixing Rust-lane and hybrid-lane replicas replicates correctly (12 of
12 runs), and the full performance sweep against the C++ baseline
(`docs/performance/raft-rust-9a361eccd`) completed all 624 runs.

Paths are under `src/deptran/raft/` unless marked. On this lane rustc
compiles the core (`src/`) and the runtime (`rt/src/`, the `raft-rt` crate)
into one archive, `libraft_rt.a`. **C++ kernel** means hand-written C++ in
`server.cc` that the Rust calls out to. Line numbers are as of this revision;
the function names are the durable reference.

---

## 1. Setup: the worker starts the Rust transport

```
RaftWorker (raft_worker.cc:352)                  C++ embedder
  │  raft_lane::Serve(server, bind_addr)
  ▼
raft_lane_rust.cc                                thin C++ bridge
  │  raft_transport_new, raft_transport_serve, raft_transport_add_peer
  ▼
RaftTransport::serve (rt/src/transport.rs)       Rust
     srpc::Server::new on the transport's own Rust PollThread, the Raft
     service registered through the generated rpc::register, admission
     closed until the worker says ready. bind_transport then publishes
     server → transport in a lock-free registry, so transport_of(server)
     can find it later.
```

After this the worker never touches the network for Raft again. The
heartbeat and election loops run as fibers on that Rust poll thread
(`raft_spawn_heartbeat_loop` and `raft_spawn_election_timer_fiber` in
`rt/src/seam.rs`).

## 2. Calling path: the leader sends AppendEntries

```
RaftWorker::Submit(log, len, par_id)             C++ embedder   raft_worker.cc:830
  │  the boundary that stays: Mako hands Raft bytes, not objects
  ▼
raft_server_start (export)                       one C ABI call
  ▼
RaftServerBase::Start                            Rust   src/server_h.rs:4669
  │  under mtx_: not leader → REJECTED with index = term = 0. Otherwise
  │  raft_command_clone_into (C++ KERNEL) copies the Command, and
  │  AppendLocal puts it in the log. RequestReplication() runs after the
  │  lock is released, so waking the heartbeat fiber never inverts its lock
  │  order.
  ▼
heartbeat_phase1_body                            Rust   src/server_cc.rs:1089
  │  per follower: pick the entries (a batch capped by entry count AND by
  │  bytes, MAKO_RAFT_APPEND_BATCH_MAX_BYTES, default 16 MiB)
  ▼
raft_phase1_send_append                          Rust   rt/src/seam.rs:359
  │  the seam kernel. The same symbol is C++ on the other lanes
  │  (server_seam_cpp.cc); on this lane raft-rt defines it.
  ▼
transport_of(server)                             Rust   rt/src/transport.rs:589
  ▼
RaftTransport::send_append_entries_with          Rust   rt/src/transport.rs:309
  │  peer() returns None when the network is disabled; callers already
  │  treat that as a lost RPC
  ▼
RaftProxy::append_entries_with_async             GENERATED   rt/src/rpc.rs:493
  │  writes the fixed fields, then calls back for the payload:
  │    raft_command_encode (C++ KERNEL, server.cc:1493) serialises the
  │    Command straight into the Rust request archive through an
  │    srpc::SinkBase adapter (EmitSink). The payload is never copied into
  │    an intermediate buffer.
  ▼
srpc Client, Rust PollThread                     srpc Rust   src/srpc/rpc/
  │  one resize and one memcpy per frame (tcpconn_append_frame)
  ▼
                                                 bytes on the wire
```

The send leaves an `AppendReply` (`Pending<AppendEntriesResponse>`,
`rt/src/transport.rs:73`) behind in the heartbeat round's
`rusty::RaftResponsePtr` slot. On this lane that 16-byte carrier holds a tag
and an `Arc` pointer, not a C++ `shared_ptr`.

## 3. Responding path: the follower answers

```
                                                 bytes on the wire
  ▼
srpc Server, Rust PollThread                     srpc Rust
  │  reads the frame, finds the handler rpc::register installed
  ▼
RaftRpcService::__dispatch__                     Rust   rt/src/service.rs:96
  ▼
rpc::dispatch                                    GENERATED   rt/src/rpc.rs:398
  │  AppendEntriesRequestRef::from_body (rpc.rs:175) borrows the payload
  │  out of the frame rather than copying it. A malformed frame gets
  │  reject_malformed_request, never a half-decoded request.
  ▼
RaftRpcService::append_entries                   Rust   rt/src/service.rs:115
  │  raft_command_from_bytes (C++ KERNEL, server.cc:439) rebuilds the
  │  janus::Command from the borrowed bytes; a payload it cannot parse is
  │  answered with error 22 (invalid argument)
  ▼
RaftServerBase::ServeAppendEntries               Rust   src/server_h.rs:4724
  │  the admission gate: IsDisconnected / IsRpcReady, with the unavailable
  │  reply written here
  ▼
OnAppendEntries → on_append_entries_body         Rust   src/server_h.rs:4790, :5453
  │  the Raft logic, under mtx_
  ▼
reply_with                                       GENERATED   rt/src/rpc.rs:455
  │  Ok  → code 0 plus the serialised fields
  │  Err → that code with no body, which the leader reads as a failed RPC
  ▼
                                                 bytes on the wire
```

## 4. Completion path: the leader reads the reply

```
srpc Rust client reply callback                  srpc Rust
  ▼
append_sink closure → decode_reply               Rust   rt/src/transport.rs:277, :81
  │  a non-zero code or a malformed body → Err, never a partial response
  ▼
AppendReply slot filled                          Rust
  ▼
heartbeat_phase2_body                            Rust   src/server_cc.rs:1501
  │  raft_append_response_read (rt/src/seam.rs) polls the slot and returns
  │  an AppendRespView {completed, status, term, last_log_index}. That is
  │  the same question AppendEntriesResponse::completed answers on the C++
  │  lanes.
  ▼
commit index advances → apply thread → the embedder's learner callback
```

## What is still C++ on this lane

Only kernels that touch Mako's own objects. Each has a reason:

| kernel (`server.cc`) | why it is C++ |
|---|---|
| `raft_command_clone_into` | a copy of `janus::Command` is a refcount bump the opaque Rust carrier cannot make |
| `raft_command_encode` | serialising a `janus::Command` needs Mako's marshalling code |
| `raft_command_from_bytes` | rebuilding a `janus::Command` from bytes, likewise |
| `raft_command_has_value` | reads whether a `janus::Command` holds a payload |
| `raft_byte_string_from_bytes` | the snapshot payload is a `std::string` the snapshot manager takes |
| log storage and snapshot manager kernels | RocksDB and the snapshot format are C++ types shared with the rest of deptran |
| `RaftWorker::Submit`, the apply callback | the embedder boundary, bytes on both ends |

Not on the list: the reactor, the poll thread, the fibers, the fiber events,
the RPC client and server, the message types, the serialisation and the
wire. On this lane those are all rustc-compiled Rust: the srpc crate from
`src/srpc/`, and Raft's own transport, service and generated messages from
`rt/src/`.

## Files that make up the complete runtime (Raft + srpc)

```
src/
├── deptran/
│   ├── rcc_rpc.rpc
│   ├── raft_main_helper.cc
│   ├── server_worker.cc
│   ├── server_worker.h
│   └── raft/
│       ├── Cargo.toml
│       ├── Cargo.lock
│       ├── src/                          raft core crate
│       │   ├── lib.rs
│       │   ├── server_h.rs
│       │   ├── server_cc.rs
│       │   ├── server_pods_h.rs
│       │   ├── scheduler_h.rs
│       │   ├── quorum_hpp.rs
│       │   ├── messages_hpp.rs
│       │   ├── log_storage_hpp.rs
│       │   ├── memory_log_storage_hpp.rs
│       │   ├── rocksdb_log_storage_hpp.rs
│       │   ├── snapshot_manager_hpp.rs
│       │   ├── memory_snapshot_manager_hpp.rs
│       │   ├── snapshot_format_hpp.rs
│       │   ├── channel_transport_hpp.rs
│       │   ├── commo_h.rs
│       │   ├── communicator_h.rs
│       │   ├── frame_cc.rs
│       │   ├── raft_worker_cc.rs
│       │   ├── raft_main_helper_cc.rs
│       │   ├── lab.rs
│       │   ├── lab_cases.rs
│       │   ├── lab_snapshot_cases.rs
│       │   └── lab_main.rs
│       ├── rt/                           raft-rt crate
│       │   ├── Cargo.toml
│       │   ├── src/
│       │   │   ├── lib.rs
│       │   │   ├── seam.rs
│       │   │   ├── transport.rs
│       │   │   ├── service.rs
│       │   │   ├── rpc.rs
│       │   │   └── lab_runtime.rs
│       │   └── tests/
│       │       ├── transport_roundtrip.rs
│       │       ├── rpc_wire_golden.rs
│       │       ├── service_is_a_service.rs
│       │       └── large_frame_bench.rs
│       ├── server.cc                     C++ host kernels
│       ├── server.h
│       ├── server_exports.h
│       ├── transport_exports.h
│       ├── lane_kernels.h
│       ├── raft_kernel_pods.h
│       ├── rust_facade_types.h
│       ├── raft_lane.h
│       ├── raft_lane_rust.cc
│       ├── raft_worker.cc
│       ├── raft_worker.h
│       ├── frame.cc
│       ├── frame.h
│       ├── log_storage.hpp
│       ├── memory_log_storage.hpp
│       ├── rocksdb_log_storage.hpp
│       ├── snapshot_manager.hpp
│       ├── memory_snapshot_manager.hpp
│       └── snapshot_format.hpp
├── srpc/                                 srpc crate
│   ├── Cargo.toml
│   ├── build.rs
│   ├── rust-modules.toml
│   ├── src/
│   │   └── lib.rs
│   ├── base/
│   │   ├── basetypes.rs
│   │   ├── callback_wrapper.rs
│   │   ├── debugging.rs
│   │   ├── logging.rs
│   │   ├── misc.rs
│   │   ├── threading.rs
│   │   └── srpc_base.c
│   ├── misc/
│   │   ├── any_message.rs
│   │   ├── rand.rs
│   │   ├── serializable.rs
│   │   ├── serializable_envelope.rs
│   │   ├── stat.rs
│   │   ├── srpc_io.c
│   │   ├── srpc_rand.c
│   │   └── srpc_timing.c
│   ├── reactor/
│   │   ├── epoll_wrapper.rs
│   │   ├── fiber.rs
│   │   ├── future.rs
│   │   ├── reactor.rs
│   │   ├── srpc_epoll.c
│   │   ├── srpc_fiber.c
│   │   ├── fiber_context_x86_64.S
│   │   └── fiber_context_aarch64.S
│   └── rpc/
│       ├── callbacks.rs
│       ├── channel.rs
│       ├── circuit_breaker.rs
│       ├── client.rs
│       ├── completion_tracker.rs
│       ├── connection_metrics.rs
│       ├── connection_state.rs
│       ├── errors.rs
│       ├── fiber_channel.rs
│       ├── frame_codec.rs
│       ├── heartbeat.rs
│       ├── idempotency.rs
│       ├── inmemory_channel.rs
│       ├── internal_protocol.rs
│       ├── load_balancer.rs
│       ├── pollable_proxy.rs
│       ├── reconnect_policy.rs
│       ├── request_options.rs
│       ├── request_queue.rs
│       ├── server.rs
│       ├── tcp_channel.rs
│       ├── utils.rs
│       ├── srpc_connect.c
│       ├── srpc_net.c
│       └── srpc_server.c
├── rusty-rustc/                          rusty facade crate
│   ├── Cargo.toml
│   └── src/
│       └── lib.rs
└── rusty-cpp-markers/
    ├── Cargo.toml
    └── src/
        └── lib.rs
scripts/
└── rpcgen_rust.py
CMakeLists.txt
```

## How the other two lanes differ

The core in `src/` is the same source on every lane. Only the runtime under
it changes:

| lane | core compiled by | seam kernels | RPC runtime |
|---|---|---|---|
| `rust` | rustc (`libraft_rt.a`) | `rt/src/seam.rs` | Rust srpc |
| `hybrid` | rustc (`libraft.a`) | `server_seam_cpp.cc` | C++ srpc (`RaftCommo`, `service.cc`) |
| `cpp` | rusty-cpp → C++20 modules (`libraft_cpp_core.a`) | `server_seam_cpp.cc` | C++ srpc |

Every lane links exactly one set of seam kernels. The build checks this with
`raft_lane_check` (`scripts/raft_lane_parity.py`).

## How to check this is still true

```
cargo test --manifest-path src/deptran/raft/rt/Cargo.toml   # wire and transport tests
python3 scripts/gen_correspondence.py --check               # every seam kernel in both runtimes
ninja -C <build> raft_lane_check                            # each kernel defined exactly once
./ci/ci.sh raftLabTest                                      # RaftLabTest on this lane
```

`rt/tests/transport_roundtrip.rs` runs the calling and responding paths above
over a real TCP socket, against an `srpc::Server` built from the same
generated `register` and `dispatch` the production service uses.
`rt/tests/rpc_wire_golden.rs` pins the wire bytes against the C++ encoding,
so a Rust-lane replica and a C++-lane replica can talk to each other.
