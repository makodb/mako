# RPC-level benchmark: Rust lane vs C++ lane

The measurement the microbenchmark could not give. Run on this host; the Mako
repo was not modified.

## Result

```
FIBER mode (one fiber spawned per request)
  C++   runs: 283410  334250  273400  291000     median 287,205    spread 21%
  Rust  runs: 449685  447120  445119  452480     median 448,402    spread  2%
  Rust / C++ = 1.56x   (+56%)

FAST mode (inline dispatch, no fiber), one run each
  C++   413,100      Rust 578,086      Rust / C++ = 1.40x   (+40%)
```

Parameters, identical on both sides: 4 client threads, 200 requests in flight
per thread (re-issued on completion to keep the pipeline full), 16-byte
payload, 6-second measurement window after a 1-second discard, one server
poll thread. Runs alternate C++/Rust so thermal drift lands on both.

**The Rust lane is faster at RPC level, by 40–56%, and far more stable** (2%
run-to-run spread against 21%).

## What was measured, and how the two sides were made comparable

**C++ lane.** `src/rrr/tests/rpcbench.cc` against the production
`build/src/rrr/librrr.a` — the archive Mako ships. That benchmark is not in
any CMakeLists: it was dropped from the build in `4a06ef0e` and had bit-rotted
against the current API (`rrr::make_source_proxy` was renamed to
`make_source_proxy_buffer`, 20 errors). Repaired with a rename in a scratch
copy of `benchmark_service.h`; no production file touched.

**Rust lane.** A twin written against `stonysystems/srpc@main`, compiled by
rustc as a staticlib with its own C kernels. Same structure as the C++ one:
`request_async` with a completion callback that immediately re-issues,
counting each issued request; a stat loop samples the counter once a second.

**Two confounds were found and removed before believing anything.**

1. *Dispatch mode.* The first pair compared C++ `-m fiber` against a Rust
   service registered with `reg_fast_rpc` — fiber-per-request against inline
   dispatch. That is not a comparison; it showed a bogus 2.04x. Both modes are
   now run matched: `reg_rpc`/`-m fiber` and `reg_fast_rpc`/`-m fast`.
2. *Server threading.* `rpcbench`'s `-w worker_threads` and `-e
   epoll_instances` are parsed and printed but **never used to size
   anything** — the C++ server is one `PollThread`, the same as the Rust one.
   Checked rather than assumed, because a thread-count mismatch would have
   invalidated the result outright.

## A finding that fell out of writing the twin

`AsyncReplyCallback` is declared `Option<Box<dyn FnMut(i32, *const u8, usize)
+ Send>>`. The C++ benchmark's pipelining idiom — capture the client in the
completion callback and re-issue from it — **does not compile in safe Rust**,
because `Client` holds `RefCell`/`Cell` and is therefore `!Sync`:

```
error[E0277]: `RefCell<Option<Arc<ClientConnection>>>` cannot be shared between threads safely
error[E0277]: `Cell<bool>` cannot be shared between threads safely
error[E0277]: `Cell<i64>` cannot be shared between threads safely
```

The callback does in fact run on the connection's own poll thread — the same
thread that owns the `Client` — so the pattern is sound; Rust's `Send` bound
simply cannot express "same thread". The benchmark asserts it with a wrapper
carrying that argument in a comment.

This is the same shape as the `client.rs:2366` violation recorded in
`rrr-under-rustc-experiment.md`, seen from the other side: there, `Send` was
absent and hid a real cross-thread race; here, `Send` is present and blocks a
legitimate same-thread pattern. Both point at the same gap — the API has no
way to say "owner thread only". A `!Send` callback variant, or a token proving
poll-thread affinity, would fix both.

## Limits

- **One workload.** 16-byte payload, 800 in-flight requests, one server
  thread, loopback. No large payloads, no many-connection fan-out, no
  contention, no failure injection, no latency distribution — only throughput.
- **The two lanes are not the same source.** The C++ lane is our `src/rrr`;
  the Rust lane is `srpc@main`, 8,142 lines ahead. Some of the gap may be
  upstream's own improvements rather than rustc versus clang. Separating
  those requires the transpiler rebase, after which both lanes can be built
  from one source — the same same-source/two-compilers design the leaf
  microbenchmark used.
- **Service bodies differ slightly.** Both deserialize a string and reply;
  the C++ reply is the generated `fast_nop` response, the Rust one is an
  `i32`. Single-digit bytes, unlikely to matter at these margins, but not
  identical.

## Relation to the earlier leaf benchmark

`rust-vs-cpp-lane-benchmark.md` measured three leaf codecs and found the
lanes within tens of percent of each other **in both directions** — rustc
ahead on `write_header` and `load64`, behind on `dump64`. That was a fair
result for what it measured and it is not contradicted here: leaf-codec time
is a small share of an RPC, which also pays for epoll, the fiber switch,
framing, buffer management and the callback path. The RPC-level number is the
one that answers "is the Rust runtime as good as the C++ runtime", and the
answer on this workload is *better*.

## Reproducing

```
# C++ lane (needs the benchmark_service.h rename in a scratch include dir)
rpcbench -s 127.0.0.1:PORT -m fiber &
rpcbench -c 127.0.0.1:PORT -m fiber -t 4 -o 200 -b 16 -n 6

# Rust lane (srpc@main scratch copy, bench2/)
srpc-rpcbench -s 127.0.0.1:PORT &
srpc-rpcbench -c 127.0.0.1:PORT 4 200 16 6
```
