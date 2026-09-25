# Rust lane vs C++ lane: what I measured, and whether srpc fits

Two questions, answered on this host at `80433a59d`. The Mako repo was not
modified — `git diff HEAD` is empty and all work was in scratch.

---

# Part 1 — Performance

## What I ran

**The experiment is same-source, two-compilers.** Comparing our Rust against
upstream's C++ would confound a source difference with a compiler difference,
so both lanes are built from **our own `src/rrr/*.rs`**:

| | lane A | lane B |
|---|---|---|
| source | `src/rrr/base/basetypes.rs`, `src/rrr/rpc/frame_codec.rs` | the same two files |
| compiler | **rustc**, `opt-level=3 lto=true codegen-units=1` | transpiler → `rrr.basetypes.cppm` / `rrr.frame_codec.cppm` → **clang** `-O3 -march=native` |
| artifact | a `cargo` binary against the `rrr` rlib | a binary linked against the **production** `build/src/rrr/librrr.a` |

Lane B links the archive Mako actually ships, so it is the real generated-C++
runtime, not a re-creation.

**Harness** is a port of `stonysystems/srpc`'s `bench/src/main.rs`, line for
line, so our numbers and theirs are the same shape: 20,000,000 iterations × 4
runs, a warm-up pass of `iters/20`, report the **min** across runs and the
spread as the noise floor. Both lanes were run **ABBA** (A,B,A,B) so thermal
drift lands on both sides rather than on whichever went second, and each
figure below is the min over both passes.

**One correction made mid-run, worth recording.** My first version reported
`dump64` at a flat ~0.48 ns/op for every length class — identical across
classes, which is the "you are timing an empty loop" signature the upstream
harness warns about. Black-boxing only the *return value* let the optimiser
hoist the call. Both lanes were rebuilt with barriers on the **input**, the
**destination buffer** and the result (`std::hint::black_box` / `asm volatile
"" ::: "memory"`). The numbers below are from the corrected harness; the
first set was discarded.

## Results (min ns/op, lower is better)

```
operation                             rustc     clang      delta
  write_header                        2.229     4.053     -45.0%
  dump64  class 1 (1)                 2.519     4.389     -42.6%
  dump64  class 2 (64)                4.361     4.364      -0.1%
  dump64  class 3 (8192)              6.064     5.730      +5.8%
  dump64  class 4 (1048576)           7.425     6.399     +16.0%
  dump64  class 5 (134217728)         8.807     7.747     +13.7%
  dump64  class 6 (17179869184)      10.129     9.092     +11.4%
  dump64  class 7 (2199023255552)    11.468     9.424     +21.7%
  dump64  class 8 (281474976710656)   6.064     5.729      +5.8%
  dump64  class 9 (3.6e16)            6.202     5.705      +8.7%
  load64  class 1 (1)                 2.682     3.019     -11.2%
  load64  class 2 (64)                2.684     3.359     -20.1%
  load64  class 3 (8192)              3.033     3.692     -17.8%
  load64  class 4 (1048576)           3.688     4.360     -15.4%
  load64  class 5 (134217728)         4.029     5.040     -20.1%
  load64  class 6 (17179869184)       4.722     5.727     -17.5%
  load64  class 7 (2199023255552)     5.727     6.402     -10.5%
  load64  class 8 (281474976710656)   3.828     3.185     +20.2%
  load64  class 9 (3.6e16)            3.840     3.187     +20.5%
```

Negative = rustc faster.

## Reading

**Mixed, with no single winner, and every difference is small in absolute
terms** — the whole table lives between 2 and 12 nanoseconds.

- **rustc wins the decode path and the frame header.** `write_header` is 45%
  faster and `load64` is 10–20% faster across the seven common classes. The
  header write is the function the lost "+12% regression" claim in
  `docs/verification.md` was about; on this harness rustc is comfortably
  ahead, not behind.
- **clang wins the encode path**, by 6–22% on the larger `dump64` classes.
- The two crossover rows (`class 1` dump, `class 8/9` load) are where the
  branch structure differs most; nothing about them is mysterious, but they
  do mean "the Rust lane is uniformly faster/slower" is false.

**What this does NOT measure, and the limit matters more than the numbers.**
These are three leaf codecs. Nothing here exercises the reactor, the fiber
switch, a socket, an allocation, or an RPC. The honest statement is *"on the
hot leaf codecs, the two lanes are within tens of percent of each other in
both directions, on operations costing single-digit nanoseconds."* Whether
end-to-end RPC throughput matches is a different experiment, and the tool for
it already exists: `scripts/raft_paired_trial.{sh,py}` (ABBA paired trials,
exact sign test) — the same method that produced the p = 1.000 verdict for
the Raft cutover. It needs a driver that runs the same workload against both
lanes, which does not yet exist.

---

# Part 2 — Does `stonysystems/srpc` fit into this repo?

Three blockers, in descending order of difficulty. None is fatal; all are
concrete.

## Blocker 1 — WE HAVE A LIVE DATA-LOSS BUG THAT UPSTREAM ALREADY FIXED

**Corrected.** An earlier draft of this file said our encoder was fine ("643
probes, 0 mismatches") and framed this as two valid encodings disagreeing.
That was wrong, and the error was in my test: it decoded from the *same*
buffer it encoded into, instead of truncating to the length `dump64` reports.
The wire only carries the reported bytes. Testing it the way the wire does:

```
value                     n  marker   decoded from wire         verdict
 36028797018963967        8   0xfe     36028797018963712        DATA LOSS
  1125899906842795        8   0xfe      1125899906842624        DATA LOSS
   562949953421313        8   0xfe       562949953421312        DATA LOSS
  4503599627370751        8   0xfe       4503599627370496        DATA LOSS
   281474976710656        8   0xfe        281474976710656        ok (low byte 0)
  1800000000000000        8   0xfe       1800000000000000        ok (low byte 0)
```

`36028797018963967 -> 36028797018963712` is the exact number in upstream's
commit message. **Any `i64` in roughly [2^48, 2^55) with a non-zero low byte
is silently corrupted on the wire by `src/rrr` today.**

The mechanism, from `src/rrr/base/basetypes.rs`'s `dump64`: the `n >= 8` path
writes eight payload bytes at offsets 1..=8 MSB-first, then the `n == 8` arm
sets marker `0xFE` at offset 0 and **returns 8**. So the frame is offsets
0..=7 — it keeps the leading always-zero high byte and drops the trailing
significant low byte at offset 8. The `0xFE` rung's budget is marker + seven
payload bytes, which is correct for the band; the byte layout was copy-pasted
from the nine-byte `0xFF` arm with only the marker and the length changed.

Upstream traced this to the 2018 genesis C++ and fixed it in `e113960af`
(2026-09-05, ten days after our fork) by **retiring the rung** rather than
re-laying it out: `val_size` folds the band into the nine-byte `0xFF` form,
and `dump64`'s `n == 8` arm is deleted as dead. `load64` and `buf_size` are
untouched.

They chose that over giving `0xFE` a correct seven-byte layout because it is
smaller, lower risk, and **compatibility-safe in the direction that matters**:
a new sender's `0xFF` frame decodes on any old receiver (`0xFF` was always the
nine-byte marker), and historical `0xFE` data still reads exactly as before.

So this is not a blocker at all — it is a **reason to adopt**. Taking
upstream's change fixes a live defect in our tree.

**Our exposure is low, which is why it has survived.** Upstream's analysis
(`xid` never reaches the band; `server_instance_id` was deterministically
mangled so restart detection still worked; only user-declared `v64` fields
were ever exposed) applies to our identical code. Our two additional `v64`
sites are a payload size in `legacy_raft_log_payload.cc` and RPC `xid`s,
neither of which realistically reaches 2.8e14. Low exposure is not the same
as no exposure, and the fix is free.

## Blocker 2 — the transpiler pins have divergent lineage (medium)

```
ours      77c3ad5a  2026-08-23  "transpiler: support scalar value-init marker"
upstream  1689f438  2026-09-13  "Keep thread parking owners alive through glibc TL"
           ours ahead: 1     theirs ahead: 48     NOT a clean ancestor
```

Our pin sits on rusty-cpp's side branch `codex/raft-value-init`, **not on
`main`**; upstream's is on `main`. And our tree depends on what that one
commit adds: `src/deptran/raft/src/messages_hpp.rs` uses
`#[cfg_attr(any(), cpp_value_init)]` in eight places.

So adopting srpc/main requires rebasing that single commit (~540 lines, 6
files) onto `1689f438` or later and pinning the result. One commit, but it
gates everything, and it moves the transpiler under the Raft DSL and the
storage headers too.

**Documentation error found alongside it:** `CLAUDE.md` states the pin is
`fa7dd9d9…` and calls it "the tip of rusty-cpp's `main`". The actual gitlink
and `scripts/check_rrr_crate_mode.py` both say `77c3ad5a`, which is five days
*newer* than `fa7dd9d9` and is on a side branch, not `main`. CLAUDE.md is
wrong on both counts.

## Blocker 3 — we carry one local feature upstream lacks (easy)

This is the good news. Three-way measurement against the 2026-08-25 fork
point:

```
lines changed since the fork    by UPSTREAM: 8142
                                by US:        118
files both sides touched:       3 of 37
```

Our `src/rrr` is **98.6% staleness, 1.4% ours**. All 118 lines come from one
commit, `7f52613fe "raft: harden recovery, durability, and runtime liveness"`:

| file | lines | what |
|---|---|---|
| `rpc/server.rs` | +53 | the **admission-ready gate** (`set_admission_ready`, `ServerAdmissionReadyAtomic`, `SERVER_ERR_TRY_AGAIN`, `RpcServiceContext::new_with_admission`) |
| `reactor/reactor.rs` | 51 | reactor liveness hardening |
| `reactor/future.rs` | 12 | `is_set_.get()` → `is_ready()` |

The admission gate is load-bearing for Mako: `RaftWorker::SetupService` calls
`set_admission_ready(false)` before `start()` and `ShutDown` uses it to drain
in-flight requests. Upstream's `Server` has no such method — which is how I
found it, when my RPC test failed to compile against srpc/main.

So the reconciliation is **port one commit forward**, not a merge.

## Verdict

Adoption is viable and the merge cost is genuinely small — 118 lines, three
files, one commit. The ordering constraint is the transpiler rebase,
which has to land first.

Suggested sequence:

1. Rebase `77c3ad5a` (value-init marker) onto rusty-cpp `main`; pin it.
2. Port `7f52613fe`'s 118 lines onto srpc/main — the admission gate is the
   substantive part, and it is a feature upstream would likely want anyway.
3. Take srpc/main, **then** verify the C++ lane still builds inside Mako and
   the emitted ABI still satisfies `src/deptran` and `src/mako`. Nobody has
   run that combination; 139 commits of drift is a lot.
4. The SparseInt change is a fix, not a hazard: adopting it repairs a live
   data-loss defect, and upstream chose the option that keeps a new sender
   readable by an old receiver.

## Reproducing

```
scratch/lanebench/rust     cargo run --release      # lane A (rustc)
scratch/lanebench/cpp      ./lane_b                 # lane B (clang + librrr.a)
```
Lane B compiles with the module map lifted from any built rrr consumer
(`build/CMakeFiles/mako.dir/src/mako/lib/rrr_rpc_backend.cc.o.modmap`), plus
`-fmodule-file=std.compat=...`, and links `build/src/rrr/librrr.a` directly.
