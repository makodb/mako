# Rust lane independence

Completed 2026-09-13. The canonical Cargo lane runs on Rust std and the small
C/assembly kernel, without a Rust facade package or C++ runtime.
The generated C++ lane remains a separate consumer of the same Rust sources.

## Current status

The canonical Cargo lane runs on Rust std and the native C/assembly kernel. It
has no production Rust dependencies and needs no facade package, C++ runtime or
transpiler.

The [independence check](../../scripts/check_rust_independence.py) copies only
canonical Rust modules, tests, Cargo inputs and native kernel sources into a
new directory. It copies no facade, C++ runtime,
C++ source or transpiler, then runs locked offline Cargo tests and doctests with a
restricted tool path and `CXX=/bin/false`.

Current acceptance on Linux x86_64/glibc, with Clang 22 and libc++ for C++:

| Check | Result |
| --- | --- |
| Isolated Cargo copy | 266 Rust tests and two doctests pass; one existing layout test is ignored |
| Regular Cargo workspace | Tests, doctests and clippy with warnings denied pass |
| Ownership and native checks | Canonical body, native source and native ABI audits pass, with all 47 negative controls |
| Complete CMake and ABI | All 37 providers and the umbrella compile; exact inventories and importer runs against fresh objects and the production archive pass |
| Configured SRPC CTests | All 31 pass, including all 17 runtime suites |
| AddressSanitizer / LeakSanitizer | All 17 runtime suites pass with the existing fiber suppressions |
| UndefinedBehaviorSanitizer | All 17 runtime suites pass with no findings |
| ThreadSanitizer | All 17 runtime suites pass with no reports |

Removal and acceptance are complete. No facade or C++ runtime dependency remains
in the canonical Rust lane. The C/assembly kernel and the separate C++ consumer
requirements are described below.

## Removed dependencies

The migration replaces the original 195 facade declarations with standard Rust
APIs, canonical implementations and explicit native ABI declarations:

| Former facade dependency | Canonical replacement |
| --- | --- |
| Arc, collections, synchronization and ownership extensions | Rust std types, `Arc::get_mut`, and lazy initialization for shared registries |
| Required and nullable callable models | `Box<dyn Fn...>` and `Option<Box<dyn Fn...>>` with the original thread-safety bounds |
| Invalid/default owner models and constant validity methods | Required owners or explicit `Option<Arc<...>>` / `Option<Box<...>>` |
| Descriptor and TCP models | Standard owned/borrowed descriptors, `TcpStream`, `TcpListener`, and I/O errors |
| Sleep, process ID, thread and logging helpers | Standard thread/process APIs and stdout `Write` / `flush` |
| Task, Poll, Context and Waker models | Standard `Future`, `Pin`, `Poll`, `Context`, `Wake` and `Waker`; scheduling remains in `reactor/reactor.rs` |
| Panic payload helpers | `panic_any`, `catch_unwind`, `AssertUnwindSafe`, and typed payload downcasts |
| STL serialization models and source/sink dispatch | Canonical byte/container algorithms plus C++ consumer forwarders |
| Pair/vector models | Rust tuples and `Vec`, with scoped compiler mappings for existing C++ pair/vector interfaces |
| Job-set model | Canonical `BTreeMap` keyed by shared job identity |
| Load-balancer contracts | Canonical traits and implementations |
| IPv4 parsing/formatting models | Standard Rust address parsing and formatting |
| pthread, FILE, C void and fiber layout models | Explicit canonical C ABI declarations checked against the native binding inventory |

C++ lowering support was extended where standard Rust and the existing C++
interface differ. This includes callback ownership/borrowing, nullable owner
aliases, future and waker lifetimes, move-only thread captures, standard panic
payloads, native type bindings, and scoped container mappings. Ordinary Rust
owners retain their normal Rust semantics.

The integrated retirement deletes `rusty-rustc` and `rusty-cpp-markers`, their
Cargo dependencies and lockfile entries, the old facade inventory/audit entry
points, the unused C++ marker header, and obsolete compiler metadata. The
canonical AST audit retains the reviewed constant-function inventory and
rejects new missing or default-returning runtime bodies.

## What remains by design

Cargo compiles the nine C files in
[scripts/native-kernel-sources.txt](../../scripts/native-kernel-sources.txt),
plus the fiber assembly selected for `x86_64` or `aarch64`.
[build.rs](../../build.rs) links them as `libsrpc_native.a`. They provide OS
operations, clock/calendar fields, entropy, native resource/layout access and
fiber context switching. Linux, a supported architecture, a C compiler and an
archiver remain required. Scheduling, transport decisions and serialization
policy stay in canonical Rust. Removing the C/assembly kernel is outside this
goal.

The C++ build continues to use the vendored rusty-cpp runtime, compiler type
mappings, module preambles and C++ compatibility headers. Cargo does not load
or link these inputs. The C++ serialization adapters perform container
operations and forward into canonical implementations; they retain existing
C++ overloads and erased adapter entry points.

## Validation record

The first integrated std/descriptor/location batch passed the complete C++
build, the existing ABI inventory, and all 26 then-configured SRPC CTests.
Later slices have independent C++ checks:

- The final compiler unit/codegen suite passes 2,472 tests with one existing
  ignored test. Two old panic entry-point expectations were updated to match
  the standard owned-payload lowering.
- Native C/fiber type binding changes preserve every defined symbol in five
  affected providers.
- Standard IPv4 parsing matches 637 Rust-derived cases. TCP retains all
  existing strong provider symbols.
- Standard stdout logging preserves all 40 defined symbols and passes byte,
  embedded-NUL, newline and flush checks.
- Serialization retains all 596 existing strong symbols and adds one
  canonical `serialize_bytes` helper. All 12 C++ parity tests pass.
- Future runtime checks cover non-default, move-only outputs, retained wakers,
  nested suspension, cancellation and ownership, including address/undefined
  sanitizer tests.

Fresh combined objects also establish the reviewed reactor change. Standard wake
and job handling add four helpers; two private, non-exported thread-ID conversion
functions disappear. Their only callers were inside worker creation and shutdown,
which now store kernel thread IDs directly. No consumer called those functions.
The server's private, non-exported `no_reply_writer` also disappears: its four
internal callers now use an absent callback. Public reply signatures are preserved.
The load-balancer traits add eight RTTI/vtable/destructor symbols. The exact
inventories record these changes and the serialization helper addition.

Explicit standard type paths and imported callback aliases resolve construction
in the reactor module's macro-containing scope. Client callback ownership
transfers and an explicit serialization factory type preserve the C++ interfaces.
All 37 providers match the reviewed unique and raw symbol inventories.

A scanned C++ consumer keeps the runtime umbrella and its dependencies current
and supplies the ABI importer's module map. This repairs Clang crashes caused by
stale synthesized runtime BMIs after canonical providers stopped importing the
runtime umbrella.

C++ lvalue waker calls retain borrowed dispatch; generated standard Rust wake
calls consume their receiver. Compiler inference keeps inferred clones movable.
The same-source Rust/C++ fixture checks independent clone ownership and immediate
release. Direct tests cover repeated and concurrent wakes, moved/empty wrappers,
and retained wakes after cancellation, including ASan/UBSan runs.

On glibc, parking-token ownership now survives C++ TLS destructors and is released
by native thread cleanup or normal process exit. Retained thread handles keep
their own owner; tokens created by later static destructors receive another
cleanup. Other platforms retain their existing implementation. Focused tests
pass for standard and POSIX backends normally, with ASan/LSan and UBSan, and with
TSan. Each configuration exercises 100 TLS destructors, external threads,
parking during teardown, retained handles, and late process-exit calls. The two
Cargo integration tests for this fixture also pass.

The old parking-token lifetime caused both direct use-after-free reports and
later heap corruption. A runtime-only reproducer showed two simultaneous malloc
allocations receiving the same address after invalid TLS access; the repair
passes that reproducer. The later waker report required no independent repair.

The full address run uses the five existing fiber-allocation suppression patterns
in [scripts/lsan_suppressions.txt](../../scripts/lsan_suppressions.txt). It reports
no ASan or LSan errors, with nine suppression summaries and nine warnings about
`__asan_handle_no_return`. Native fiber switches still lack sanitizer fiber hooks;
these results do not establish that suppressed allocations are leak-free or fully
validate custom fiber stacks. The paired Rust process is ordinary Cargo and is
not instrumented by the C++ sanitizer configuration.

The authoritative symbol expectations remain in
[scripts/check_srpc_crate_mode.py](../../scripts/check_srpc_crate_mode.py).

## Starting investigation

The initial investigation used SRPC `c591960` and rusty-cpp
`3e1d95059839e4bf1968891047ff1563a2f08c17`. The facade then contained 195 AST
declarations across 1,482 lines: 52 import records, 108 standard/model records,
14 trait-dispatch records, 12 C-layout records and nine future records.
The old inventory and F1-F6 probe details are available in this file's Git
history. They describe the starting tree, not remaining work.

See [canonical-rust-runtime.md](../canonical-rust-runtime.md) for runtime
ownership and the migration's behavioral contracts.
