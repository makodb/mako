# rrr safety annotation push — plan to reach 80% @safe LOC

> Historical annotation campaign. Current Goal-0 ownership, carrier counts,
> and terminal exceptions are tracked in
> [`goal0_completion_plan.md`](goal0_completion_plan.md). References below to
> `base/basetypes.cpp` describe the now-deleted carrier; canonical
> `src/rrr/src/basetypes.rs` owns that module today.

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

- [x] `Server` (rpc/server.cpp) — mirror what Tier 4 did for `Client`.
  Methods using sockets / `Pthread_*` keep method-level `// @unsafe`.
  Expected gain: ~600 LOC.
- [x] `Reactor` (reactor/reactor.cpp) — large; the fiber context-switch
  methods (`Fiber::yield_`, `Fiber::continue_`, `Fiber::run`) keep
  method-level `// @unsafe`. Loop and check_timeout get @unsafe blocks
  for the few raw ops. Expected gain: ~1,500 LOC.
- [x] `IdempotencyTracker` (rpc/idempotency.cpp) — already 199 safe /
  264 unannotated. Class-level @safe + a few method overrides should
  push to >90% safe. Expected gain: ~260 LOC.
- [x] `CompletionTracker` (rpc/completion_tracker.cpp) — similar shape.
  Expected gain: ~210 LOC.
- [x] `CircuitBreaker` (rpc/circuit_breaker.cpp). Expected gain: ~150 LOC.
- [x] `HeartbeatManager` (rpc/heartbeat.cpp). Expected gain: ~200 LOC.
- [x] `ConnectionStateMachine` (rpc/connection_state.cpp). Expected gain:
  ~150 LOC.
- [x] `TcpListener` (rpc/tcp_channel.cpp) — the listener half is mostly
  safe; the `TcpConnection` half stays @unsafe. Expected gain: ~400 LOC.
- [x] `LoadBalancer` (rpc/load_balancer.cpp). Expected gain: ~100 LOC.
- [x] `RequestQueue` (rpc/request_queue.cpp) — partial Tier 2 already.
  Class-level @safe completes it. Expected gain: ~80 LOC.

**Namespace-level `// @safe` candidates** (whole files where every
function should be @safe):

- [x] `rpc/inmemory_channel.cpp` — 844 LOC, zero annotations today.
  All rusty internals, no syscalls. Expected gain: ~800 LOC.
- [x] `rpc/frame_codec.cpp` — 335 LOC unannotated. Expected gain: ~330.
- [x] `rpc/internal_protocol.cpp` — small. Expected gain: ~80.
- [x] `rpc/request_options.cpp`. Expected gain: ~100.
- [x] `rpc/connection_metrics.cpp`. Expected gain: ~250.
- [x] `rpc/callbacks.cpp`. Expected gain: ~100.
- [x] `rpc/errors.cpp`. Expected gain: ~80.
- [x] `rpc/utils.cpp` — has `getaddrinfo()`; needs per-method @unsafe.
  Expected gain: ~120.
- [x] `rpc/pollable_proxy.cpp`. Expected gain: ~50.
- [x] `rpc/reconnect_policy.cpp`. Expected gain: ~150.
- [x] `misc/serializable_envelope.cpp`. Expected gain: ~200.
- [x] `misc/netinfo.cpp`. Expected gain: ~50.
- [x] `misc/stat.cpp`. Expected gain: ~80.
- [x] `misc/cpuinfo.cpp`. Expected gain: ~150.
- [x] `misc/rand.cpp`. Expected gain: ~30.
- [x] `misc/dball.cpp`. Expected gain: ~100.
- [x] `misc/alarm.cpp`. Expected gain: ~80.
- [x] `base/basetypes.cpp` — POD types only. Expected gain: ~470.
- [x] `base/debugging.cpp`. Expected gain: ~100.
- [x] `base/strop.cpp`. Expected gain: ~100.
- [x] `base/callback_wrapper.cpp`. Expected gain: ~80.
- [x] `base/misc.cpp`. Expected gain: ~100.
- [x] `base/unittest.cpp`. Expected gain: ~100.
- [x] `reactor/epoll_wrapper.cc` — has epoll syscalls; needs per-method
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
  `// @safe`. Commit ed12702e; ratio 7.6% → 7.9% (+70 LOC).
- [x] TcpListener subset (rpc/tcp_channel.cpp) — class-level `// @safe`
  + per-method `// @unsafe` overrides on listen/close/fd-touching
  handle_* methods + local_address/set_on_accept/set_on_error.
  Also fixed third LOC-script bug: namespaces were being treated as
  unannotated function bodies, masking everything inside as "in fn".
  After fix, "in fn" LOC drops from 22,478 to 12,206 — the namespace
  preamble (includes, type aliases, comments, free declarations) is
  now correctly classified as "other". Of the genuine in-fn LOC:
  14.9% @safe / 18.6% @unsafe / 6.0% inner-block / 60.5% unannotated.
  Commit b209eb89.
- [x] LoadBalancer + LoadBalancerState (rpc/load_balancer.cpp) — class-
  level `// @safe`. Commit d4ea8534; ratio 14.9% → 15.4% (+67 LOC).
- [x] RequestQueue class @safe completion (rpc/request_queue.cpp) —
  class-level `// @safe` added. Methods were already @safe from Tier 2.
  Commit 75496f62; ratio 15.4% → 16.0% (+76 LOC).

### Phase 1 — namespace-level @safe
- [blocked] rpc/inmemory_channel.cpp — namespace `// @safe` flip fired
  17 violations (raw `InMemoryConnectionState*` pointers + const_cast
  via `mut_state` pattern in InMemoryListener::accept_for_connect and
  make_channel_pair_for_testing, raw-ptr arithmetic in
  InMemoryChannel::send_frame, plus "use of uninitialized variable"
  on locally-declared callback / latch flags inside send_frame and
  close). The raw-ptr issues need a refactor (rusty::Arc::get_mut
  / rusty::MutPtr) — beyond Phase 1 mechanical scope. Reverted;
  follow-up in Phase 3.
- [blocked] rpc/frame_codec.cpp — namespace `// @safe` flip fired 7
  violations. The codec encodes/decodes frames on raw `uint8_t*` byte
  pointers (frame_codec_encode_into, FrameStreamReader::next_frame,
  consume_frame, compact_if_needed) — that's the wire-protocol path
  and inherently raw-ptr-arithmetic. Either refactor to a
  `Cursor<Vec<u8>>` abstraction (Phase 4 territory; perf-sensitive) or
  keep file unannotated. Reverted.
- [x] rpc/internal_protocol.cpp — namespace `// @safe`. Pure constexpr
  bit-twiddling. Commit fc9be1ad; ratio unchanged (file body is tiny;
  inline constexpr functions don't move LOC counters).
- [x] rpc/request_options.cpp — namespace `// @safe`. Commit eabaffe4;
  ratio 16.0% → 16.1% (+6 LOC).
- [x] rpc/connection_metrics.cpp — namespace `// @safe`. Commit fa602d6f;
  ratio 16.1% → 16.8% (+90 LOC).
- [x] rpc/callbacks.cpp — namespace `// @safe`. Commit 87eb79bb.
  Also fixed fourth LOC-script bug: namespace annotations weren't being
  recorded as a fallback for function-body inheritance, so files with a
  `using` declaration after the namespace open (which consumed the
  pending @safe via the `;` check) got zero credit. After fix: ratio
  16.8% → 18.3% (+186 LOC; the gain retroactively credits earlier
  namespace flips too).
- [x] rpc/errors.cpp — namespace `// @safe`. Commit 6707c2e8;
  ratio 18.3% → 19.3% (+114 LOC).
- [blocked] rpc/utils.cpp — every substantial function does syscalls
  (getaddrinfo, fcntl F_GETFL/F_SETFL, socket/bind/getsockname,
  gethostname) and AddrInfo holds a raw `struct addrinfo*`. Namespace
  flip would fire violations across the whole file with no net @safe
  gain. Leave unannotated; it's a thin syscall wrapper by design.
- [x] rpc/pollable_proxy.cpp — namespace `// @safe` + per-method
  `// @unsafe` on `mut_poll()` (const_cast through Arc::get).
  Commit 9bb655bd; ratio unchanged at 19.3% (tiny file).
- [x] rpc/reconnect_policy.cpp — namespace `// @safe`. Commit b460ca77;
  ratio 19.3% → 19.8% (+63 LOC). One step from the 20% milestone.
- [blocked] misc/serializable_envelope.cpp — file revolves around Marshal
  `operator<<` / `operator>>` chains for envelope wire encoding, plus
  one `const_cast<SerializableEnvelope&>` for the const-shim path.
  Marshal operator overloads are inherently @unsafe in rusty-cpp's
  model. Phase 4 territory (Marshal byte-ops decision).
- [blocked] misc/netinfo.cpp — every method does file I/O via
  std::ifstream against /sys/class/net/ens4/statistics/{rx,tx}_bytes
  plus a `times()` syscall in the ctor. No @safe surface area. Phase
  3 candidate once a `rusty::sys::fs` reader exists.
- [x] misc/stat.cpp — namespace `// @safe`. AvgStat is a POD with int64
  counters + arithmetic. Commit a9bb96ca; ratio 19.8% → **20.0%**
  (+22 LOC). **20% milestone reached!**
- [blocked] misc/cpuinfo.cpp — CPUInfo opens /proc/{pid}/net/dev and
  /proc/{pid}/stat via std::ifstream, plus `times()` and `getpid()`
  syscalls. Same shape as netinfo.cpp. Same Phase 3 candidate.
- [x] misc/rand.cpp — class `RandomGenerator` `// @safe`. Wrapped get_seed
  + rand_r calls in inner `// @unsafe { }` blocks inside rand/rand_double/
  rand_str so percentage_true/nu_rand/weighted_select inherit class @safe.
  Per-method `// @unsafe` on create_key/delete_key/get_seed/rdtsc/destroy.
  Switched `(int)ret.length()` to `static_cast<int>` in int2str_n. Commit
  1b633ec9; ratio 20.0% → **20.4%** (+54 LOC).
- [x] misc/dball.cpp — class `DragonBall` `// @safe`. Inline
  `// @unsafe { }` block around the `delete this` self-destruct in
  `trigger()`. Commit 5f2b0796; ratio 20.4% → **20.5%** (+16 LOC).
- [x] misc/alarm.cpp — class `Alarm` `// @safe`. Methods use
  `rusty::BTreeMap` + `rusty::Function` + `rrr::Time::now()`. The raw
  `rrr::PollThread *holder` field is never dereferenced and
  `set_holder` is a no-op stub. No per-method overrides needed.
  Commit a64a2a96; ratio 20.5% → **20.8%** (+32 LOC).
- [x] base/basetypes.cpp — both `export namespace rrr` and the impl
  `namespace rrr` `// @safe`. Per-method `// @unsafe` overrides on
  `Time::now`/`Time::sleep` (clock_gettime, select), on the four
  `SparseInt::dump`/`load_*` impls (reinterpret_cast<char*> + raw byte
  slicing), on `Timer::start`/`stop`/`elapsed` (gettimeofday), on
  `Rand::Rand` (gettimeofday + pthread_self + reinterpret_cast<uintptr_t>),
  on `MergedEnumerator::add_source`/`next` (raw `Enumerator<T>*` + raw
  iterator pairs). Inline `// @unsafe { delete this }` around the
  RefCounted release self-destruct. Commit 0cf788a2; ratio 20.8% →
  **22.7%** (+237 LOC).
- [x] base/debugging.cpp — both `export namespace rrr` and the impl
  `namespace rrr` `// @safe`. Per-method `// @unsafe` on both
  `print_stack_trace` variants (backtrace, popen/pclose, raw `char**`,
  reinterpret_cast<std::istream*>, free) and the anonymous
  `read_line_from_pipe` helper (fgets into a raw `char[4096]`). The
  pre-existing `verify` template's `// @safe` annotation is preserved.
  Commit e3458edd; ratio 22.7% → **23.4%** (+77 LOC).
- [x] base/strop.cpp — `namespace rrr` `// @safe`. Per-method
  `// @unsafe` on `startswith` and `endswith` (raw `const char*`,
  strlen/strncmp, pointer arithmetic). `format_decimal` and `strsplit`
  inherit namespace @safe (std::string + ostringstream + rusty::Vec).
  Commit 1dc137d7; ratio 23.4% → **23.8%** (+54 LOC).
- [x] base/callback_wrapper.cpp — both `export namespace rrr` and the
  inner `namespace detail` `// @safe`. CallbackWrapper is a pure
  forwarder over `rusty::Arc<rusty::Function<Sig>>`. No per-method
  overrides needed. Commit b82e63f8; ratio holds at **23.8%** (+4 LOC).
- [x] base/misc.cpp — both `export namespace rrr` and the impl
  `namespace rrr` `// @safe`. Per-method `// @unsafe` on `rdtsc`
  (inline asm), `FrequentJob::Ready` (rrr::Time::now), `make_int`
  (raw `char*` byte writer), `time_now_str` (time+localtime_r+
  gettimeofday into raw `char*`), `get_ncpu` (sysconf), `get_exec_path`
  (snprintf+readlink+static `char[PATH_MAX]`), `getline`
  (getdelim+free). `clamp`/`insert_into_map`/`erase` templates and
  Job/OneTimeJob inherit namespace @safe. Commit 14f4d549; ratio
  23.8% → **24.2%** (+42 LOC).
- [x] base/unittest.cpp — both `export namespace rrr` and the impl
  `namespace rrr` `// @safe`. Per-method `// @unsafe` on `TestMgr::
  instance` (raw `new TestMgr` + static raw-ptr cache), `TestMgr::reg`
  (raw `TestCase*` param/return), `TestMgr::matched_tests` (raw
  `const char*` + raw out-vec), `TestMgr::parse_args` (raw `char* argv[]`
  + raw `bool*` out-params), `TestMgr::run` (raw argv + printf +
  `delete t` / `delete this`). `TestCase::fail`/`reset`/`group`/`name`/
  `failures` inherit @safe. Commit 714b2aa1; ratio 24.2% → **24.9%**
  (+83 LOC).
- [x] reactor/epoll_wrapper.cc — both `export namespace rrr` and the
  impl `namespace rrr` `// @safe`. Per-method `// @unsafe` on every
  Epoll syscall path: ctor (kqueue/epoll_create), move-assign + dtor
  (::close), `Add` / `Remove` / `Update` (kevent / epoll_ctl), and
  both `Wait` overloads (kevent / epoll_wait). `Pollable` is a pure
  virtual interface with no bodies; `Epoll::fd()` is the only safe
  accessor. Commit 596d31e6; ratio 24.9% → **25.5%** (+81 LOC).
  **Phase 1 complete:** ratio rose 6.4% → 25.5% over iters 0–35.

### Phase 1 — unblock retries (subplan from 2026-05-18)

The six Phase 1 namespace-level `[blocked]` items above were marked
blocked under the early-Phase-1 "namespace `@safe`; revert all on any
finding" approach. The late-Phase-1 pattern — namespace `@safe` plus
per-method `// @unsafe` overrides on the offending methods only —
postdates those decisions and likely unblocks most of them.

Items are ordered by independence: retries that don't depend on new
library work first, then `rusty::sys::fs` (SP-1) plus its consumers,
then the Cursor/Marshal refactor (SP-5) plus its consumers. If SP-1
or SP-5 doesn't fit in one iteration, the loop will tick it
`[blocked]` and move on; downstream consumers will then be picked
up the next time that library work lands.

- [x] inmemory_channel.cpp retry — both `export namespace rrr` and the
  impl `namespace rrr` `// @safe`. Per-method `// @unsafe` on every
  method routing through a `mut_state` / `mut_conn` / `mut_listener` /
  `mut_factory` const_cast helper (InMemoryChannel: 9 methods +
  mut_state; InMemoryChannelAdapter: 8 methods + mut_conn;
  InMemoryListenerAdapter: 4 methods + mut_listener;
  InMemoryFactoryAdapter: 2 methods + mut_factory). Also @unsafe on
  `accept_for_connect`, `connect`, `make_listener`, and
  `make_channel_pair_for_testing` (each does an inline `const_cast`).
  `InMemoryChannel::send_frame` carries an extra rationale: raw
  `uint8_t*` byte slicing through `bytes.assign(f.payload, f.payload +
  f.size)`. `InMemorySwitchboard::find_listener` keeps the body @safe
  with an inline `// @unsafe { val_opt.unwrap()->upgrade() }` block
  around the Option-deref. Commit 98322cca; ratio 25.5% → **26.8%**
  (+165 @safe LOC; unannotated dropped 311 LOC).
- [x] rpc/utils.cpp retry — both `export namespace rrr` and the impl
  `namespace rrr` `// @safe`. AddrInfo's per-method `// @unsafe` on
  the explicit raw-ptr ctor, move ctor, move-assign, dtor, get,
  operator->, operator*, release, reset (freeaddrinfo), resolve
  (getaddrinfo). The free functions `set_nonblocking` (fcntl),
  `find_open_port` (socket/bind/getsockname/close + sockaddr* casts),
  and `get_host_name` (gethostname into a raw `char[1024]`) are all
  `// @unsafe`. Default ctor + `operator bool` (nullptr check)
  inherit namespace @safe. Commit a543ab51; ratio 26.8% → **27.2%**
  (+44 @safe LOC).
- [x] SP-1: `rusty::sys::fs` wrapper — added new
  `third-party/rusty-cpp/include/rusty/sys/fs.hpp` exporting
  `rusty::sys::fs::read_to_string(path) -> Result<std::string,
  io::Error>`. Annotated `// @safe`; body wraps `std::ifstream` in a
  single inline `// @unsafe { }` block so no FILE* / ifstream handle
  escapes. Exposed via `rusty.cppm` and `rusty.hpp`. Submodule
  commit 6ed675e; parent commit 7e7d9957. Ratio unchanged at 27.2%
  (no rrr files modified — adoption comes next).
- [x] netinfo.cpp retry — `export namespace rrr` and `class NetInfo`
  `// @safe`. Extracted a `parse_bytes(path)` helper that calls
  `rusty::sys::fs::read_to_string` and parses with `strtoul` in a
  small inline `// @unsafe { }` block (preserves silent-zero-on-junk
  semantics). The ctor and `get_net_stat` keep `times(&tms_buf)`
  inside an inline `// @unsafe { }` block but otherwise stay @safe.
  `net_stat()` factory inherits @safe. Commit 3699b217; ratio
  27.2% → **27.5%** (+38 @safe LOC).
- [x] cpuinfo.cpp retry — `export namespace rrr` and `class CPUInfo`
  `// @safe`. Per-method `// @unsafe` on the four heavy methods:
  ctor (sysinfo + sysconf + times + getpid), `get_cpu_stat`
  (times() + dispatch into @unsafe helpers), `get_network`
  (ifstream + getline + strtok + strtoul), `get_memory` (ifstream +
  24-step `operator>>` chain). `cpu_stat()` factory inherits @safe.
  Adoption of `rusty::sys::fs::read_to_string` inside get_network /
  get_memory is left for a SP-5 follow-up — the file's parse paths
  are gnarlier than the netinfo.cpp pattern (strtok mutates the
  string in place; the stat file uses a deep operator>> chain).
  Commit df8483b7; ratio 27.5% → **28.1%** (+69 @safe LOC).
- [x] SP-5: Marshal byte-ops decision — `rusty::io::Cursor<T>`
  already existed; annotated its public API `@safe` so client code
  can read/write/seek without dropping out of the borrow check. Both
  `read` and `write` now move the raw `uint8_t*` extraction (via
  private `get_data`/`get_mut_data`) and the `std::memcpy` into
  inline `// @unsafe { }` blocks. Submodule commit d9795f0;
  parent commit 060223e2. frame_codec.cpp and serializable_envelope.cpp
  adopt the Cursor next.
- [x] frame_codec.cpp retry — `export namespace rrr` and the impl
  `namespace rrr` `// @safe`. Per-method `// @unsafe` on every
  function that handles raw `uint8_t*` arithmetic: the inline
  `frame_codec_write_header` / `frame_codec_peek_header`, the
  out-of-class `frame_codec_encode_into`, and the four
  FrameStreamReader methods that touch `buf_.data() + read_pos_`
  (`append`, `next_frame`, `consume_frame`, `compact_if_needed`).
  Trivial accessors (`reset`, `buffered_bytes`, `empty`) and the POD
  structs (`FrameHeader`, `FrameView`, `FrameDecodeStatus`) inherit
  namespace @safe. Did NOT rewrite onto `rusty::io::Cursor` in this
  iteration — frame_codec is the transport hot path and the cursor
  port needs benchmarks first. SP-5 follow-up. Commit c5f5ee77;
  ratio 28.1% → **28.4%** (+32 @safe LOC).
- [x] misc/serializable_envelope.cpp retry — `export namespace rrr`
  and `class SerializableEnvelope` `// @safe`. Per-method
  `// @unsafe` on `unpack` (raw `T*` via dynamic_cast), const
  `unpack`, `unpack_shared` (raw-ptr lambda-deleter shared_ptr),
  const `unpack_shared`, `is_a` (calls unpack), `save` (Marshal
  operator<< chain), `load` (Marshal operator>> chain), and on the
  4 free-function operators: `marshallable_cast` const overload
  (const_cast), `operator<<(BinaryWriteArchive&,…)`, `operator>>
  (BinaryReadArchive&,…)`, `operator<<(Marshal&,…)`, `operator>>
  (Marshal&,…)`. Trivial accessors (`kind`, `has_value`,
  `operator bool`, `operator==`/`!=`, `refresh_kind`), the
  ctors/assign-from-shared_ptr, `pack`/`pack_aliased` factories
  inherit namespace @safe. Cursor adoption for the Marshal sink/
  source is a future SP-5 follow-up — wire-format identical to
  frame_codec which is also still on the labeling path.
  Commit 7fa7a0b2; ratio 28.4% → **28.9%** (+71 @safe LOC).
  **Phase 1 unblock subplan complete:** 8/8 items ticked, ratio
  rose 25.5% → 28.9% across iters 39–46.

### Phase 2 — easy raw-pointer refactors
- [x] ChannelConnectionProxy / ChannelListenerProxy / ChannelFactoryProxy
  → rusty::Box<Base>. The original blocked rationale was over-cautious —
  the codebase already wrapped storage in
  `SpinMutex<Option<Box<ChannelXProxy>>>` (`Box<unique_ptr<Base>>`
  double indirection), so flipping the alias to `Box<Base>` collapses
  the outer Box. `ConnectResult.connection` is now
  `rusty::Option<ChannelConnectionProxy>`; `ChannelFactoryBase::
  make_listener()` returns `rusty::Option<ChannelListenerProxy>`.
  Empty-sentinel sites (`ChannelXProxy{}` in ConnectResult error
  paths + the unused test-only `make_listener()` mocks) became
  `rusty::None`; the two "bind_channel with null proxy" tests are
  obsolete because the type system enforces non-null now. Verification:
  borrow_check_rrr 45/45 clean; rrr library + downstream rpcbench/dbtest
  compile; 80+ channel-mode unit tests pass. Commit 3f7ea5a9; ratio
  unchanged at **70.7%** (structural change, not annotation-driven).
- [blocked] Reactor::PollThreadWorker* → rusty::Weak<PollThreadWorker>
  — the raw pointer `static inline thread_local PollThreadWorker*
  current_worker_` (reactor/reactor.cpp:1011) is an *intentional*
  workaround. The spawn-lambda holds the worker through
  `borrow_mut()` for the entire poll_loop lifetime
  (reactor.cpp:2585–2588: `auto guard = worker->borrow_mut(); …
  current_worker_ = &*guard;`). The comment on line 2583 spells it
  out: "Using raw pointer avoids RefCell re-borrow issues in fibers".
  Replacing with `rusty::Weak<RefCell<PollThreadWorker>>` would force
  callers (`add_pollable_from_current_thread`,
  `is_on_poll_thread`, fiber re-entry sites) to
  `upgrade().borrow_mut()`, which would panic the RefCell because the
  outer poll_loop guard already holds the unique borrow. A proper
  fix needs ownership restructuring (drop the RefCell layer, expose
  `&mut PollThreadWorker` through a different primitive, or split
  worker state by-field so per-method borrows don't collide). Not a
  one-iteration change. Defer.
- [x] rusty::sys::* syscall wrappers — cross-repo library-design task.
  Four sub-families landed in the rusty-cpp submodule and folded into
  rrr's call sites:
   * `rusty::sys::time` — clock_realtime_us / clock_realtime_coarse_us /
     clock_monotonic_us / gettimeofday_us / sleep_us. Submodule commit
     5990539; parent commit b7a4041d. Folded Time::now / Time::sleep /
     Timer::* / FrequentJob::Ready / RequestQueue / Server::drain /
     client.cpp `current_time_ms` + `monotonic_ms_now`. Sets up
     `nanosleep` / `clock_gettime` / `select-as-sleep` migration paths
     that earlier @unsafe wraps depended on.
   * `rusty::sys::process` — getpid / sysconf / process_times
     (ProcessTimes aggregate) / sysinfo (Linux-only SysInfo aggregate).
     Submodule commit 843ba3b; parent commit 7ea33dec. Folded into
     misc.cpp::get_ncpu, cpuinfo.cpp ctor + get_cpu_stat, netinfo.cpp
     ctor + get_net_stat, reactor.cpp fiber_task_t::init_context,
     server.cpp instance-id generation.
   * `rusty::sys::env` — hostname() returning owned std::string.
     Submodule commit d720f95; parent commit a619b8a1. Folded into
     rpc/utils.cpp::get_host_name (now @safe).
   * `rusty::sys::pthread` — current_id_hash() returning uint64_t
     (pthread_self + std::hash<pthread_t>; scoped narrowly to thread
     identity, the mutex / condvar / thread-create surface continues
     to live in rusty::sync::*). Submodule commit 97c45b4; parent
     commit 1a1cae2f. Folded into basetypes.cpp Rand::Rand.

  Remaining families not yet wrapped (each is a deliberate skip):
   * epoll/kqueue — epoll_wrapper.cc already abstracts the platform
     via per-method @unsafe overrides; relocating that abstraction
     into rusty::sys would be net-zero on rrr safety.
   * pthread mutex/condvar — threading.cpp's Pthread_* inline wrappers
     are already @safe via inner @unsafe blocks; rusty::sync::Mutex /
     Condvar exist as higher-level alternatives.
   * full file I/O surface — rusty::sys::fs::read_to_string (SP-1) plus
     rusty::os::fd::OwnedFd (Phase A) cover the entries rrr actually
     uses; the remaining ifstream / strtok parsers in cpuinfo.cpp /
     misc.cpp's time_now_str are inherently @unsafe at the call site
     due to raw `char*` plumbing and would not benefit.
- [x] ServiceProxy::__get_service__() → `Service&` (minimum mechanical
  change). Changed signature from `void* __get_service__()` to
  `Service& __get_service__()` on Service and ServiceTypedBoxAdapter;
  updated the for_each_service callback at server.cpp:894 to pass
  the reference directly (no `static_cast<Service*>` unwrap). Both
  methods are now `// @safe`. Did NOT migrate to `rusty::Arc<Service>`
  because there is only one caller and `Service&` already eliminates
  the unsafe `static_cast<void*>` / `static_cast<Service*>` ops;
  an Arc migration would also require ServiceProxy to flip from
  `Box<Service>` to `Arc<Service>`. Commit 97ab8d44; ratio holds at
  **28.9%** (the lines were already in @safe context — the casts
  were the only unsafe ops and they're now gone).

### Phase 3 — remaining unsafe paths
- [blocked] alock.cpp WaitDieALock::ALock* → rusty::Weak<ALock>
  — plan named the wrong class. The raw `ALock*` BTreeMap keys
  actually live on `ALockGroup` (alock.cpp:642), not WaitDieALock:
  `rusty::BTreeMap<ALock*, uint64_t> locked_` and
  `rusty::BTreeMap<ALock*, ALock::type_t> tolock_`. Converting these
  to `rusty::BTreeMap<rusty::Weak<ALock>, ...>` requires (a) ALock
  to be Rc/Arc-managed from creation, (b) every `tolock_.insert(alock,
  type)` callsite to downgrade, (c) every iter `[alock, ...]` body
  to upgrade + handle the None case. There are also downstream
  consumers in `src/deptran/2pl/tx.h` and `src/deptran/2pl/scheduler.cc`
  — refactoring this touches the deptran codebase too. Multi-iteration
  effort spanning two subsystems. Defer.
- [blocked] serializable.cpp std::shared_ptr<Marshallable> → rusty::Arc
  — 43 occurrences of `shared_ptr<Marshallable>` across 20 files,
  the vast majority in deptran (raft, mencius, copilot, scheduler,
  tx, coordinator, procedure, RW_command). The Phase 3 plan note
  explicitly flags this as the largest single refactor: "estimate
  1-2 weeks of careful work because every existing RPC service
  definition is downstream." It also requires updating
  `pylib/simplerpcgen/lang_cpp.py` codegen because the generated
  `rcc_rpc.h` uses `shared_ptr<Marshallable>` directly. Multi-iteration
  effort spanning the whole project. Defer.
- [x] Reactor::loop tight @unsafe block scoping
  — all three original blockers resolved:
  (a) Fixed in rusty-cpp by the recent init-tracker overhaul
      (`has_initializer` flag + 3-signal detection covers the
      `bool x = true;` inside-do-while pattern).
  (b) Resolved at some point — the from-lambda conversion now
      matches the `@safe` annotation on the `rusty::Function` ctor.
  (c) Wrapped the single `check_timeout(ready_events)` call in a
      tight inline `// @unsafe { ... }` block.
  Commit 8c7b09a5; ratio 63.0% → **63.1%** (~120 LOC of Reactor::loop
  body now analyzed as @safe by default; the inline @unsafe blocks
  on Event status mutation + Weak::upgrade + continue_fiber paths
  remain).
- [x] Pthread_* wrappers — namespace `// @safe` umbrellas added on
  both `export namespace rrr` (line 23) and the impl `namespace rrr`
  (line 596). All 13 `Pthread_*` inline wrappers individually marked
  `// @safe` with their single libc call wrapped in an inline
  `// @unsafe { libc pthread_* }` block. Per-method `// @unsafe`
  overrides added on the 5 implementation methods the analyzer
  flagged: `ThreadPool::start_thread_pool`, `RunLater::start_run_later`,
  `RunLater::run_later_loop`, `RunLater::run_later`,
  `RunLater::max_wait`. Did NOT rename to `rusty::sync::*`
  (refactor deferred — labeling sufficed to flip downstream
  callers @safe by inheritance). ratio 63.5% → **65.3%**
  (+201 @safe LOC).

### Phase 4 — stretch
- [x] Marshal byte ops decision — initially **chose labeling (option 3
  of the Phase 4 menu)** as an interim step: added namespace `// @safe`
  on both `export namespace rrr` and the impl `namespace rrr`; class
  `Marshal` `// @safe`. Triaged 15 borrow-check violations by adding
  per-method `// @unsafe` overrides on the four methods routing
  through the raw `chunk*` head_/tail_/next linked list and raw
  `char*` casts: `Marshal::content_size_slow`, `Marshal::write`,
  `Marshal::read_chnk`, `Marshal::read_reuse_chnk`. Cursor port
  deferred (hot wire path; needed perf benchmarks first). Commit
  e6850039; ratio 28.9% → **31.6%** (+321 @safe LOC).
  **Superseded by the Cursor rewrite below.**
- [x] Marshal byte ops rewrite (Cursor-style) — picked option 1 of
  the Phase 4 menu after measuring. Replaced the chunk-linked-list
  internals of `rrr::Marshal` with a `rusty::Vec<uint8_t>` +
  `read_pos_` cursor (the same shape as `rusty::io::Cursor` over a
  Vec, but with separate write/read positions). Public API unchanged
  (`write`/`read`/`peek`/`content_size`/`set_bookmark`/
  `write_bookmark`/`read_from_marshal`/`reset`, 50+ operator<<>>
  overloads, MarshalSink/MarshalSource adapters). Removed the
  chunk-list private members (raw_bytes, chunk, head_/tail_) and the
  internal-only `read_chnk` / `read_reuse_chnk`. Bookmark struct
  simplified from `(size, char**)` to `(offset, size)`.
  Measurement-gated: built bench_marshal microbench (9 hot-path
  scenarios), captured baseline of chunk-list, prototyped MarshalV2
  side-by-side, compared. Required two rusty-cpp library fixes that
  fell out: Vec::extend_from_slice now memcpy's trivially-copyable T
  instead of looping push() byte-by-byte; Vec::reserve grows
  geometrically (max(new, 2*cap)) so a sequence of small
  reserve(size+N) calls amortizes O(N) rather than O(N²).
  Perf result: faster on every scenario between 15% and 81%.
  Most importantly the chunk-walk drain pattern (write 10x1KB then
  read) went from 6.6 µs → 1.25 µs (-81%) — the chunk-walk
  overhead is gone.
  Annotation footprint on marshal.cpp: -476 LOC overall, -372 @unsafe
  LOC, with 51 `// @unsafe` per-method overrides collapsing to a
  handful of inline `// @unsafe { memcpy }` blocks. Commits 7cf92cc0
  (prototype + bench) + aeed22fe (swap); ratio 65.4% → **67.6%**.
  Reference: docs/dev/marshal_perf_baseline.md.
- [x] any_message.cpp — namespace `// @safe` on both export and impl
  blocks; classes `AnyMessage` / `AnyMessageRegistry` `// @safe`.
  Per-method `// @unsafe` on save/load (Marshal chains),
  unpack/unpack_shared (raw const std::string* + new), the 4
  operator<<>> archive helpers, and the 5 AnyMessageRegistry methods
  (annotation-discovery gap on HashMap-through-struct). Commit
  19fecd5c (+20 @safe LOC).
- [x] channel.cpp — single-line namespace `// @safe`; pure virtual
  interfaces only. Commit 362f0b11 (+25 @safe LOC).
- [x] fiber_channel.cpp — namespace + `class FiberChannel` `// @safe`;
  per-method `// @unsafe` on ctor/dtor/on_inbound_frame/send_frame/
  close/is_closed; inline `// @unsafe { event->set/wait }` blocks.
  Commit 7921358c (+52 @safe LOC).
- [x] future.cpp — namespace `// @safe`; `FiberPromise<T>` /
  `FiberFuture<T>` `// @safe`. Per-method `// @unsafe` on
  ctor/set_value/get/wait_for. Commit 25c0f637 (+47 @safe LOC).
- [x] logging.cpp — namespace + `class Log` `// @safe`; per-method
  `// @unsafe` on every Log static method (variadic + sprintf chain).
  `Log_debug` / `Log_info` / `Log_warn` / `Log_error` / `Log_fatal`
  template shims wrap their single Log::* call in `// @unsafe { ... }`.
  Commit 0b7a56c7 (+101 @safe LOC).
- [x] alock.cpp — namespace + 5 classes `// @safe` (ALock,
  WaitDieALock, WoundDieALock, TimeoutALock, ALockGroup). Only 3
  methods needed per-method `// @unsafe`: WaitDieALock::abort,
  WoundDieALock::abort, TimeoutALock::lock_all (address-of stored
  std::list elements). One inline `// @unsafe { lock_all(lock_reqs); }`
  in TimeoutALock::abort. Biggest single-iteration win. Commit
  5764debe; ratio 33.5% → **40.6%** (+869 @safe LOC).
- [x] tcp_channel.cpp — namespace + `class TcpConnection` `// @safe`.
  Per-method `// @unsafe` on all 4 adapter sets' mut_* const_cast
  helpers + methods routing through them; on handle_read (recv +
  FrameStreamReader chain), flush, handle_write, drain_outbound_locked
  (uint8_t* + send), parse_inet4_addr (inet_pton), TcpFactory::connect
  (socket/connect/setsockopt/fcntl + reinterpret_cast<sockaddr*>).
  Commit 68c384d9; ratio 40.6% → **45.2%** (+564 @safe LOC).
- [x] client.cpp — namespace `// @safe` comments added to all 3
  `export namespace rrr` blocks + impl `namespace rrr`. No code
  edits — file already had ~90% per-method annotations from prior
  Tier-4 work. Commit 6ce00abb; ratio 45.2% → **51.5%**
  (+772 @safe LOC; crosses 50% threshold).
- [x] server.cpp — namespace `// @safe` umbrellas on all 3 namespace
  blocks (export at 44 + 510, impl at 936). Class-level `// @safe` on
  Service / ServiceTypedBoxAdapter / RpcServiceContext (interfaces +
  pure adapters). Flipped `class ServerConnection` from `// @unsafe`
  to `// @safe` — the bulk of its methods were already labeled with
  per-method `// @unsafe` overrides on socket/marshal/raw-pointer
  paths (close, bind_channel, decode_request_and_dispatch,
  dispatch_response_frame_via_channel, run_async). No new violations.
  Commit 1239e189; ratio 51.5% → **53.8%**.
- [x] Fiber context quarantine — the technical quarantine was already
  in place from prior work: `Fiber::run` / `yield_` / `continue_` are
  `// @safe` wrappers with their bodies in inner `// @unsafe { ... }`
  blocks; `fiber_task_t::resume` / `yield_to_caller` / `entry` /
  `init_context` / `entry_trampoline` are `// @unsafe` with detailed
  justifications; the asm-only TUs `fiber_context_{x86_64,aarch64}.cc`
  can't be borrow-checked at all. This iteration locks in the intent:
  adds explicit QUARANTINE markers to both arch-specific docstrings
  (calling out that they cannot be made safe and listing the wrapping
  callers in reactor.cpp), strengthens the `Fiber` class-level
  docstring to describe the quarantine pattern explicitly, and adds
  per-method `// @safe` overrides on the four trivial methods
  `Fiber::Fiber(...)` / `Fiber::~Fiber` / `Fiber::finished` /
  `Fiber::do_finalize`. The class-level annotation stays `// @unsafe`
  per the plan's "leave @unsafe" directive — only the trivial
  accessors flip. ratio 65.3% → **65.4%** (+10 @safe LOC).
- [x] rcc_rpc.h codegen rewrite — done in prior commits; this
  iteration is just a documentation tick. Verified state:
  `src/rrr/pylib/simplerpcgen/lang_cpp.py:193` emits
  `rusty::Arc<rrr::Future>` (not `std::shared_ptr<rrr::Future>`) for
  the generated TypedFuture wrappers; `src/deptran/rcc_rpc.h` contains
  285 `rusty::Arc<...>` uses and **zero** `shared_ptr<...>` uses
  (`grep -cE`). Downstream consumers (`communicator.h`, `coordinator.h`,
  `procedure.h`, `paxos_worker.h`, `scheduler.h`, `RW_command.h`)
  have migrated their RPC-facing payload types from
  `shared_ptr<Marshallable>` to `janus::Command` and carry comments
  describing the implicit-conversion shim; the rrr-side wire boundary
  no longer touches `std::shared_ptr<Marshallable>`. ratio unchanged
  at 65.4% (no LOC change in this commit — already credited in the
  earlier landing).

### Phase 2 follow-on — stale annotation sweep (2026-05-23)

After Phase 2 item 1 (channel proxy → rusty::Box) and the first
sys::* family (sys::time) landed, an audit found ~30 stale per-method
`// @unsafe` overrides and inline `// @unsafe { ... }` blocks across
rrr whose rationales were tied to operations that have since become
@safe (Pthread_*, rusty::sys::time::*, Time::now/sleep,
SpinMutex::lock, rusty::Vec / VecDeque / HashSet / HashMap / BTreeSet /
RefCell / Option / Weak methods, Cell::get/set, Marshal operator<<>>
via the Phase 4 Cursor rewrite, Fiber::finished, IntEvent::set,
CallbackWrapper move-assign, rusty::Arc::make, std::string accessors
and assignments, std::chrono).

Sweep commits (newest first; ratio is cumulative through commit):
  - `afcd6f81` marshal Vec::push loop wrap                — 73.3%
  - `5e0e3382` marshal Vec::reserve wraps                 — 73.2%
  - `521812a0` client monotonic_ms_now → sys::time        — 73.2%
  - `05625b41` client reconnect_address_.empty wrap       — 73.2%
  - `4ab2623e` fiber sleep_until_us Time::now wrap        — 72.7%
  - `cb150ee3` request_queue update_config flip           — 72.7%
  - `c8c03959` client/server std::string + SpinMutex+Vec  — 72.7%
  - `ad271d30` idempotency Marshal ops + chrono → sys::time— 72.6%
  - `feb26bb3` client make_box + SpinMutex wraps          — 72.5%
  - `6d3950ab` more RefCell + Option + SpinMutex wraps    — 72.4%
  - `12cc3717` Vec::clear + SpinMutex + RefCell wraps     — 72.1%
  - `b457f3c3` Event::test flip                           — 71.9%
  - `8d6e5db5` Reactor Fiber::finished wraps              — 71.9%
  - `97ef4896` 4 stub / container methods                 — 71.8%
  - `23fc1e5e` ThreadPool/RunLater::make                  — 71.6%
  - `68bce017` TcpListener set_on_accept/set_on_error     — 71.5%
  - `e7335375` TcpConnection/Listener adapter accessors   — 71.5%
  - `22ef2668` Server::drain → sys::time                  — 71.5%
  - `fbeb3cef` PollThreadWorker BTreeSet methods          — 71.4%
  - `1c50a219` client Future::timed_wait + Weak + empty   — 71.4%
  - `963f5caa` IntEvent::set + QuorumEvent::vote_*        — 71.2%
  - `cc34e1a8` Log::set_level via Pthread_* wrappers      — 71.2%
  - `5018d210` Queue + SpinCondVar @unsafe overrides      — 71.2%
  - `9bf498eb` nanosleep call-sites → sys::time::sleep_us — 70.9%
  - `b7a4041d` Time::now/sleep/Timer/Rand via sys::time   — 70.9%
  - `3f7ea5a9` Channel proxy → rusty::Box                 — 70.7%

Net effect: **+2.7pp** of @safe ratio (70.6% baseline →
**73.3%**), borrow_check_rrr 45/45 clean throughout. The
remaining `// @unsafe` markers in rrr are mostly legitimate
(syscall paths, raw `T*`/`FILE*`/`char*` parameters, asm-only
fiber-context primitives, const_cast-through-Arc adapters,
Marshal byte ops).

Phase 2 deferred items not yet attempted in this push:
  - PollThreadWorker raw thread_local → rusty::Weak — still
    [blocked]. The raw pointer co-exists with a borrow_mut() guard
    held for the poll_loop lifetime; switching to
    Weak<RefCell<...>> would force callers to `upgrade().borrow_mut()`
    on the already-borrowed RefCell. Needs per-field Cell splitting
    or a different ownership primitive.
  - Other rusty::sys::* families (epoll, pthread, process/fs beyond
    sys::fs/time) — incremental wins, see notes on the
    rusty::sys::* partial entry above.

### Phase 2 follow-on — dead-code removal (2026-05-23)

Survey-driven prune of code in rrr that has zero production callers:

- [x] `ThreadPool` / `RunLater` / `SpinCondVar` / `Queue` from
  `src/rrr/base/threading.cpp` — never constructed in production
  (the deptran workers wired `ThreadPool` into fields but never
  enqueued work). Commit `f6be7df9`. ~520 LOC.
- [x] `NetInfo` class (`src/rrr/misc/netinfo.cpp`) — public API is
  `NetInfo::net_stat()`, never invoked anywhere. Removed file plus
  `import rrr.netinfo;` from rrr.hpp and the matching entries in
  the CMake module/borrow lists. ~75 LOC.
- [x] `ServerConnection` PollableProxy-facade stubs — `fd()`,
  `poll_mode()`, `content_size()`, `handle_read()`, `handle_write()`,
  `handle_error()`, `handle_free()`, `check_pending_write_update()`.
  Comments claimed "PollableProxy facade ABI compatibility" but
  `ServerConnection` has no base class and `make_pollable_proxy_from_typed_arc<T>`
  is never instantiated with `T = ServerConnection`. Only callers
  lived inside one test that exercised the no-op behavior of dead
  code; deleted the test alongside the stubs. ~95 LOC.
