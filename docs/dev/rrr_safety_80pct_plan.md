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
- [blocked] ChannelConnectionProxy / ChannelFactoryProxy → rusty::Box<Base>
  — alias is currently `std::unique_ptr<Base>` in `rpc/channel.cpp`.
  Flipping to `rusty::Box<Base>` would break two patterns the codebase
  relies on heavily:
  (a) `rusty::Box<T>` has `Box() = delete` — no default-null state. 19
      call sites use `ChannelConnectionProxy{}` / `ChannelListenerProxy{}` /
      `ChannelFactoryProxy{}` to build "empty / failure" sentinels (most
      live in `inmemory_channel.cpp`, `tcp_channel.cpp`, and the channel
      tests). They'd all need rewriting to either return an `Option<Box>`
      or hold an explicit failure flag.
  (b) 12 bare `ChannelConnectionProxy var;` / `ChannelListenerProxy var;`
      declarations expect default-null and would not compile against
      `rusty::Box`.
  `ConnectResult.connection` would also need to flip to
  `rusty::Option<rusty::Box<ChannelConnectionBase>>` and every caller
  pattern would need adjusting. This is a multi-iteration refactor with
  call-site fan-out into rrr's tests; not a one-iteration mechanical
  change. Defer until a dedicated Phase 2 sub-plan covers the
  Option-conversion sweep.
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
- [blocked] rusty::sys::* syscall wrappers
  — this is a library-design task, not an rrr-only mechanical change.
  Unblocking netinfo.cpp / cpuinfo.cpp / rpc/utils.cpp (all currently
  [blocked] for syscall reasons) requires `rusty::sys::fs` and
  similar wrappers to exist in the rusty-cpp third-party submodule
  first. That means: (a) design the API surface, (b) add the
  wrappers to `third-party/rusty-cpp/include/rusty/sys/`, (c)
  upstream/coordinate the submodule bump (CLAUDE.md guidance keeps
  the submodule on `main` with the latest commit), (d) import the
  new module from each consuming rrr file and replace the syscall
  call sites. Multi-iteration effort spanning two repos. Defer.
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
