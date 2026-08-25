# Reactor canonical-Rust checkpoint

Status: **HOLD (Cargo-valid source repair; native promotion blocked)**

This tree is an isolated working clone at
`/var/tmp/srpc-reactor-canonical-v1.Y4e5zD/repo`, branch
`agent/goal0-reactor-canonical-v1`, based on accepted TCP V2 commit
`953908308bda64f70d9fac0ed31288db9ab63a0d`.  No commit, push, or freeze has
been made.

## Canonical carrier

- Sole source of truth: `reactor/reactor.rs` (historically `reactor/reactor.cpp`;
  the rename was pure, so `git log --follow` still reaches the C++ era)
- Cargo view: `#[path = "../reactor/reactor.rs"] pub mod reactor;`, declared by
  the generated `src/lib.rs`
- Current carrier: 3,372 lines, SHA256
  `44c34b4a677f19b2fae2aaadd445a4a714ef7642ca98e7fc80a1493a5cd312d5`
- Scope lineage: all 111 incumbent inline-Rust blocks are retained in the
  full carrier; this is not a PollThread-only extraction.
- Incumbent hybrid carrier SHA256:
  `7070afe90daa9c658bf1c27945cdb03389cb0fa58e5b5f4b6a947b2cfb78f0f3`
- Verbatim 111-block extraction SHA256:
  `39c0d6be66c9d79a33f19051d7df3b56c3d1392e758337e4215cd042b5984e5c`

## Incumbent C++ ABI contract

The source-matching reference object is
`/var/tmp/srpc-standalone-final-build.RVqRue/CMakeFiles/rrr.dir/reactor/reactor.cpp.o`,
SHA256 `a12d420053bf5f04190af301f1d9424b21f3c9347e48e7bdcfc0daa769151808`.

Its strong-symbol audit contains 323 raw entries.  The normalized owned
manifest contains 300 entries: the module initializer and C
`fiber_task_entry_thunk` plus 298 owned C++ symbols.  Exact manifests are:

- `/var/tmp/reactor-incumbent-strong.raw`, SHA256
  `727e3a4d8de5864fe8f2dc7797a24b378cbbcca2d6efb7c689c1e354264160ba`
- `/var/tmp/reactor-incumbent-strong.unique.raw`, SHA256
  `a3c61d6f3278ac557f2cc71694a537c3edbcd6b2373b99be98644427b044f35a`
- `/var/tmp/reactor-incumbent-strong.demangled`, SHA256
  `d92b225de82c6e449ce1c1b7cc27c4e4fa06315ac3e6a01d6350f032d2e9c9d8`
- `/var/tmp/reactor-incumbent-owned.unique.demangled`, SHA256
  `e566039257c993ce43e9d96132ffc55d24300edbbd4bfd65c8b9104bc8d5be86`

Namespace identity is part of this contract.  The reference manifest has 46
strong entries rooted directly in global `janus` (QuorumPolicy, QuorumEvent,
QuorumEventWrapper and their factories/helpers), plus three QuorumEvent
RTTI/vtable entries that also name `janus`.  The remaining Reactor surface is
rooted in `rrr`, all attached to module
`rrr.reactor`.  A generated `rrr::QuorumEvent` or `rrr::janus::QuorumEvent`
is not ABI-compatible.

Exact incumbent size/alignment pairs, in bytes:

| Type | Size | Align |
| --- | ---: | ---: |
| `rrr::EventStatus` | 4 | 4 |
| `rrr::EventState` | 160 | 16 |
| `rrr::EventPollable` | 8 | 8 |
| `rrr::BoxEvent<int>` | 224 | 16 |
| `rrr::IntEvent` | 224 | 16 |
| `rrr::SharedIntEvent` | 56 | 8 |
| `rrr::NeverEvent` | 208 | 16 |
| `rrr::TimeoutEvent` | 224 | 16 |
| `rrr::WaitAny` | 256 | 16 |
| `rrr::WaitAll` | 272 | 16 |
| `rrr::fiber_yield_t` | 8 | 8 |
| `rrr::fiber_task_t` | 240 | 16 |
| `rrr::FiberStatus` | 4 | 4 |
| `rrr::Fiber` | 144 | 16 |
| `rrr::StacklessTaskEntry` | 64 | 16 |
| `rrr::Reactor` | 504 | 8 |
| `rrr::PollCommand` variant | 16 | 8 |
| `rrr::PollCommand_AddPollable` | 8 | 8 |
| `rrr::PollCommand_RemovePollable` | 4 | 4 |
| `rrr::PollCommand_ClosePollable` | 4 | 4 |
| `rrr::PollCommand_UpdateMode` | 8 | 4 |
| `rrr::PollCommand_AddJob` | 8 | 8 |
| `rrr::PollCommand_RemoveJob` | 8 | 8 |
| `rrr::PollCommand_Shutdown` | 1 | 1 |
| `rrr::PollThreadWorker` | 200 | 8 |
| `rrr::PollThread` | 104 | 8 |
| `janus::QuorumPolicy` | 4 | 4 |
| `janus::QuorumEvent` | 352 | 16 |
| `janus::QuorumEventWrapper` | 8 | 8 |

The probe source/object are
`/var/tmp/srpc-reactor-audit.eLLZeD/repo/.reactor-audit/layout_probe.cpp`
and `layout_probe.o`.

## Rust gates reached

- `python3 scripts/extract_rrr_rust.py --check`: PASS, 35 manifest-owned
  modules plus generated `src/lib.rs`.
- `cargo check --all-targets`: PASS (zero errors).
- `cargo test --all-targets --no-fail-fast`: PASS, 129 tests discovered,
  128 passed and one generated-C++-only layout test ignored.
- `cargo test --doc`: PASS, including the `Job` compile-fail gate that rejects
  an `Rc<Cell<_>>` implementation even when it uses `unsafe impl`.
- `tests/reactor_rust.rs::worker_transfer_types_prove_auto_traits`: PASS.
  This proves `PollCommand: Send`, `PollThread: Send + Sync`,
  `PollableProxy: Send`, and `Arc<dyn Job>: Send + Sync` without an unsafe
  blanket auto-trait implementation.
- `tests/reactor_rust.rs::stackless_wakers_use_owner_ingress_and_stable_bindings`:
  PASS.  This pins the source-level ownership/lifetime shape and rejects the
  former raw Reactor back-pointer wake path; it is not a native concurrency
  runtime proof.

Safety changes deliberately make the proof source-level:

- `PollableBase: Send`
- documented `unsafe trait Job: Send + Sync`, because `Work(&mut self)` is
  exclusively worker-owned while readiness may be observed across threads
- `OneTimeJob` callback is `FnMut() + Send + Sync`
- the former broad unsafe Send/Sync shells for PollCommand/PollThread are gone

`OneTimeJob`'s three state fields are private only in Rust, preventing a
caller that retains a concrete Arc from racing worker mutation.  A focused
emission probe at `/var/tmp/reactor-misc-private-fields-probe.cppm`, SHA256
`d1a366f50fee8e1b9b69f89034f69e0b87f45ddf22d02ed760901945d1820b78`,
confirms the production C++ remains an exported `struct OneTimeJob : Job`
with the same public `bool done_`, `bool ready_`, and
`rusty::Function<void()> func_` fields in order.

Additional source-fidelity corrections made after the first emitted-C++ audit:

- Reactor's libc character facade now uses the established `LegacyCChar`
  mapping.  It remains `i8` for rustc but emits C++ `char`, so the generated
  `getenv` declaration and call no longer reinterpret `char*` as `int8_t*`.
- The seven concrete `get_self` methods read their own `self_` weak field
  directly.  This is behavior- and layout-neutral and removes a missing early
  declaration for the generic helper from the native compile frontier.
- `stackless_profile_enabled` uses a 0/1/2/3 atomic initialization state.  One
  racing caller evaluates `getenv`; all others wait until it publishes the
  disabled/enabled result.  This restores the incumbent C++ magic-static's
  single-evaluation contract without pretending a racy load/store cache is
  equivalent.
- The variadic `syscall` declaration now uses a checked `LegacyCLong` facade,
  mapped to C++ `long`.  This removes the prior `int64_t`/`long` source
  mismatch on the accepted LP64 target; the compiler still drops the required
  ellipsis and therefore still fails the native C declaration gate.
  SUPERSEDED: the variadic `syscall` declaration and the `LegacyCLong` facade
  are deleted outright — the only caller was `syscall(SYS_gettid)`, now the
  non-variadic `srpc_reactor_gettid()` C seam.  See the configuration-fidelity
  resolution at the end of this document.
- `QuorumDanglingVec` now uses the existing `rusty::StdPair` facade and checked
  `std::make_pair` import.  Frozen output is exactly
  `rusty::Vec<std::pair<uint16_t, int64_t>>`, matching the incumbent callback
  ABI rather than the former incorrect `std::tuple` spelling.
- `Reactor::new` seeds `thread_id_` with the creating thread.  This keeps the
  owner-thread teardown guard valid for public direct construction as well as
  the two TLS factories; it changes neither field order nor method signature.

## TLS and Rust runtime limitation

Nine declarations now use exact inert `#[cfg_attr(any(), thread_local)]`
markers: seven historical namespace statics, historical function-local
`last_report_us`, and the new private function-local wake-owner registry.
Frozen-compiler output was inspected directly: all seven namespace statics
have `export extern thread_local` declarations and `inline thread_local`
definitions, while both function-local statics are `static thread_local`.
Rustc still sees ordinary process-global mutable statics.  Therefore the
direct-Rust Reactor lane remains intentionally typecheck/auto-trait/source-
shape only; no Cargo result is claimed as Reactor runtime parity.

Promotion requires generated-C++ multithread/TLS runtime coverage, including
the incumbent `test_reactor`, `test_reactor_extended`,
`test_reactor_minimal`, `test_timeout_race`, `test_and_event`, `fiber_test`,
`fiber_runtime`, and `rpc_pollthread_proxy_storage_test` behaviors (plus their
dependent RPC integration tests).

## Stackless waker/Context source repair

The former three waker closures retained raw `*const Reactor` back-pointers
and could mutate owner-only `RefCell`/`VecDeque` queues from a foreign thread.
They also polled through stack-local `Waker`/`Context` objects even though the
native `Task::poll` contract retains `Context*`.  That source-owned lifetime
and race defect has been repaired without adding a field to `Reactor`,
`StacklessTaskEntry`, or `PollCommand` and without changing their public Rust
signatures:

- A private TLS registry associates each Reactor address with an
  `Arc<StacklessWakeIngress>`.  Its TLS object is only a null-initialized raw
  pointer (trivially destructible); the heap registry survives namespace-TLS
  Reactor destruction and is freed/reset by the last active Reactor
  unregister.  This avoids reverse TLS-destruction UAF.  A foreign waker
  retains only the ingress and an `Arc<StacklessWakeTicket>`; no Reactor
  pointer crosses threads.
- The ingress has an atomic accepting flag and a mutex-protected pending
  queue.  The owner drains it from `process_stackless_tasks`; duplicate wakes
  coalesce through the ticket's atomic `enqueued` bit.
- Each ticket's atomic slot is changed to `STACKLESS_UNREGISTERED_SLOT` before
  completion, binding retirement, teardown, or slot reuse.  Old/in-flight
  tickets are therefore ignored rather than waking a recycled task slot.
- Every `Waker` and `Context` is in one stable boxed binding.  The initial
  poll's binding is retained in the task state, and field order makes the
  native Task die before the retained Context/Waker.  The registered poller is
  explicitly dropped before its binding is detached.
- Reactor destruction first rejects new ingress, tombstones tickets, and
  moves task closures out of their `RefCell`, then destroys them while
  bindings still exist and no borrow is held.  Reentrant registration sees
  `accepting=false` and destroys the rejected poller without publishing a
  slot.  It then unregisters the private owner state.  A late copied waker
  retains only the rejected ingress and ticket.
- Completion destroys the Task and detaches its old binding before publishing
  the index to the free-slot stack.  Reentrant cancellation destructors cannot
  reuse a slot whose old Context is still live.
- Stackless ingress, enqueue, processing, both spawn paths, and Reactor
  destruction enforce the historical owner-thread contract.  A native
  wrong-thread call cannot silently create an unretirable second TLS registry.

All new helper functions are generic and instantiated as C++ templates, and
the registry is function-local TLS.  This is deliberately designed to avoid
new externally linked ordinary helper symbols, but it is not a symbol proof:
only a successfully compiled generated object plus exact `nm` comparison can
establish no strong-symbol drift.

As a bounded no-drift check, the complete generated declaration bodies before
and after this repair are byte-identical for `Reactor`
(`cbe667e49b7f6d862d2ec138dc22d76125d88071e758219dffd801cb88adbe21`),
`StacklessTaskEntry`
(`aba2c2feb962769c2e19cb98b09c0159b92fd144811bfca6f6fe3138b9d7ea87`),
and the full `PollCommand` algebraic declaration
(`f0b13435a8fc17ef15f134e62938d6b419b7b24de6e3fa549df39d19bcbc214c`).
This proves the repair did not change those generated declarations; it does
not substitute for native `sizeof`/`alignof` or symbol gates.  In particular,
the unchanged generated `PollCommand_AddPollable` is already wrong relative
to the incumbent: it stores/factories a `void*`, while the historical contract
is owning `rusty::Box<PollableBase>`.

Native promotion remains on HOLD.  Required tests are: a foreign-thread wake
completing on the original owner TID; wake during the initial poll; duplicate
and concurrent wake coalescing; completion racing a forced slot reuse; Reactor
destruction and PollThread shutdown racing a retained waker; normal TLS, disk
TLS, and arbitrary non-PollThread Reactors; shutdown latency; and ASan/TSan.
The exact 29-layout and 300-owned-strong-symbol oracles must then pass.  Cargo
cannot run these semantics faithfully because its inert TLS markers are not
Rust TLS.

An independent bounded re-audit qualified-accepts this source-level waker
repair: after the TLS lifetime, destruction/reentrancy, slot-reuse, owner-
thread, and direct-construction corrections, it found no remaining
unconditional critical/high source-owned stackless-waker defect.  That verdict
does not waive any generated-native gate below.

## Transpiler frontier

The frozen integrated producer used for the current atomic rerun is:

- binary:
  `/var/tmp/rusty-cpp-goal0-integrated-v1.ivJ2g3/repo/target/debug/rusty-cpp-transpiler`
- SHA256:
  `4db2d95f7b12dd4855e5c82d6a0d45bd634424ec6475368c836873914c4f1f63`
- compiler base/source revision: `fda55318a82c4c95bfaa9df0181860ad17d3aae5`
- frozen integration patch:
  `/var/tmp/rusty-cpp-goal0-integration-freeze.K5fi0l/goal0-compiler-integration-v1.patch`,
  SHA256 `6405c0fca92c7c9a4171d1f86de142687f1bebb2075c05d02e90f18584f0fc01`
- reconstructed staged tree:
  `0bd39839c1f679b90142ea7ab5a40db8321a291d`

This producer identity does not make Reactor an accepted/frozen provider.

It transpiles the entire 36-module crate with zero errors and zero hand slots.
The untouched output is
`/var/tmp/srpc-reactor-transpile-waker-sourcefix3.d6VWvr/rrr.reactor.cppm`, 10,123
lines, SHA256
`92cdea2a2884b790e0b871f0af98d2249d684280cd492cc4a74317ea10b1fc85`.
The output directory contains 36 module providers plus generated CMake and a
zero-slot report.  The seven namespace TLS declarations/definitions and both
function-local TLS sites were verified in this exact file.

Direct Clang 22 compilation with the incumbent Reactor's exact configured
module map reaches that generated provider and stops in these compiler-owned
clusters:

1. Authenticated type-map targets `::srpc_fiber_ctx` and `::srpc_fiber` lose
   their leading-global semantics and become nonexistent
   `::rrr::srpc_fiber_ctx` / `::rrr::srpc_fiber` (generated lines 5032-5033).
2. The raw C function-pointer parameter of `srpc_fiber_init` is rewritten as
   `rusty::UnsafeFn<void(void*)>` at line 5572.  The `syscall` source now maps
   exactly to C++ `long`, but its required `...` is dropped at line 5577, so
   both declarations conflict with authoritative C headers.
3. Private trait `EventCore` is forward-declared in root `rrr` at line 4989
   but defined in an anonymous namespace at line 5611.  Root adapter
   specializations begin at line 6200, making the base ambiguous/incomplete.
   Their `core_self` signatures also collapse authenticated
   `std::sync::Weak<dyn EventPollable>` into `rusty::rc::Weak<void*>`, while
   emitted implementations use `rusty::sync::Weak<void*>`.
4. The repaired waker's explicitly typed
   `Box<dyn Fn() + Send + Sync>` local is emitted at lines 6936-6939 as a
   `rusty::Box<std::function<void()>>` initializer for move-only
   `rusty::Function<void() const>`.  Native `rusty::Waker` requires a copyable
   `std::function<void()>`; this is neither a valid coercion nor the exact
   runtime type.

Untouched generated text also proves these ABI blockers even though the first
Clang error limit is reached earlier:

- `PollCommand_AddPollable` at lines 5414-5451 collapses the owning
  `Box<dyn PollableBase>` field and factory argument to `void*`.  Size may
  coincide, but ownership, destruction, type identity, and calling ABI do not.
- Resolved by-value aliases such as `EventTestFn`, `FiberTaskFn`, `FiberFn`,
  `TaskVoid`, `PollCmdReceiver`, and `QuorumFinalizeFn` are rewritten as
  abbreviated `auto` parameters in non-generic constructors/methods/helpers
  (first examples at lines 5132, 5136, 5157, 5169-5171, and 5200).  This turns
  historical ordinary symbols into templates or changes their mangling.
- `TaskVoid` is declared as `rusty::Task<rusty::Unit>` at line 5019 although
  the incumbent/native specialization is `rusty::Task<void>`.  Exported
  `SrcFileCStr` is `std::string_view` at line 5014 despite its authenticated
  `const char*` map (parameter uses happen to map correctly).
- `Arc::downgrade(&base)` after an explicit
  `Arc<dyn EventPollable>` coercion is emitted as `Arc<Ev>::downgrade(base)` at
  line 7176, compounding the erased Weak target identity.
- Large synthetic `EventPollable_`/`EventCore_` UFCS declaration and definition
  surfaces begin at lines 5203/5224 and 9476/9532.  They have no incumbent
  owned-symbol counterparts and must not create new strong ABI.
- `reactor_make` at lines 7676-7678 calls
  `rusty::Rc<Reactor>::new_(Reactor::new_())`, but the generated `Reactor`
  declaration has only `Reactor()` (line 6097) and no `Reactor::new_` exists.
  The incumbent uses its constructor/in-place creation path; this is a
  compiler ctor-call lowering defect rather than a source waker defect.

The exact repro compiles from
`/var/tmp/srpc-standalone-final-build.RVqRue` with Clang 22, the response file
`CMakeFiles/rrr.dir/reactor/reactor.cpp.o.modmap`, and the generated
`rrr.reactor.cppm` above.  It exits 1 at the exact clusters above.  No Reactor
object or BMI is produced, so layout and strong-symbol comparisons cannot yet
be claimed.

The non-mutating syntax repro is:

```sh
/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang++ \
  -I/var/tmp/srpc-reactor-canonical-v1.Y4e5zD/repo \
  -I/var/tmp/srpc-reactor-canonical-v1.Y4e5zD/repo/third-party/rusty-cpp/include \
  -stdlib=libc++ -g -std=gnu++23 -stdlib=libc++ -w -Wreturn-type -MD -MP \
  -DRUSTYCPP_DISABLE_ARC_LOG -DREUSE_FIBER -O2 -g -fno-omit-frame-pointer \
  -MF /var/tmp/srpc-reactor-waker-sourcefix3-repro.d \
  @CMakeFiles/rrr.dir/reactor/reactor.cpp.o.modmap \
  -fmodule-output=/var/tmp/srpc-reactor-waker-sourcefix3-repro.pcm -fsyntax-only \
  /var/tmp/srpc-reactor-transpile-waker-sourcefix3.d6VWvr/rrr.reactor.cppm
```

Diagnostic stream-patched syntax checks did not edit either canonical source
or frozen output.  Cascades after the authoritative first clusters are not
accepted as independent blockers until those clusters are fixed and the
unpatched provider is regenerated.  The boxed-callable mismatch is recorded
independently because it is directly present in the untouched generated text.

Direct single-file named-module transpilation is not an alternative: the
compiler correctly rejects Reactor's physical sibling imports because only
prepared crate mode can authenticate `cpp_import_namespace` bindings.

## Exact minimal compiler contracts

Source workarounds cannot faithfully repair the remaining failures.  The
minimum compiler-owned contracts are:

1. **Global `::janus` placement.**  Namespace placement must come from an
   exact authenticated source/type-map contract, never name-tail inference.
   The Quorum family must be emitted at module-global `export namespace
   janus`, while ordinary crate items remain global `::rrr`.  `rrr::janus`,
   `rrr::QuorumEvent`, and namespace/type aliases have different mangling and
   are invalid substitutes.  The override covers `QuorumPolicy`,
   `QuorumEvent`, `QuorumEventWrapper`, their constructors/methods,
   `create_sp_quorum_event`, `quorum_event_make`, the private-but-strong
   quorum helpers, and Quorum RTTI/vtable identity.  Ambiguous or overlapping
   placement contracts must reject atomically.
2. **Absolute type maps.**  A leading `::` in an authenticated type-map target
   is semantic and must survive verbatim.  Only relative targets receive the
   configured C++ namespace prefix.
3. **Native C ABI.**  Exact `extern "C"` declarations preserve raw function
   pointer spelling `R (*)(...)`, calling convention, pointer mutability, and
   variadic `...`; unsupported forms reject rather than silently becoming
   `UnsafeFn` or losing arguments.  The global C identity and authenticated
   `LegacyCChar`/`LegacyCLong` mappings must remain exact.
4. **Private adapter namespace identity.**  `EventCore`'s forward declaration,
   definition, adapter declarations, and adapter definitions must inhabit the
   same exact lexical/anonymous namespace.  The compiler must not seed a root
   forward declaration for an anonymous private trait.
5. **Ownership and trait-object identity.**  Adapter signatures preserve the
   authenticated `std::sync::{Arc, Weak}` provenance and the
   `dyn EventPollable` target identity.  They must not fall back by leaf name
   to `rc::Weak` or collapse the target to `void*`; the historical native
   contract is `rusty::sync::Weak<EventPollable>`.  Associated calls such as
   downgrade must use the resolved coerced owner (`Arc<EventPollable>`), not a
   stale pre-coercion generic owner (`Arc<Ev>`).
6. **Boxed callable coercion.**  Expected owner/type comes from the explicitly
   typed waker field/local, not an enclosing return context.  A source
   `Box<dyn Fn() + Send + Sync>` used for native `Waker::wake_fn` must lower to
   its copyable `std::function<void()>` contract (or an equivalent copyable
   lambda conversion), not `rusty::Box` or move-only `rusty::Function`.
   Unsupported callable coercions must reject before output.
7. **Private helper linkage.**  If instantiated generic wake helpers produce
   ordinary strong symbols, the compiler needs an authenticated internal-
   linkage mechanism.  No helper-symbol exception is allowed in the exact
   incumbent manifest gate.
8. **Nominal alias fidelity.**  A resolved concrete alias in a by-value
   signature remains that concrete type; `auto` is only valid for an actual
   source generic.  This applies to all callable/task/channel aliases above.
   Unit in the authenticated native `TaskVoid` position maps to the native
   `Task<void>` specialization, and exported aliases such as `SrcFileCStr`
   honor their exact type map as well as their uses.
9. **Owning trait objects in ADTs.**  `Box<dyn PollableBase>` in a variant field
   and its factory remains `rusty::Box<PollableBase>` with the same move-only
   ownership/destructor contract.  Raw `void*` erasure is forbidden even when
   size/alignment happen to match.
10. **Synthetic adapter linkage.**  Generated UFCS/helper namespaces for
    imported/public and private traits must be inline, internal, or omitted as
    appropriate.  They may not add ordinary strong symbols absent from the
    incumbent owned manifest.
11. **Constructor-call lowering.**  A Rust associated constructor that lowers
    to a native C++ constructor must be invoked through the authenticated
    constructor/in-place path.  The compiler may not emit a nonexistent
    `Type::new_` static call; `Rc<Reactor>` construction must preserve the
    incumbent ownership and constructor semantics.

Promotion remains on HOLD until an unmodified regenerated provider passes
Clang module compilation, link, the exact 29-layout and 300-owned-strong-
symbol oracles (including all global `janus` identities), TLS and native
runtime races, ASan, TSan, and independent review.  Rust facade containers
intentionally do not claim the incumbent C++ storage layout.

Two configuration-fidelity items were also explicit promotion gates rather
than silently accepted constants: `REUSING_FIBER` was fixed true to match this
accepted build's `-DREUSE_FIBER`, and `SYS_gettid = 186` was the x86-64 Linux
value rather than the incumbent platform-header macro.  A
portable/configurable facade or target-conditional proof was required before a
cross-architecture/configuration claim.

**RESOLVED** — both are now answered by the reactor's own plain-C seam,
`reactor/srpc_fiber.c` (declared in `reactor/srpc_fiber.h`, which the
`rrr.reactor` module preamble already includes):

  * `srpc_reactor_gettid()` returns `syscall(SYS_gettid)` using
    `<sys/syscall.h>`'s number *for the target being compiled*.  No syscall
    number appears in portable source, so aarch64 (178) and i386 (224) are
    correct without a per-target source edit.  The variadic `syscall`
    declaration and its `LegacyCLong` facade are deleted from the canonical
    Rust along with it, retiring that half of native-C-ABI frontier item 2.
  * `srpc_reactor_reusing_fiber()` evaluates the incumbent predicate
    `#if defined(REUSE_FIBER) || defined(REUSE_CORO)` verbatim, in a
    translation unit compiled with the library's own flags.  A build without
    `-DREUSE_FIBER` therefore behaves as it always did.

The Rust side is a private `fn reusing_fiber() -> bool` and a private
`fn current_thread_gettid() -> i64`.  Deliberately *not* `pub`: the incumbent
public surface exposed a preprocessor macro, not an exported constant, and
`export constexpr bool REUSING_FIBER = true;` was both a surface addition and
a value frozen at library-build time under the name of a macro a consumer can
still define differently.  Removing the name from the module's export list
restores macro-equivalence at the public surface; the configuration answer
now lives in exactly one place, the compiled library.

Verified: generated `rrr.reactor.cppm` differs from the pre-fix generation by
39 lines and the other 35 generated modules are byte-identical; the emitted
construct (global-module-fragment include + module-purview `extern "C"`
redeclaration + the two facades) compiles, links and runs under clang 22.1.8
with the correct answer for both `-DREUSE_FIBER` and `-UREUSE_FIBER`; and the
Cargo lane is unchanged at 38 suites / 133 tests / 0 failed with identical
warning and error sets.
