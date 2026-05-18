# rrr safety annotation push — plan to reach 80% @safe LOC

## Goal

Grow the share of rrr LOC that is explicitly `// @safe` (function-level
annotation, or class/namespace inheritance) from the current ~6% to ≥80%
of in-function LOC across the 45 borrow-checked module units.

Baseline (computed 2026-05-18 with `/tmp/safety_loc.py`):

| | LOC | % of fn body |
|---|---:|---:|
| Total LOC (45 modules) | 23,446 | — |
| Inside function bodies | 22,438 | 100% |
| @safe (function or class-inherited) | 1,435 | 6.4% |
| @unsafe (function or class-inherited) | 2,526 | 11.3% |
| inner `@unsafe { ... }` blocks | 733 | 3.3% |
| unannotated (namespace default = @unsafe) | 17,744 | 79.1% |

The unannotated bucket is what we can move into @safe via labeling and
modest refactoring. The borrow check already passes 45/45 clean today —
none of that code violates the analyzer's strict rules; it just hasn't
been tagged with author intent.

## Honest assessment of the 80% target

**80% is a stretch goal.** Realistic ceiling with the existing rusty-cpp
analyzer expressiveness and the Marshal wire-protocol design is closer
to 70–75%. Reaching 80% would require either:

1. Rewriting Marshal byte access onto a `Cursor<Vec<u8>>` pattern
   (perf cost on the hot wire path), or
2. Extending rusty-cpp with external annotations for trusted unsafe
   helpers, or
3. Moving Marshal to a separate `rrr.marshal_unsafe` module that we
   accept stays @unsafe (~500 LOC quarantined).

We will pursue 80% on paper but treat **70%** as the success criterion
for this push, with the remaining 5–10 pp marked as known-quarantine
zones (fiber context switching, raw socket fds, Marshal byte ops).

## Phases

### Phase 0 — Fix the LOC script

Before doing any conversion work, fix `/tmp/safety_loc.py` so its baseline
is honest. The script currently undercounts out-of-class method
definitions (`ClientPool::foo() {`) that should inherit their class's
annotation but are mis-classified as unannotated. Best estimate is this
alone moves the headline by +4–6 percentage points.

**Acceptance**: re-running the script reports a baseline within ±1pp of
the manually-computed truth on `rpc/client.cpp` (where we have full
context from Tier 1–5 work).

### Phase 1 — Labeling sweep (target ratio: 35–40%)

No refactoring; just apply class-level and namespace-level `// @safe`
to bodies that are already safe in fact but lack the tag. Each commit
should land 1–3 classes or 1 file.

**Class-level `// @safe` candidates** (ordered by LOC payoff):

- [ ] `Server` (rpc/server.cpp) — mirror what Tier 4 did for `Client`.
  Methods using sockets / `Pthread_*` keep method-level `// @unsafe`.
  Expected gain: ~600 LOC.
- [ ] `Reactor` (reactor/reactor.cpp) — large; the fiber context-switch
  methods (`Fiber::yield_`, `Fiber::continue_`, `Fiber::run`) keep
  method-level `// @unsafe`. Loop and check_timeout get @unsafe blocks
  for the few raw ops. Expected gain: ~1,500 LOC.
- [ ] `IdempotencyTracker` (rpc/idempotency.cpp) — already 199 safe /
  264 unannotated. Class-level @safe + a few method overrides should
  push to >90% safe. Expected gain: ~260 LOC.
- [ ] `CompletionTracker` (rpc/completion_tracker.cpp) — similar shape.
  Expected gain: ~210 LOC.
- [ ] `CircuitBreaker` (rpc/circuit_breaker.cpp). Expected gain: ~150 LOC.
- [ ] `HeartbeatManager` (rpc/heartbeat.cpp). Expected gain: ~200 LOC.
- [ ] `ConnectionStateMachine` (rpc/connection_state.cpp). Expected gain:
  ~150 LOC.
- [ ] `TcpListener` (rpc/tcp_channel.cpp) — the listener half is mostly
  safe; the `TcpConnection` half stays @unsafe. Expected gain: ~400 LOC.
- [ ] `LoadBalancer` (rpc/load_balancer.cpp). Expected gain: ~100 LOC.
- [ ] `RequestQueue` (rpc/request_queue.cpp) — partial Tier 2 already.
  Class-level @safe completes it. Expected gain: ~80 LOC.

**Namespace-level `// @safe` candidates** (whole files where every
function should be @safe):

- [ ] `rpc/inmemory_channel.cpp` — 844 LOC, zero annotations today.
  All rusty internals, no syscalls. Expected gain: ~800 LOC.
- [ ] `rpc/frame_codec.cpp` — 335 LOC unannotated. Expected gain: ~330.
- [ ] `rpc/internal_protocol.cpp` — small. Expected gain: ~80.
- [ ] `rpc/request_options.cpp`. Expected gain: ~100.
- [ ] `rpc/connection_metrics.cpp`. Expected gain: ~250.
- [ ] `rpc/callbacks.cpp`. Expected gain: ~100.
- [ ] `rpc/errors.cpp`. Expected gain: ~80.
- [ ] `rpc/utils.cpp` — has `getaddrinfo()`; needs per-method @unsafe.
  Expected gain: ~120.
- [ ] `rpc/pollable_proxy.cpp`. Expected gain: ~50.
- [ ] `rpc/reconnect_policy.cpp`. Expected gain: ~150.
- [ ] `misc/serializable_envelope.cpp`. Expected gain: ~200.
- [ ] `misc/netinfo.cpp`. Expected gain: ~50.
- [ ] `misc/stat.cpp`. Expected gain: ~80.
- [ ] `misc/cpuinfo.cpp`. Expected gain: ~150.
- [ ] `misc/rand.cpp`. Expected gain: ~30.
- [ ] `misc/dball.cpp`. Expected gain: ~100.
- [ ] `misc/alarm.cpp`. Expected gain: ~80.
- [ ] `base/basetypes.cpp` — POD types only. Expected gain: ~470.
- [ ] `base/debugging.cpp`. Expected gain: ~100.
- [ ] `base/strop.cpp`. Expected gain: ~100.
- [ ] `base/callback_wrapper.cpp`. Expected gain: ~80.
- [ ] `base/misc.cpp`. Expected gain: ~100.
- [ ] `base/unittest.cpp`. Expected gain: ~100.
- [ ] `reactor/epoll_wrapper.cc` — has epoll syscalls; needs per-method
  @unsafe. Expected gain: ~150.

Estimated Phase 1 gain: ~7,000–8,000 LOC. Resulting ratio: **~40%**.

### Phase 2 — Easy raw-pointer refactors (target: 50%)

1. [ ] `ChannelConnectionProxy` / `ChannelFactoryProxy`: change the
   underlying `std::unique_ptr<Base>` to `rusty::Box<Base>` at the
   channel-layer boundary; mark class @safe.
2. [ ] `Reactor::PollThreadWorker*` (the raw class-static thread_local)
   → `rusty::Weak<PollThreadWorker>` with `upgrade()` at call sites.
3. [ ] `rusty::sys::*` syscall wrappers for the top syscalls used:
   `nanosleep`, `usleep`, `pthread_*`, `epoll_*`. Each wrapper is
   marked @safe with internal @unsafe blocks. Eliminates many bare
   `// @unsafe` annotations across rrr.
4. [ ] `ServiceProxy::__get_service__()` returning raw `Service*`
   → return `rusty::Arc<Service>` (or pass-by-reference where the
   callback semantics allow).

Estimated Phase 2 gain: ~2,000 LOC + invalidates ~300 LOC of inner
@unsafe block markers. Resulting ratio: **~48–52%**.

### Phase 3 — Targeted refactors of remaining unsafe paths (target: 65–70%)

1. [ ] `alock.cpp::WaitDieALock`: change `ALock*` raw-pointer storage
   in `tolock_` / `locked_` BTreeMaps to `rusty::Weak<ALock>` with
   `upgrade()` at the use sites in `unlock()` / `abort_all_locked()`.
2. [ ] `serializable.cpp`: replace `std::shared_ptr<Marshallable>`
   boundary with `rusty::Arc<Marshallable>`. Touches generated
   `rcc_rpc.h` — needs `pylib/simplerpcgen/lang_cpp.py` codegen update.
   This is the largest single refactor; estimate 1-2 weeks of careful
   work because every existing RPC service definition is downstream.
3. [ ] Reactor::loop and `process_stackless_tasks`: tighten @unsafe
   block scoping so most of the body is @safe.
4. [ ] Threading helpers: rename `Pthread_*` macro wrappers as
   `rusty::sync::*` safe wrappers; downstream callers get @safe.

Estimated Phase 3 gain: ~3,500 LOC. Resulting ratio: **~65–70%**.

### Phase 4 — Stretch: chase the last 10% (target: 80%)

1. [ ] Marshal byte ops — choose one of:
   - rewrite to `Cursor<Vec<u8>>` (perf cost; benchmark first).
   - extend rusty-cpp external annotations for trusted byte-level
     helpers.
   - quarantine `marshal.cpp` byte-ops into a separate
     `rrr.marshal_unsafe` submodule; the rest of marshal becomes
     @safe.
2. [ ] Fiber context switching (`fiber_context_x86_64.cc` +
   `Fiber::yield_`/`continue_`/`run`): leave @unsafe, document as
   known quarantine.
3. [ ] `rcc_rpc.h` generated wire types: codegen rewrite to emit
   `rusty::Arc<Marshallable>` instead of `std::shared_ptr<Marshallable>`.
   Blocks Phase 3 item 2.

If Phase 4 lands cleanly → **75–80%**. If Marshal stays quarantined →
asymptote at ~70%.

## Per-iteration protocol (for the self-pacing loop)

Each loop iteration does ONE concrete unit of work:

1. **Read** this doc's "Progress log" section to find the next unchecked
   item across the current phase.
2. **Read** the target file(s) and design the smallest mechanical
   change (class-level annotation + per-method overrides for the few
   genuinely-unsafe operations).
3. **Apply** the changes via Edit tools.
4. **Verify**: run `borrow_check_rrr` and confirm it stays clean.
   Run `cmake --build build_clang21 --target rrr` to confirm compile.
5. **Commit** with a message that names the file/class and shows the
   safety LOC delta from `/tmp/safety_loc.py` if material.
6. **Update** this doc's Progress log section: tick the item, record
   the commit SHA and the new ratio.
7. **Decide** whether to continue or exit the loop:
   - Continue if the ratio is still rising and findings/build remain
     clean.
   - Exit if a change introduced findings the analyzer doesn't accept
     and there's no obvious fix (revert + flag in Progress log).
   - Exit if all current-phase items are checked off (move to next
     phase in next loop session).

The loop is allowed to:
- Edit source files in `src/rrr/` and `third-party/rusty-cpp/include/`.
- Bump the rusty-cpp submodule (if needed for library annotations).
- Commit to the `worktree-srpc` branch (NOT push to remote).
- Update this doc's Progress log.

The loop must NOT:
- Push to remote.
- Skip the borrow-check verification step.
- Mark a phase complete without all items checked off in the Progress log.

## Progress log

(Newest entries on top. Each entry: phase ID, item, commit SHA, delta.)

### Phase 0
- [x] Fix LOC script + relocate into repo as `scripts/rrr_safety_loc.py`.
  Restricts out-of-class method detection to brace depth==0 (file scope) and
  anchors the regex to type-prefix patterns (not control-flow keywords).
  Net effect: tightens classification rather than growing @safe — the
  earlier estimate of "+4-6pp" was wrong. Honest baseline after the fix:
  @safe 6.3%, @unsafe 9.9%, inner @unsafe-block 3.3%, unannotated 80.5%
  of in-fn LOC.

### Phase 1 — class-level @safe
- [x] Server (rpc/server.cpp) — class-level `// @safe` with method-level
  `// @unsafe` overrides preserved. Also fixed an LOC-script bug where
  multi-line `// @safe -` annotation comments containing `;` (e.g.
  "// overrides; ...") spuriously cleared pending → Future/Client class
  annotations from Tier 4 also weren't being credited. Commit 54a2d98a;
  borrow_check_rrr 45/45 clean; ratio 6.3% → 7.2% (after script fix).
- [x] Reactor (reactor/reactor.cpp) — class-level `// @safe` added.
  Commit af6db929; borrow_check_rrr 45/45 clean; ratio 7.2% → 7.4%.
  Most Reactor:: out-of-class methods already had explicit annotations,
  so the flip mainly credits inline class-body methods. Bigger reactor
  wins (PollThreadWorker* / Reactor::loop) live in Phases 2 + 3.
- [x] IdempotencyKeyGenerator + IdempotencyCache (rpc/idempotency.cpp)
  — class-level `// @safe` added to both classes. Commit 1f4d6b5a.
  Also fixed second LOC-script bug: `pending_for_class` leaked across
  function-body `{` consumption, falsely crediting some classes as
  `@safe` / `@unsafe` from a stale annotation many lines earlier.
  Honest baseline post-fix: 6.2% @safe / 9.5% @unsafe / 3.3% inner-block /
  81.0% unannotated. This iteration's class flip on idempotency.cpp
  doesn't move the ratio because every method already had explicit
  per-method annotations — only unannotated bodies in @safe classes
  gain from inheritance.
- [x] CompletionTracker (rpc/completion_tracker.cpp) — class-level
  `// @safe`. Commit ce802ff5; ratio 6.2% → 6.7% (+107 LOC).
- [x] CircuitBreaker (rpc/circuit_breaker.cpp) — class-level `// @safe`.
  Commit a12f0ee8; ratio 6.7% → 7.2% (+118 LOC).
- [x] HeartbeatManager (rpc/heartbeat.cpp) — class-level `// @safe`.
  Commit 8f4bf96d; ratio 7.2% → 7.6% (+88 LOC).
- [x] ConnectionStateMachine (rpc/connection_state.cpp) — class-level
  `// @safe`. Commit (pending); ratio 7.6% → 7.9% (+70 LOC).
- [ ] TcpListener subset (rpc/tcp_channel.cpp)
- [ ] LoadBalancer (rpc/load_balancer.cpp)
- [ ] RequestQueue class @safe completion (rpc/request_queue.cpp)

### Phase 1 — namespace-level @safe
- [ ] rpc/inmemory_channel.cpp
- [ ] rpc/frame_codec.cpp
- [ ] rpc/internal_protocol.cpp
- [ ] rpc/request_options.cpp
- [ ] rpc/connection_metrics.cpp
- [ ] rpc/callbacks.cpp
- [ ] rpc/errors.cpp
- [ ] rpc/utils.cpp
- [ ] rpc/pollable_proxy.cpp
- [ ] rpc/reconnect_policy.cpp
- [ ] misc/serializable_envelope.cpp
- [ ] misc/netinfo.cpp
- [ ] misc/stat.cpp
- [ ] misc/cpuinfo.cpp
- [ ] misc/rand.cpp
- [ ] misc/dball.cpp
- [ ] misc/alarm.cpp
- [ ] base/basetypes.cpp
- [ ] base/debugging.cpp
- [ ] base/strop.cpp
- [ ] base/callback_wrapper.cpp
- [ ] base/misc.cpp
- [ ] base/unittest.cpp
- [ ] reactor/epoll_wrapper.cc

### Phase 2 — easy raw-pointer refactors
- [ ] ChannelConnectionProxy / ChannelFactoryProxy → rusty::Box<Base>
- [ ] Reactor::PollThreadWorker* → rusty::Weak<PollThreadWorker>
- [ ] rusty::sys::* syscall wrappers
- [ ] ServiceProxy::__get_service__() → rusty::Arc<Service>

### Phase 3 — remaining unsafe paths
- [ ] alock.cpp WaitDieALock::ALock* → rusty::Weak<ALock>
- [ ] serializable.cpp std::shared_ptr<Marshallable> → rusty::Arc
- [ ] Reactor::loop tight @unsafe block scoping
- [ ] Pthread_* → rusty::sync::* wrappers

### Phase 4 — stretch
- [ ] Marshal byte ops decision (refactor / external annot / quarantine)
- [ ] Fiber context quarantine
- [ ] rcc_rpc.h codegen rewrite
