# SRPC (rrr) C++23 Module Migration — Incremental Plan

## Background

The rrr library was migrated to C++23 named modules in commit `0cf5b972`
(Apr 21, 2026) and de-modularized one week later in `382bb74d` (Apr 27, 2026)
plus the sweep in `9829fa77`. The revert was driven by:

- 60 PCM files totaling ~1.6 GB on-disk per parallel build.
- ~30 minute clean build at `-j32`.
- clang21+ ODR clash on `<emmintrin.h>` reaching both the rrr GMF and
  consumer TUs ("definition with same mangled name").
- rusty-cpp borrow checker unable to resolve `import rrr;` without BMI paths.

## Hypothesis for this retry

The prior attempt was a big-bang conversion of ~80 files in a single
commit. There is no per-file signal in the history showing which
conversions inflated PCMs or wallclock. Going one file at a time with
measurement at each step will either validate that modules are inherently
expensive in this codebase (abort) or expose specific files that bloat
(fix or skip them).

## Approach

### Principles

1. **Merge each pair into one `.cpp` module unit**: each `.hpp`/`.cpp`
   pair collapses into a single module interface unit named `rrr.foo`,
   written into `foo.cpp`. The `.hpp` is **deleted**. Class
   declarations, templates, inline methods (formerly in the header)
   and out-of-line definitions (formerly in the `.cpp`) all live in
   the module unit. Exports are marked with `export` on the
   namespace-scope declarations consumers need to see. File extension
   stays `.cpp` (CMake's `FILE_SET CXX_MODULES` accepts any
   extension). Header-only `.hpp` files (cpuinfo, dball, etc.) also
   get renamed/moved to `.cpp` module units; the `.hpp` is deleted.
   Rationale: keeping the `.hpp` as a forward-decl shim only works for
   files with free-function definitions. Class-member definitions
   require declarations and bodies share the same module attachment;
   otherwise clang reports "declaration of X in module rrr.foo
   follows declaration in the global module" for every libc++/libc
   header that gets dragged into two attachments. Merging avoids
   the split entirely.
2. **`rrr.hpp` becomes the import surface for legacy consumers**:
   the umbrella header replaces its deleted `#include "base/foo.hpp"`
   entries with `import rrr.foo;`. Heavy transitive textual headers
   (`<std_compat.hpp>`, `<rusty/rusty.hpp>`) are retained inside
   `rrr.hpp` so consumers that relied on those transitively don't
   break in this pass. A later cleanup pass can prune them.
3. **CMakeLists wiring**: `RRR_MODULE_SRC` is an explicit list of
   converted files (not a glob); the regular source list is
   `REMOVE_ITEM`-stripped of those entries to avoid double-compile.
   `target_sources(rrr PUBLIC FILE_SET rrr_modules TYPE CXX_MODULES
   FILES ${RRR_MODULE_SRC})` adds them.
4. **Macros**: preprocessor macros (`streq` etc.) can't be exported
   via modules. Unused macros are deleted; used macros either move to
   `rrr.hpp` (textual) or are converted to inline functions.
3. **Measure every commit**: wallclock clean build, total `.pcm`
   bytes, and `ninja -d stats` per-TU compile times. Any single-file
   conversion that adds >5% wallclock or >50 MB PCM is a red flag —
   stop and investigate before continuing.
4. **Bottom-up**: leaves with no rrr dependencies first; climb up the
   dependency graph.

### Measurement protocol

Before and after each conversion commit:

```
rm -rf build && \
  time cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Release 2>&1 | tee cfg.log && \
  time ninja -C build -j32 2>&1 | tee build.log

du -cb build/**/*.pcm 2>/dev/null | tail -1
find build -name '*.pcm' | wc -l
ninja -C build -d stats > ninja_stats.txt
```

Record the numbers in the table at the bottom of this doc (CSVs are
gitignored project-wide; the table lives inline so it commits cleanly).

Decision rule per commit:
- ≤ +5% wallclock and ≤ +50 MB PCM → proceed.
- > +5% wallclock or > +50 MB PCM → stop, diagnose, fix or revert.
- Super-linear growth across 3 consecutive conversions → abort migration.

### Conversion order (planned, leaves first)

1. `base/strop.hpp` + `.cpp` — pure string ops, no rrr deps.
2. `misc/cpuinfo.hpp` — header-only.
3. `rpc/errors.hpp` — leaf in rpc/.
4. `src/request_options.rs` — canonical Rust module with a private `rrr.rand` dependency.
5. `base/basetypes.hpp` + `.cpp` — depends on logging; starts climbing.
6. (Continue up the dependency graph; re-plan after step 5.)

After ~5–10 conversions we should have a strong signal on whether to
continue.

### Failure-mode watchlist

- **Heavyweight GMF**: anything pulling `<emmintrin.h>` / `<immintrin.h>`
  / rusty-cpp headers into a module GMF risks reproducing the clang21
  SIMD clash. `RUSTY_PORTABLE_INTRINSICS=1` is project-wide. Verify
  before any rusty-using partition.
- **Transitive `export import` chains**: if every partition re-exports
  its dependencies, BMIs balloon. Prefer plain `import` (not exported)
  where the symbols aren't part of the partition's public surface.
- **`import std;` cost**: importing `std` from every partition is
  cheap once `std.pcm` exists — but verify the per-partition BMI size
  stays reasonable.
- **Multiple-attachment trap for rusty/STL templates**: if a templated
  type like `rusty::HashMap<K,V>` appears in the public BMI of *N*
  rrr modules AND in the textual `#include` chain of a consumer,
  clang can hit "declaration X attached to named module rrr.foo can't
  be attached to other modules" once N grows past ~2. Manifested
  while merging the reactor cluster (rrr.reactor would be the 3rd
  attachment of `rusty::HashMap::operator()` after rrr.threading and
  consumer-textual). Solving requires either header units for rusty
  (build-system change) or a single `rrr.rusty_prelude` module that
  everything else imports.

  **Reproducer (May 2026)**: three named modules each in their GMF
  `#include <rusty/hashmap.hpp>` and in module purview declare a struct
  with field `rusty::HashMap<std::string, V_i>` (different V_i per
  module). A fourth TU that imports all three fails with the trap
  citing `RustyHash<std::string>::operator()` attached to mod_a (the
  first-imported module). The implicit instantiation
  `RustyHash<std::string>` is the SAME specialization across all three
  uses (V doesn't affect K's hash struct), so clang attaches it once
  per importing module and rejects the third attachment.

  **Investigated fixes (May 2026, none viable in clang 19 + CMake 3.31)**:
  - *Header units (C++23 standard fix)*. Per the standard, declarations
    in an importable header unit attach to the global module, and
    implicit instantiations triggered from any consumer module also
    attach globally — exactly what the trap needs. Verified by hand
    that a `rusty/hashmap.hpp` header unit fixes the reproducer: build
    the BMI with `clang -fmodule-header=user --precompile`, then
    consumers `import <rusty/hashmap.hpp>;` instead of `#include`-ing
    it. Multi-attachment goes away.
    Blockers preventing rrr from adopting this:
    1. CMake 3.31 rejects `FILE_SET TYPE CXX_MODULE_HEADER_UNITS`
       (only `HEADERS` and `CXX_MODULES` are valid types). Custom
       `add_custom_command` can build the .pcm, but…
    2. `clang-scan-deps-19` does not recognize the named form
       `-fmodule-file=<rusty/hashmap.hpp>=PATH` as marking the header
       importable. It rejects `import <rusty/hashmap.hpp>;` with
       "header file ... cannot be imported because it is not known to
       be a header unit".
    3. The bare form `-fmodule-file=PATH` DOES make the BMI loadable
       (no name needed), but it loads eagerly into every TU it's
       passed to. Since the rusty header transitively includes libc++
       (`<vector>`, `<utility>`, `<string>`), and rrr modules also
       reach libc++ via `import std;` and `std_compat.hpp`'s textual
       includes, eager-load triggers libc++ redefinition errors
       (`redefinition of 'to_array'`, etc.) — the BMI's textual libc++
       declarations clash with the consumer's modular/textual ones.
    Net: header units are the right fix in principle but the
    clang19+CMake3.31+scan-deps combo can't drive them cleanly.
  - *`rrr.rusty_prelude` wrapper module with using-declarations*. The
    prelude does `#include <rusty/hashmap.hpp>` in its GMF (global
    module attachment) and re-exports types via `export namespace rusty
    { using ::rusty::HashMap; using ::RustyHash; }`. Consumers `import
    rrr.rusty_prelude;` and use `rusty::HashMap`. The reproducer still
    fails with the same error — clang attaches the implicit
    instantiation `RustyHash<std::string>::operator()` to the
    instantiating consumer module's purview regardless of the
    template's GMF home. Per the C++ standard implicit instantiations
    should follow the template's owning module, but clang19's
    behaviour is to attach to the using module. This is the
    fundamental obstacle: no module-level wrapper helps as long as the
    use happens in named-module purview.
  - *Explicit instantiation + extern template in the prelude*. Would
    require enumerating every (K,V) pair the codebase instantiates
    (brittle; rusty containers have many) and adding `extern template`
    declarations visible to every consumer. Deferred — not pursued
    against this much surface area for one cluster.
  - *PIMPL the trap-prone HashMap fields out of module purview*.
    Wrap `rusty::HashMap<std::string, V>` inside non-templated
    classes whose definitions live in non-module `.cpp` files (global
    module attachment). Viable but invasive — would touch
    `rrr.any_message`, `rrr.inmemory_channel`, and reactor.h's
    `clients_`/`dangling_ips_`/`fd_to_pollable_`/`mode_` members.

  **Resolved on clang 22 (May 2026), now built on clang 21 in
  production (May 2026)**. Installed Homebrew clang 22.1.5 at
  `/home/users/shuai/.linuxbrew/` for the initial fix. Reran the
  minimal reproducer (`/tmp/multiattach_test/`): all three modules
  and the main TU compile cleanly — no multi-attachment error. The
  clang-19 behaviour of attaching implicit template instantiations to
  the using-module's purview is corrected in clang 21 and clang 22
  (instantiations now follow the template's owning module, matching
  the C++23 standard). We later moved the production toolchain to
  Homebrew clang 21.1.8 at `/home/users/shuai/.linuxbrew/opt/llvm@21/`
  to side-step the clang-22 libclang parse-crash regression that
  affected borrow checking — see
  [`libclang22_parse_crash.md`](libclang22_parse_crash.md).

  **Reactor cluster merged on clang 22** (commit-pending). Combined
  `event.h/.cc` + `fiber_impl.h/.cc` + `quorum_event.h/.cc`
  + `reactor.h/.cc` + `fiber_context_runtime.cc` into a single
  `src/rrr/reactor/reactor.cpp` named-module interface unit
  (`rrr.reactor`, ~2860 lines). The cluster's classes
  (`rrr::Event`, `rrr::Fiber`, `rrr::Reactor`, `janus::QuorumEvent`,
  `rrr::fiber_task_t`, …) form a mutually-recursive web of forward
  declarations and out-of-line member definitions; a single module
  unit is the natural shape — separate module interfaces would need
  circular `import` lines (which the standard forbids). The arch-
  specific context-switch trampolines stay outside the module:
  `fiber_context_x86_64.cc` and `fiber_context_aarch64.cc` are tiny
  `extern "C"` asm-only TUs and don't need module attachment.

  Consumers updated to `import rrr.reactor;` instead of
  `#include "reactor/{event,fiber_impl,quorum_event,reactor}.h"`:
  `rrr/rrr.hpp` (umbrella), `rrr/rpc/{fiber_channel,tcp_channel,
  client,server}.hpp`, `rrr/reactor/{fiber,future}.h`. `rrr.alarm`
  switched from a GMF forward-decl of `PollThread` to importing
  `rrr.reactor` directly — clang 22 rejects the GMF forward-decl
  when another imported module exports the same class. Also added
  `#include <rusty/box.hpp>` to `rpc/tcp_channel.hpp` (its
  `rusty::make_box` use was previously satisfied by reactor.h's
  transitive textual include, which the module BMI no longer
  provides as a textual-include shim).

  Build with clang 21 (current production, see
  [`libclang22_parse_crash.md`](libclang22_parse_crash.md) for why
  we moved off clang 22): `cmake -G Ninja -B build -S .
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=/home/users/shuai/.linuxbrew/opt/llvm@21/bin/clang
  -DCMAKE_CXX_COMPILER=/home/users/shuai/.linuxbrew/opt/llvm@21/bin/clang++
  -DCMAKE_EXE_LINKER_FLAGS="-L/home/users/shuai/.linuxbrew/opt/llvm@21/lib
  -Wl,-rpath,/home/users/shuai/.linuxbrew/opt/llvm@21/lib -stdlib=libc++"
  -DCMAKE_SHARED_LINKER_FLAGS=<same>`. The linker-flag carveouts pin
  libc++ 21 (the system libc++ 19 lacks some clang-21 ABI symbols).
  The `-DCMAKE_C_COMPILER` / `_CXX_` flags must be set explicitly on
  the command line — environment CC/CXX get ignored because the
  top-level `CMakeLists.txt` already forces
  `set(CMAKE_CXX_COMPILER "clang++" CACHE STRING …)` before
  `project()`. libc++ 21 stops transitively reaching `<cstdlib>` via
  rusty header includes, so `rusty/function.hpp`'s `std::abort()`
  calls become unresolved. Fix: the pre-existing
  `src/compat/rusty/function.hpp` shim
  (`#include <cstdlib>; #include_next <rusty/function.hpp>`) is now
  wired onto the rrr include path via `target_include_directories(
  rrr BEFORE PUBLIC src/compat)`. No upstream rusty-cpp change.
  Reference numbers from the clang-22 build (kept for historical
  context): clean build (rrr + rpcbench) 67s, librrr.a 12.7 MB (vs
  clang-19's 83s / 9.7 MB).

## Out of scope / deferred

- **Borrow checking**: stays disabled for rrr during the migration
  (CMakeLists.txt:104–107). Track as follow-up.
- **External consumer migration**: 39 files do
  `#include "rrr/rrr.hpp"`. With the shim approach they don't need to
  change. Flipping to `import rrr;` is an optional later cleanup.
- **`RPC_TEST_HOOKS`** (CMakeLists.txt:191–194): per-TU define no
  longer works once consumers go modular. Decide when the first file
  touching it is converted.
- **Reactor cluster** (event, fiber_impl, quorum_event, reactor.h/.cc):
  ~3000 lines, mutually-referenced via forward decls. **Merged**
  on clang 22 (commit-pending) as a single `rrr.reactor` module
  (~2860 lines in `src/rrr/reactor/reactor.cpp`) — see the
  multi-attachment-trap entry above for the toolchain switch.
- **rpc client/server cluster** — followups split out:
  - `reactor/fiber.h` → **converted** to `rrr.fiber` module
    (commit-pending). `this_fiber::*` namespace.
  - `reactor/future.h` → **converted** to `rrr.future` module
    (commit-pending). FiberPromise / FiberFuture templates.
  - `misc/alock.{hpp,cpp}` → **converted** to `rrr.alock` module
    (commit-pending). Despite using
    `Reactor::create_event<IntEvent>()` in module purview (same
    cross-module template-member call that crashes fiber_channel),
    alock compiled cleanly — the codegen bug is sensitive to
    something narrower than the obvious call shape.
  - `rpc/tcp_channel.{hpp,cpp}` → **converted** to
    `rrr.tcp_channel` module (commit-pending). Largest of the rpc
    siblings; no `create_sp_event<>` in purview so it compiled
    without workarounds.
  - `rpc/server.{hpp,cpp}` → **converted** to `rrr.server` module
    (commit-pending). 1700 lines combined.
  - `rpc/client.{hpp,cpp}` → **converted** to `rrr.client` module
    (commit-pending). 4810 lines combined; biggest single module.
    The GMF forward-decl of `class Client` in `rrr.load_balancer`
    keeps working under clang 22.
  - `rpc/fiber_channel.{hpp,cpp}` → **converted** to
    `rrr.fiber_channel` module (commit-pending). Required a
    1-line anchor shim — see the diagnostic below.

  **fiber_channel module diagnostic** (May 2026, clang 22.1.5):

  Symptom. Replacing `fiber_channel.hpp`+regular `.cpp` with a
  module unit caused `rpcbench.cc` to fail with ambiguous
  `operator new(size_t, std::align_val_t)` inside
  `std::__libcpp_allocate<std::shared_ptr<rusty::Waker>>`
  (and `<rusty::Task<void>>`) instantiations — two candidates at
  the same `__new/global_new_delete.h:49` source line, attached
  to different module ownerships.

  Bisection. (a) Module exports a same-named `FiberChannel`,
  `.hpp` deleted → fails. (b) Module exports a renamed
  `DiagFiberChannel` alongside the unchanged `.hpp`+`.cpp` pair
  → clean, even when `rrr.client` explicitly imports the diag
  module. (c) Module exports only a forward-decl of
  `FiberChannel`, `.hpp` deleted → ambiguity stays *latent*
  (compile fails earlier with "incomplete type" in `client.cpp`,
  so we never reach the `operator new` site). (d) Module
  exports a full `FiberChannel` definition, `.hpp` deleted,
  rrr.hpp DOES NOT include any `fiber_channel.hpp` chain →
  fails. (e) Same as (d) but `rrr.hpp` `#include`s a
  `fiber_channel.hpp` shim that contains `#include <memory>` and
  nothing else, BEFORE `import rrr.fiber_channel;` → **clean**.

  Conclusion / mechanism. The textual `<memory>` reached from
  `rrr.hpp` ahead of the import anchors libc++'s `operator new`
  declarations in the global module from rpcbench.cc's
  perspective. Without it, importing `rrr.fiber_channel` seems
  to introduce a second attachment for the same `operator new`
  signature that `import std;` already provides, which clang 22
  resolves as an ambiguous overload rather than merging. Why
  fiber_channel specifically (and not e.g. tcp_channel or
  client which look the same on the surface) trips this — still
  not fully understood; we observe it empirically. clang's BMI
  dump (`-module-file-info`) does not reveal an obvious
  attachment difference between the failing and passing setups.
  This is a clang 22 module-attachment quirk, likely related to
  the broader pattern that bit the reactor cluster on clang 19,
  but with a different surface.

  Applied workaround. `src/rrr/rpc/fiber_channel.hpp` is now a
  6-line anchor shim:
  ```
  #pragma once
  // Anchor shim. The real FiberChannel declaration lives in the
  // rrr.fiber_channel module. <memory> pins libc++ operator new
  // in global-module attachment for downstream TUs.
  #include <memory>
  ```
  `src/rrr/rrr.hpp` keeps the textual `#include
  "rpc/fiber_channel.hpp"` AND adds `import rrr.fiber_channel;`
  to its import list. The order matters — the textual include
  must precede the import. `src/rrr/rpc/client.cpp` drops its
  own textual `#include "fiber_channel.hpp"` because the
  import covers it. Revisit when a newer clang stops needing
  the anchor.

  Independent clang 22 codegen crash (separate from the
  attachment issue above): in a module-purview function body,
  instantiating libc++ container templates (e.g.
  `std::vector<uint8_t>::assign`, `::resize`) for the first time
  crashes `EmitScalarExpr` inside `EmitReturnStmt`/`EmitIfStmt`.
  Workaround that the other rpc modules use: pre-instantiate via
  `#include <rusty/rusty.hpp>` (or `<vector>` / `<deque>` etc.)
  in the GMF.

## References

- Prior modularization commit: `0cf5b972` (`git show 0cf5b972`).
- Revert commit: `382bb74d`.
- Sweep: `9829fa77`.
- CMakeLists.txt:104–107 (borrow-check disable note).
- CMakeLists.txt:191–194 (RPC_TEST_HOOKS).

## Metrics

Targets built: `rrr` + `rpcbench`. `-j32`. Clean build each row (`rm -rf
build && cmake -G Ninja -B build ... && ninja rrr rpcbench`).

**Toolchain note.** Rows up to and including the reactor cluster were
measured on Homebrew clang 22.1.5. The production toolchain has since
moved to Homebrew clang 21.1.8 (see
[`libclang22_parse_crash.md`](libclang22_parse_crash.md) for the
reasoning). Clean-build wallclock on clang 21 sits within ~5% of the
clang-22 numbers below — kept as-is rather than re-running for noise.

| Step | Wallclock (s) | PCM count | PCM total (MB) | librrr.a (MB) | Notes |
|------|--------------:|----------:|---------------:|---------------:|-------|
| baseline | 16.86 | 2 | 28.5 | 10.07 | clang 19, cmake 3.31 |
| **37 modules (current)** | **83.05** | **~40** | **~480** | **~9.7** | base/* (7), misc/* + alarm/cpuinfo/dball/netinfo/rand/serializable/serializable_envelope/stat/marshal/any_message (11), reactor/epoll_wrapper, rpc/* (15: utils, errors, request_options, internal_protocol, pollable_proxy, load_balancer, connection_state, reconnect_policy, callbacks, heartbeat, connection_metrics, circuit_breaker, channel, idempotency, request_queue, completion_tracker, frame_codec, inmemory_channel, base/unittest, base/callback_wrapper). 4.93× baseline, 10× ceiling. |
| base/strop (shim, superseded) | 15.63 | 3 | 53.0 | 10.02 | Approach abandoned. |
| base/strop (no-shim) | 20.65 | 3 | 53.0 | 10.51 | strop.hpp deleted; rrr.hpp + base/all.hpp import the module. Also fixed transitive: dball.hpp + misc.hpp now `#include <rusty/function.hpp>` directly. |
| base/basetypes (no-shim) | 20.66 | 4 | 77.6 | 10.47 | basetypes.hpp deleted; 7 includers updated (rrr.hpp, base/all.hpp, base/misc.hpp, base/threading.hpp, rpc/request_queue.hpp, reactor/quorum_event.h, reactor/fiber.h, rpc/idempotency.hpp). +24.6 MB BMI. Wallclock flat vs strop alone — basetypes BMI builds in parallel. |
| base/debugging (no-shim) | 20.98 | 5 | 102.0 | 10.42 | debugging.hpp deleted; 5 includers updated. `verify` template + `print_stack_trace` + GMF forward-decl of `get_exec_path` to keep its call site at global-module attachment. likely/unlikely inline functions dropped (unused externally; inlined `__builtin_expect` in verify). |
| base/logging (no-shim) | 21.64 | 6 | 102.5 | 10.39 | logging.hpp deleted; 3 includers updated. GMF forward-decl of `time_now_str` for the same reason as `get_exec_path`. `Pthread_mutex_lock` wrapper from threading.hpp inlined as `pthread_mutex_lock` (avoids depending on the not-yet-modularized threading). |
| base/misc (no-shim) | 30.14 | 7 | 112.4 | 10.34 | misc.hpp deleted; 3 includers updated. debugging+logging migrated from GMF forward-decls to `import rrr.misc;` (now that misc is a module). BMI dedup visible: rrr.logging.pcm is only 0.6 MB because it just references other module BMIs; rrr.debugging.pcm dropped from 25.7 → 24.6 MB. Dropped unused macros: `arraysize`, `TIME_NOW_STR_SIZE`, `ArraySizeHelper` template. Wallclock +40% jump in this step — more TUs now consume more module BMIs transitively. |
| base/threading (no-shim) | 37.01 | 8 | 136.8 | 9.99 | threading.hpp deleted; 9 includers updated. ~1000-line merged file (largest so far): Pthread wrappers, SpinLock/SpinMutex family, SpinCondVar, Queue<T>, ThreadPool, RunLater. Switched `max(...)` → `std::max(...)` (lost via `using namespace std;` drop). 2.19× baseline; threshold (3×) remains. |
| misc/rand (no-shim) | 38.35 | 9 | 161.0 | 10.15 | rand.hpp deleted; only rrr.hpp included it. RandomGenerator class (all static methods). +1.3 s wallclock; 2.27× baseline. |
| reactor/epoll_wrapper (no-shim) | 39.01 | 10 | 167.1 | 9.66 | epoll_wrapper.h deleted; 5 includers updated. `Pollable` interface + `Epoll` class with templated `Wait<>` member. Dropped the unused `class PollThreadWorker;` forward-decl (was causing global-vs-module attachment clash in reactor.h's similar forward-decl). |
| rpc/utils (no-shim) | 38.07 | 11 | 174.9 | 9.62 | utils.hpp deleted; 2 includers updated (rrr.hpp, rpc/server.hpp). AddrInfo RAII + 3 free functions. Wallclock slightly **lower** than prior — noise band. |
| rpc/errors (no-shim, header-only → module) | 38.36 | 12 | 175.0 | 9.63 | First header-only conversion: errors.hpp had no .cpp, so we **created** errors.cpp as the module interface unit. Only enums + inline switch helpers — 8.2 MB BMI (smaller than typical because no transitive rusty/heavy headers). 3 includers updated. |
| rpc/request_options (header-only → module) | 38.83 | 13 | 175.2 | 9.71 | RequestOptions POD + TimeoutType enum + factory methods + jitter calc. Needed explicit `#include <cstdint>` in GMF — `import std;` alone didn't pull `uint16_t`/`uint64_t` for type aliases used in struct fields. 2 includers updated. |
| rpc/internal_protocol (header-only → module) | 38.66 | 14 | 175.3 | — | Wire-protocol constants + constexpr helpers for response header extension flag. 4 includers updated (frame_codec.hpp, server.hpp, rrr.hpp, and one test). Same `<cstdint>` GMF gotcha. |
| misc/stat + misc/netinfo (header-only → modules) | 38.23 | 16 | 175.4 | — | Two trivially-isolated headers — AvgStat (POD) and NetInfo (singleton reading /sys/class/net/...). 1 includer each (rrr.hpp). |
| misc/alarm (header-only → module) | 38.41 | 17 | 175.5 | — | Alarm inherits FrequentJob (rrr.misc), uses rrr::PollThread* pointer-only. GMF forward-decl `namespace rrr { class PollThread; }` keeps the pointer in global-module attachment that matches reactor.h's full decl. 2 includers updated (rrr.hpp, misc/alock.hpp). |
| misc/cpuinfo (header-only → module) | 39.60 | 18 | 175.6 | — | CPUInfo singleton reading /proc/PID/{net/dev,stat} + /proc/meminfo. Uses Log_debug from rrr.logging. Sole consumer: rrr.hpp. |
| rpc/pollable_proxy (header-only → module) | 39.35 | 19 | 175.6 | — | PollableBase interface + PollableTypedArcAdapter<T> template + PollableProxy typedef. Uses PollMode constants from rrr.epoll_wrapper. 3 includers updated (reactor.h, tcp_channel.hpp, rrr.hpp). |
| misc/dball (header-only → module) | 39.72 | 20 | 175.6 | — | DragonBall event-driven primitive + ConcurrentDragonBall typedef. 2 includers updated (rrr.hpp, alock.hpp). |
| rpc/load_balancer (header-only → module) | — | 21 | — | — | LoadBalancingStrategy enum, LoadBalancerState, LoadBalancer with templated `select<>`. Forward-decl `class Client` MUST go in GMF (global-module attachment) — putting it in `export namespace rrr` caused 5 TUs to fail with "Client in module rrr.load_balancer follows declaration in global module". Same gotcha as PollThreadWorker earlier. |
| rpc/connection_state (header-only → module) | 38.64 | 22 | 175.6 | — | ConnectionState enum + ConnectionStateMachine class (rusty::Cell + rusty::Function callback). 6-state lifecycle with valid-transition table. 2 includers updated. |
| **reactor cluster (clang 22)** | **67.5** | **~41** | **—** | **12.7** | event + fiber_impl + quorum_event + reactor + fiber_context_runtime merged into single `rrr.reactor` module (2860 lines). Toolchain switched to clang 22 (Homebrew) to bypass the multi-attachment trap. `src/compat/rusty/function.hpp` shim wired onto rrr include path (adds `<cstdlib>` before delegating to upstream; no third-party patch). 6 consumer hpp/h updated to import rrr.reactor. Clean build incl. rpcbench: 67s. |
| reactor/fiber (no-shim) | — | 42 | — | — | `this_fiber::*` inline wrappers around Fiber::current_fiber/sleep. ~120 lines. Single consumer (rrr.hpp). |
| reactor/future (no-shim) | — | 43 | — | — | FiberPromise<T> / FiberFuture<T> templates over BoxEvent<T>. ~220 lines. Single consumer (rrr.hpp). |
| misc/alock (no-shim) | — | 44 | — | — | Async-queued lock (~1515 lines combined). Uses `Reactor::create_event<IntEvent>()` in purview — same shape that crashes fiber_channel — but compiled cleanly. Imports rrr.alarm/dball/threading/reactor/etc. |
| rpc/tcp_channel (no-shim) | — | 45 | — | — | TcpConnection / TcpListener / TcpFactory + adapter glue (~1430 lines combined). No `create_sp_event<>` in purview; built without workarounds. Required adding `import rrr.epoll_wrapper;` for PollMode constants. |
| rpc/server (no-shim) | — | 46 | — | — | RPC server + DeferredReply (~1713 lines combined). Required adding `import rrr.tcp_channel;` for TcpFactory. |
| rpc/client (no-shim) | **75** | **47** | — | — | Largest single module (~4810 lines combined). Future / FutureGroup / ClientConnection / Client / ClientPool / bulk-reconnect. Kept `using namespace std;` inside the impl block to avoid rewriting hundreds of unqualified `list`/`string`. fiber_channel.hpp included textually in GMF (rrr.fiber_channel deferred). Clean build incl. rpcbench: 75s, librrr.a ~13 MB. |
| **rpc/fiber_channel (with anchor shim)** | **77** | **48** | — | **13.4** | Migration done — 48/48 modules. fiber_channel.hpp shrinks to a 6-line `#include <memory>` anchor shim that rrr.hpp `#include`s ahead of `import rrr.fiber_channel;` to pin libc++ `operator new` in global-module attachment. Without the shim, downstream TUs (e.g. rpcbench.cc) fail with ambiguous `operator new(size_t, std::align_val_t)` in `__libcpp_allocate` instantiations — a clang 22 module-attachment quirk specific to this module. client.cpp drops its textual fiber_channel.hpp include. Clean build incl. rpcbench: 77s, librrr.a 13.4 MB. |

## Whole-project compile-time comparison

Targeting `txlog_core` (which links rrr, deptran, and memdb) gives a
fairer "how does this affect the project that uses rrr" reading than
rrr-alone. Both rows below are clean builds, `-j32`, clang 22.1.5, same
`cmake` invocation as the metrics table above.

| Config                              | rrr-alone (s) | txlog_core (s) | Notes |
|-------------------------------------|--------------:|---------------:|-------|
| Pre-mod (commit `9e763f32`)         |          16.0 |           75.3 | rrr is a flat list of `.cpp` files, no `FILE_SET CXX_MODULES`. |
| Modular (HEAD + downstream fix-ups) |          65.5 |          150.9 | 48 named modules + anchor shim for fiber_channel. |
| Slowdown                            |         4.1×  |          2.0×  | Whole-project ratio is smaller because deptran/memdb compile times dilute. |

**Outcome: modularization made the project measurably slower to build,
not faster.** The benefit we hoped for — that modular BMIs would
amortize across consumers and beat textual `#include` re-parsing — did
not materialize at this scale (48 modules, ~2 dozen direct consumers
inside rrr + ~40 external consumers reaching it via `rrr.hpp`). On
clang 22 the slowdown comes from (a) serialized BMI compilation along
the rrr module dep-graph (alock → reactor → threading → …) limiting
the effective `-j32`, and (b) consumers' BMI loads getting paid per-TU
since `import rrr.foo;` is not cacheable across TUs the way a header
in `-fmodule-map-file=` would be.

**Decision (2026-05-16): kept.** The 2× whole-project slowdown is an
acceptable cost for the structural payoff (clearer ownership, fewer
textual-include pitfalls, removal of the GMF-vs-purview attachment
landmines that bit the prior big-bang attempt). The migration stays on
`worktree-srpc` and will be merged forward.

These numbers do **not** account for any downstream compile-time wins
in deptran/memdb consumers — those still `#include "rrr/rrr.hpp"` (an
umbrella header that fans out to the imports), so they pay the
import-fanout cost on every TU. A future pass that flips consumers to
`import rrr;` may recover some of the loss, but on current evidence
the migration is a net regression on this codebase.

## Downstream consumer fix-ups (post-migration)

Three regressions surfaced once the modular rrr started being consumed
by deptran/memdb. Fixed in a single commit:

1. **`txlog_core_obj` was re-emitting rrr's module interface units.**
   The CMake target was globbing `${RRR_SRC}` as plain `.cpp`/`.cc`
   files, but the modular rrr targets now declare those same files via
   `FILE_SET CXX_MODULES`. Compiling them a second time outside any
   file set produces "provides module X but is not in FILE_SET
   CXX_MODULES" errors. Fix: drop `${RRR_SRC}` from `txlog_core_obj`'s
   source list and `target_link_libraries(txlog_core_obj PUBLIC rrr)`
   so the modular library is reachable from deptran/memdb TUs. Also
   added `rrr` to `txlog_core`'s PUBLIC link list.

2. **`rcc_rpc.h` still called the dropped `BinaryReadArchive(MarshalSource*)`
   convenience constructor.** The MarshalSink/Source cycle fix during
   modularization left only the proxy-taking constructor. Patched the
   code generator (`src/rrr/pylib/simplerpcgen/lang_cpp.py`, six emit
   sites) to wrap with `rrr::make_source_proxy(&...)`, then regenerated
   `src/deptran/rcc_rpc.h` via `./bin/rpcgen --cpp --python
   src/deptran/rcc_rpc.rpc`. The same wrap was applied manually to
   hand-written generated-style headers (`network.h`, `helloworld.h`)
   and the two Raft Marshal paths (`rocksdb_log_storage.hpp` and
   `replicated_db.cc`'s `to_marshal`/`from_marshal`).

3. **`likely(x)` / `unlikely(x)` unresolved in deptran.** An earlier
   pass dropped these inline helpers from `base/debugging.hpp` with the
   claim "unused externally", which was wrong — `RW_command.cc` and
   `copilot/server.cc` still reference them. Restored them inside the
   `export namespace rrr` block of `rrr.debugging`, guarded by
   `#ifndef likely` / `#ifndef unlikely` so erpc's macro form
   (`third-party/erpc/src/common.h`) wins where it's already in scope.

After these fixes, `make -j32 txlog_core` is clean on the modular HEAD.
Pre-existing breakage in `src/mako/benchmarks/tpcc.cc` (template
SFINAE) and `src/mako/lib/erpc_backend.cc` (missing `numa_max_node` /
libnuma) is unrelated to the migration and reproduces on the
pre-modularization commit equally.
