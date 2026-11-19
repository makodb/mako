# Translation and runtime audit, 2026-09-09

This is a historical baseline report, retained to explain the defects and repair requirements.
The findings, line numbers, counts, and removed file paths below refer to SRPC `9bba8a7`, not the
current working tree. For example, inspect a removed source with
`git show 9bba8a7:rusty-rustc/src/srpc.rs`. Links identify logical source locations; current files may
have changed or been deleted.

The completed repair routes runtime and serialization calls to canonical Rust, links the shared
C/assembly kernel, and removes the Rust facade packages. Cargo uses std and that kernel without
a C++ runtime or transpiler. Final acceptance on 2026-09-13 includes all 31 configured SRPC CTests
and all 17 runtime suites under each of AddressSanitizer, UndefinedBehaviorSanitizer and
ThreadSanitizer. [Rust lane independence](dev/facade-and-runtime-remaining.md) records the toolchain,
checks and limits, including the existing fiber suppressions and stack warnings in the address run.
[Canonical runtime notes](canonical-rust-runtime.md) describe the implementation and earlier
validation. This status annotation does not change the historical baseline findings below.

## Audited baseline

Audited SRPC commit `9bba8a7` with rusty-cpp pinned at `2abea1dc`.
The report is substantially true. Live Rust call paths still use substitute
implementations, some of which discard data or simulate runtime operations.
Other paths deliberately panic because only the C++ implementation works.
Passing the current tests does not establish that both languages execute the
same SRPC implementation.

The 37 canonical module providers really are Rust sources selected for C++
generation by CMake. The problem is the code they call and how those calls
resolve. `rusty-rustc` is omitted from C++ generation by package identity.
A call into that package can execute a substitute under Cargo and resolve to
a canonical generated implementation under C++. This audit establishes that
behavior; it does not establish anyone's intent.

Confirmed findings, ordered by repair priority:

| Priority | Finding | Evidence and consequence |
| --- | --- | --- |
| P0 | Payload construction discards the supplied payload under Rust. | `ArcMake<Arc<T>>` ignores its argument in [rusty-rustc/src/lib.rs](../rusty-rustc/src/lib.rs), lines 1859-1864. The resulting `SerializableBase` has empty save/load methods and kind 0, lines 1974-1985. Canonical AnyMessage and SerializableEnvelope packing reach this path. |
| P0 | Tests accept lost payloads as successful behavior. | [tests/serializable_envelope_rust.rs](../tests/serializable_envelope_rust.rs), lines 10-30, packs a KIND 61 value but expects kind 0 and a failed downcast. [tests/any_message_rust.rs](../tests/any_message_rust.rs), lines 47-84, expects unpacking to fail and verifies type-name bytes without checking payload bytes or recovering the original value. |
| P1 | Client/server execution uses a separate poll runtime. | [rpc/client.rs](../rpc/client.rs), lines 103-104, and the server/TCP aliases resolve to the facade. [rusty-rustc/src/srpc.rs](../rusty-rustc/src/srpc.rs), lines 104-534, implements another command queue, epoll worker, job dispatcher, and shutdown path. It runs jobs inline at line 391. The canonical worker in [reactor/reactor.rs](../reactor/reactor.rs) pumps `Reactor::run_loop` at lines 3309-3310 and starts jobs in fibers at line 3428. |
| P1 | Fiber helpers and events substitute different semantics. | [reactor/fiber.rs](../reactor/fiber.rs), lines 17-75, calls facade Fiber and fiber_sleep. The latter only records a duration at `rusty-rustc/src/srpc.rs:587`; yield only increments a counter at `rusty-rustc/src/lib.rs:452`. FiberFuture uses the facade BoxEvent at `reactor/future.rs:16`; FiberChannel stores the facade IntEvent at `rpc/fiber_channel.rs:65`. Those events wait on OS condition variables instead of yielding reactor fibers. |
| P1 | Canonical generic serialization and factories cannot run under Rust. | [misc/serializable.rs](../misc/serializable.rs), lines 504-514 and 663-690, reaches the unimplemented ADL bridge at `rusty-rustc/src/lib.rs:1289`. The deserialize bridge also panics. Factories at `misc/serializable.rs:1186` use panicking Arc/holder helpers. [tests/serializable_rust.rs](../tests/serializable_rust.rs), lines 308-337, expects container operations to panic; the factory test at line 158 checks signatures rather than execution. |
| P2 | SRPC algorithms remain duplicated inside the facade. | [rusty-rustc/src/srpc.rs](../rusty-rustc/src/srpc.rs), lines 20-62, implements a separate fixed-seed RNG reached by `rpc/client.rs:170`. Lines 611-755 contain a separate string codec used with facade archives by AnyMessage. These bypass the corresponding canonical code. |
| P2 | A platform adapter violates its clock contract. | `rusty-rustc/src/lib.rs:1462` implements `clock_monotonic_us` with wall-clock SystemTime. `rpc/client.rs:1986` uses it for time calculations, and `rpc/server.rs:690` uses it for shutdown elapsed time. The C++ runtime uses CLOCK_MONOTONIC. |
| P2 | Some original implementation logic was moved to C instead of Rust. | [rpc/srpc_net.c](../rpc/srpc_net.c), lines 13-46, owns the port search. [rpc/srpc_connect.c](../rpc/srpc_connect.c), lines 76-163, owns connect timeout control flow and self-connect rejection. [misc/srpc_io.c](../misc/srpc_io.c), lines 22-64, owns read/write retry loops formerly in C++. These are real native implementations, but the algorithms remain outside Rust translation. |

The poll facade performs real epoll I/O on a real thread. It is an alternate,
reduced runtime, not a completely fake socket implementation. Its optional
tick hook can pump the canonical reactor, but the only registration found is
in `tests/stackless_wake_pollthread_rust.rs:181`. Ordinary client/server
creation does not install that hook.

Several boundaries are legitimate. Standard-library wrappers, descriptor and
pthread ABI adapters, context-switch assembly, and the Future/Task waker
adapter need language-specific representations. The remaining
[misc/serializable_support.hpp](../misc/serializable_support.hpp) implements
C++ ADL and construction adapters. The C++ container serialization loops
themselves come from canonical Rust. The epoll platform file contains inline
Rust DSL and generated C++, rather than an unconverted second poll policy.
These distinctions matter when deciding what to delete or lower.

The current checks miss these problems for specific reasons:

- [check_facade_shadow.py](../scripts/check_facade_shadow.py) has ten explicit
  shadow exceptions. Its scan checks matching public names under the facade's
  `srpc` modules. Root aliases, re-exports, differently named helpers, and
  reachable behavior require a broader check. Its current success message
  omits the number of exceptions.
- [check_facade_stubs.py](../scripts/check_facade_stubs.py), lines 133-145,
  recognizes empty or simple constant bodies. Recording a duration or
  constructing an empty payload proxy passes this check. Panicking bodies
  are explicitly accepted. The empty SerializableBase methods are exempted
  as legitimate base hooks, even though the fake holder construction turns
  that base into the payload implementation.
- [check_srpc_crate_mode.py](../scripts/check_srpc_crate_mode.py), lines
  10282-10316, runs the same C++ importer against independently compiled
  generated objects and the production C++ archive. This is useful C++
  validation, but neither execution runs the Rust facade. Symbol equality
  and zero hand-attention slots do not prove Rust/C++ behavioral equivalence.

Validation performed on the audited baseline:

| Check | Result | What it establishes |
| --- | --- | --- |
| `cargo test --locked --workspace --all-targets` | 183 passed, 0 failed, 1 ignored | The current Rust suite accepts the substitutes above. |
| Both facade check scripts | Both passed; 12 facade items and 247 bodies checked | Existing checks permit the confirmed problems. |
| `python3 scripts/tests/test_goal0_standalone.py` | 7 passed | The structural inventory checks pass. |
| `ctest --test-dir build -L runtime_battery --output-on-failure` | 9 suites passed | Existing C++ runtime binaries pass. They were not rebuilt during this audit. |
| Direct Rust sleep probe | `this_fiber::sleep_ms(200)` returned in about 9 microseconds | Confirms the public helper takes the recorder path. This probe ran outside a real fiber and is not a test of the canonical scheduler's timing contract. |
| Temporary canonical PollThread rewire | `cargo check --offline --lib` passed with 11 cleanup warnings | The previously documented Rust type mismatch blocker no longer applies to this rewire. |
| Temporary fiber/event rewire | FiberFuture produced 9 generic-bound errors | The canonical BoxEvent bounds remain a concrete integration issue. |

For the successful PollThread probe, the temporary copy replaced
`cpp::ReactorPollThread` with `crate::reactor::PollThread` and qualified
`cpp_reactor::PollThread::` calls with `crate::reactor::PollThread::` in
client, server, and tcp_channel. The warnings were two unused imports and
nine unnecessary unsafe blocks. This was a library type check, not a
generated-C++ build or a runtime validation. No production source was changed.

Some comments describing compiler blockers are stale. In particular, the
PollThread shadow exception still cites six type mismatches. The generic
serialization limitation must also be reproduced on the pinned compiler
before assigning compiler work. The currently broken runtime paths are
confirmed regardless of whether their historical workaround is still needed.

The original repair plan called for small changes with the following completion
conditions. Start the P0 serialization repair in step 4 immediately after
step 1, alongside runtime work. Full runtime acceptance depends on completing
both steps 2 and 3.

1. Establish executable requirements and an inventory of remaining substitutes.
   Add regression cases that require payload preservation, successful generic
   serialization, real factory invocation, cooperative yielding, and timer
   wakeups. Record their failures on the current implementation. Replace the
   present kind-0, failed-unpack, and expected-unsupported-panic success
   expectations as each implementation is repaired. Inventory facade calls,
   alias targets, native symbols, and the canonical owner each should reach.
   Completion requires every finding above to have an executable behavior
   requirement or an explicit structural check.

2. Make native Rust integration tests run the canonical runtime.
   Add build/link support for the existing permitted C and assembly kernels
   in a Rust integration target. Rewire client/server/TCP to canonical
   PollThread and remove the now-unnecessary imports and unsafe scopes.
   Run the canonical worker with ordinary clients, deferred close jobs,
   and tasks that suspend. Complete stackful handler and receive-loop
   integration together with step 3. Tests must not provide fake
   fiber kernels or install facade tick hooks. Re-transpile and compile the
   affected C++ modules before accepting the rewire. Delete the separate
   facade poll loop once every caller and integration test uses the canonical
   worker.

3. Route fiber helpers, futures, and channels through canonical events.
   Change the Fiber aliases, current-fiber lookup, sleep/yield calls, event
   factories, and stored event types together. Resolve BoxEvent's existing
   `Clone + Default + 'static` requirements explicitly. Prefer narrowing
   bounds to the operations that need them; review any necessary public
   signature change against the C++ API. Test two fibers making progress
   during a wait, payload delivery after suspension, timeout ordering,
   wakeups from another thread through owner-thread dispatch, close during
   receive, and teardown. Assert owner-thread affinity rather than setting
   thread-affine events directly from a foreign thread.
   Completion requires deleting the sleep recorder, yield counter, facade
   IntEvent/BoxEvent, and the associated shadow exceptions.

4. Make serialization use executable canonical Rust throughout.
   Use canonical archives and real payload holders in AnyMessage and
   SerializableEnvelope. Preserve ownership, kind, dynamic type, registry
   construction, and unpack identity. Give generic serialization and factory
   functions the Rust traits they actually require. First probe these changes
   with the pinned transpiler; implement general constrained-generic,
   trait-dispatch, and owning-object lowering only where a minimal reproducer
   fails. Replace SRPC-name-based dispatch handling in the emitter with
   resolved trait/signature information. Keep a small explicit ADL adapter
   for external C++ payloads. Completion requires container, registry,
   AnyMessage, and envelope round trips to execute and recover the full
   payload in both languages, with no dropped payload or panic-backed
   construction helper.

5. Remove remaining algorithm duplication and correct adapter contracts.
   Route ClientPool random selection to canonical RandomGenerator. Reproduce
   the ABI-marked sibling lookup issue before changing the compiler; preserve
   public C++ spelling through metadata or generated wrappers. Remove the
   facade RNG and string codec after their callers use canonical code. Use
   the same monotonic clock contract in both languages and test deadline
   behavior independently of wall-clock changes. Retire unused facade
   declarations that only panic. Standard-library and OS adapters should
   have narrow contracts and contain no alternate SRPC policy.

6. Lower retained native policy to Rust.
   Move the port search, connect timeout/self-connect policy, and read/write
   retry loops into canonical modules. Keep platform structures, errno
   capture, individual OS operations, and context-switch machinery behind a
   documented native ABI. Link the same native leaves in Rust and C++.
   Preserve existing behavior first, including error and partial-I/O cases;
   review unrelated algorithm corrections separately. Completion requires
   the inventory to show a canonical Rust owner for every SRPC policy loop.

7. Make the gates verify implementation ownership and behavior.
   Extend the facade inventory to resolve aliases and re-exports and to
   cover root-level and private reachable helpers. Check canonical-to-facade
   dependencies against an explicit list of permitted runtime adapters.
   Report each remaining exception rather than an unconditional no-shadow
   success. Add negative controls proving the checks reject payload erasure,
   recorder-only sleep, and a substitute worker. Add shared Rust/C++ wire
   fixtures and comparable scheduler traces; do not use a function's own
   output as its expected answer. Retain C++ ABI/import checks as separate
   requirements. Refresh documentation after the executable checks agree.

Each canonical change must pass Rust tests and clippy, fresh C++ generation
and compilation, the existing ABI gate, and the relevant C++ runtime suites.
Runtime ownership changes also need the repository's sanitizer battery.
Closure means zero reachable fake SRPC behavior, no alternate SRPC runtime
in the facade, executable serialization in both languages, and a reviewed
inventory containing only necessary native and language adapters. A reduction
in the number of facade functions alone is not sufficient.
