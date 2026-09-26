# The Raft RPC path on the pure Rust lane

What one AppendEntries looks like once stage 3e lands: who calls whom, in
which language, with the line each step is at today. Everything named here
exists and compiles — `transport.rs`, `service.rs` and `rpc.rs` are built into
`libraft.a` now. What does not exist yet is the *wiring*: nothing calls
`raft_transport_serve`, so today the same request takes the C++ path instead.

Paths are under `src/deptran/raft/` unless marked. **Rust** = rustc-compiled
into `libraft.a`. **C++ kernel** = hand-written C++ the Rust calls out to.

---

## Calling path — the leader sends AppendEntries

```
RaftWorker::Submit(const char* log, int len, uint32_t par_id)   C++ embedder
  │                                              the boundary that STAYS: Mako
  │                                              hands Raft bytes, not objects
  ▼
raft_server_start  (export)                      one C ABI call
  ▼
RaftServerBase::Start                            Rust   server_h.rs:4779
  │  under mtx_: IsLeaderLocked() or REJECTED with index=term=0; then
  │  raft_command_clone_into (C++ KERNEL, server.cc:579) to copy the
  │  Command; then AppendLocal (server_h.rs:4329), which is what actually
  │  appends to state_.raft_log_
  │  RequestReplication() runs AFTER the lock scope closes (:4806-4807),
  │  not inside it — it wakes the gate, and waking it under mtx_ would
  │  invert the order the heartbeat fiber takes them in
  ▼
heartbeat_loop_body → heartbeat_phase1_body      Rust   server_cc.rs:1083
  │  per follower: decide entries, build the request
  ▼
transport_of(server) -> &RaftTransport           Rust   transport.rs:484
  │  by server identity, because RaftTransport is !Send and RaftServerBase
  │  must stay Send + Sync for srpc's Service bound
  ▼
RaftTransport::send_append_entries               Rust   transport.rs:241
  │  peer() consults the network-enabled flag first — None means "no peer",
  │  which callers already treat as a lost RPC
  ▼
RaftProxy::append_entries_async                  GENERATED   rpc.rs:397
  │  from rcc_rpc.rpc by scripts/rpcgen_rust.py
  ▼
Client::request_async(rpc_id::APPENDENTRIES, …)  srpc Rust   client.rs:1515
  │  serialises through BinaryWriteArchive, queues on the Rust PollThread
  ▼
                                                 bytes on the wire
```

One C++ kernel IS touched on this path, and an earlier revision of this file
claimed none was: `raft_command_clone_into`. It is there for the same reason
`raft_command_from_bytes` is on the responding path — `janus::Command` holds a
refcount the opaque Rust carrier cannot bump, so the copy happens on the C++
side and lands in a slot Rust owns. Everything else above is Rust.

The `Pending<AppendEntriesResponse>` the
call returns (`transport.rs:79`) is `Arc<Mutex<Option<Result<T, i32>>>>` — the
Send, Mutex-backed shape, so the reply can land on the poll thread while the
heartbeat fiber reads it later.

## Responding path — the follower answers

```
                                                 bytes on the wire
  ▼
srpc::Server on the Rust PollThread              srpc Rust
  │  reads the frame, looks up the rpc id registered by
  │  rpc::register (rpc.rs:281) via reg_fast_rpc (server.rs:924)
  ▼
RaftRpcService::__dispatch__                     Rust   service.rs:89
  ▼
rpc::dispatch(self, rpc_id, &req, &sconn)        GENERATED   rpc.rs:302
  │  AppendEntriesRequest::from_body(&req.body)  rpc.rs:135
  │  ── the stage-2c decode: `cmd` is at fixed offset 50 and exactly one
  │     fixed-width field follows, so its extent is arithmetic, not framing
  │  a malformed frame → reject_malformed_request, never a half-decode
  ▼
RaftRpcService::append_entries                   Rust   rpc.rs:274 (trait)
  ▼
raft_command_from_bytes                          C++ KERNEL   service.rs:35
  │  rebuilds a janus::Command from the payload bytes. This one stays C++
  │  while Mako owns that object — it puts the envelope on at Submit and
  │  takes it off at apply
  ▼
RaftServerBase::ServeAppendEntries               Rust   scheduler_h.rs:65
  │  the admission gate is HERE (stage 3c): IsDisconnected / IsRpcReady, and
  │  the unavailable reply is written on this side
  ▼
OnAppendEntries → on_append_entries_body         Rust   server_cc.rs:2411
  │  the actual Raft logic, under mtx_
  ▼
reply_with(weak_sconn, req, Ok(resp))            GENERATED   rpc.rs:359
  │  Ok  → code 0 plus the serialised fields
  │  Err → that code with no body, which peers read as "drop this reply"
  ▼
                                                 bytes on the wire
```

## Completion path — the leader reads the reply

```
Rust client reader fiber                         srpc Rust
  ▼
on_reply(code, ptr, len)                         Rust   transport.rs:241 closure
  │  AsyncReplyCallback = Option<Box<dyn FnMut(i32, *const u8, usize) + Send>>
  ▼
decode_reply::<AppendEntriesResponse>            Rust   transport.rs:94
  │  code != 0, or a malformed body → Err; never a partial response
  ▼
pending.slot.lock() = Some(result)               Rust
  ▼
heartbeat_phase2_body polls Pending::take()      Rust   server_cc.rs:1495
  │  the same question AppendEntriesResponse::completed answers by polling
  │  on the C++ path
  ▼
commit index advances → apply thread             Rust
```

## What is still C++ after all of this

Two kernels and one boundary, and each has a reason that is not "we ran out
of time":

| | why |
|---|---|
| `raft_command_clone_into` | copying a `janus::Command` is a refcount bump Rust cannot make |
| `raft_command_from_bytes` | `janus::Command` is Mako's object, not Raft's |
| `raft_byte_string_from_bytes` | same, for the snapshot payload's `std::string` |
| `RaftWorker::Submit` / the apply callback | the embedder boundary, already bytes on both ends |

Note what is NOT on that list: the reactor, the poll thread, the fibers, the
serialisation and the wire. Those are Rust — `srpc::PollThread` and the codec
come from `src/srpc/reactor/reactor.rs` and `misc/serializable.rs`, which are
canonical Rust compiled *twice*. Production runs the transpiled-to-C++ build
of them today; the lane move switches Raft to the rustc build of the same
source. It does not rewrite them.

## How to check this is still true

```
python3 scripts/raft_field_census.py      # no C++ names Raft server state
bash scripts/raft_dsl.sh --check          # carriers fresh, crate builds
cargo test --manifest-path src/deptran/raft/Cargo.toml
```

`tests/transport_roundtrip.rs` exercises the calling and responding paths
above against a real socket — a real `srpc::Server` built from the same
generated `register`/`dispatch` the production service uses.
