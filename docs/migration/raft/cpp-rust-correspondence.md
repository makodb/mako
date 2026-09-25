# Raft C++ ↔ Rust: what corresponds to what

Measured 2026-09-25 on `srpc-subtree-forward`. Sizes are line counts.

## Where each piece lives

| C++ | Rust | state |
|---|---|---|
| `raft/server.h` 606 + `server.cc` 1880 | `raft/src/server_h.rs` 5009 + `server_cc.rs` 2774 | Rust owns it; the C++ left is kernels and a pointer-holding shim |
| `raft/service.cc` 123 | `raft/src/service.rs` 150 | both exist; C++ is the one srpc dispatches to |
| `raft/commo.cc` 325 | `raft/src/transport.rs` 460 | both exist; C++ is the one that runs |
| `deptran/communicator.cc` 174 | `raft/src/communicator_h.rs` 207 | one source, two lanes — the Rust is transpiled into the C++ both engines link |
| `deptran/rcc_rpc.h` (Raft slice) | `raft/src/rpc.rs` 437 | generated from the same `rcc_rpc.rpc`; ids frozen in `raft/rpc_ids.txt` |

## How the boundary is crossed

| direction | mechanism | count |
|---|---|---|
| C++ → Rust | `extern "C"` exports over `RaftServerBase`, in `server_exports.h` | 31 |
| Rust → C++ | kernels declared `extern "C"` in the Rust, defined in `server.cc` | 78 |

An export is a method of the Rust server the C++ shim calls. A kernel is the
other direction — and most of them are C++ because of where the boundary sits
today, NOT because Rust cannot express them. Worth being exact, because an
earlier revision of this file said "the reactor, threads" and both are wrong:

| kernel group | why it is C++ | irreducible? |
|---|---|---|
| `raft_queue_wake_job`, `raft_spawn_heartbeat_loop`, `raft_fiber_sleep_us` | the Raft server runs on the C++ lane's reactor | **no** — the Rust lane has one, and `transport.rs` already uses it |
| `raft_spawn_apply_thread`, `raft_apply_thread_join` | `using RaftStdThread = ::std::thread` (`server.h:336`) | **no** — `std::thread::spawn`; CLAUDE.md lists this migration |
| `raft_monotonic_now_us` | it is `clock_gettime` | **no** — `std::time::Instant` |
| `raft_command_*`, `raft_byte_string_*` | `janus::Command` is Mako's object, put on at Submit and taken off at apply | yes, while Mako owns it |
| `raft_bind_commo` | `dynamic_cast` | yes — no Rust spelling |
| the 8 `raft_catch` sites | catching a C++ throw | yes |
| the four embedder `std::function`s | the embedder supplies them | yes |
| rocksdb, yaml-cpp | third-party C++ | yes |

So the boundary is where it is because the server has not moved lanes yet
(stage 3e), not because of a language limit.

## How C++ idioms appear in Rust

| C++ | Rust here | faithful? |
|---|---|---|
| `class X : public Y` (interface) | `pub trait Y` + `#[cpp_inherit] impl Y for X` | yes |
| `class X : public Y` (data-carrying base) | composition — `Y`'s fields become one owned value type | no, deliberately |
| `std::mutex mtx_;` beside the state | `mtx_: RaftCheckedMutex` beside `state_` | **transliterated** — Rust would be `Mutex<State>` |
| `void f(uint64_t* out)` | `fn f(&mut self, out: *mut u64)` (26 of these) | **transliterated** — Rust would return a value |
| `OnRequestVote`, `IsLeader` | same names under `#[allow(non_snake_case)]` | **transliterated** |
| `shared_ptr<T>` handed to Rust | opaque carrier: `#[repr(C)]` byte array + destructor kernel | yes, by necessity |
| `std::map<K,V>` in a two-lane type | `rusty::Vec` scanned linearly | no — a replica group is a handful of sites |
| quorum event you `wait_timeout` on | `VoteTally` you poll; same six-scalar outcome | no, deliberately |
| `shared_ptr<Response>` with a `completed` flag | `Pending<T>` = `Arc<Mutex<Option<Result<T, i32>>>>` | shape kept, mechanism changed |

## What must stay exactly faithful

Wire bytes, the four rpc ids, lock ordering, and observable protocol
behaviour. Everything else is shape, and shape is free.
