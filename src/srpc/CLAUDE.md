# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## The one thing to understand first

SRPC is a **C++23 named-module RPC library whose every module provider is generated, not hand-written.**
All 37 production modules are canonical **Rust** files living at their historical C++ paths
(`base/`, `misc/`, `reactor/`, `rpc/`), and the pinned `rusty-cpp` transpiler generates
`srpc.<name>.cppm` from them. Two consumers read the *exact same bytes*:

- **rustc/Cargo** — via `src/lib.rs`, a *generated* crate index of
  `#[path = "../rpc/frame_codec.rs"] pub mod frame_codec;` declarations. `src/` holds nothing else.
- **rusty-cpp** — one whole-crate invocation (`--crate Cargo.toml --verus-exec --crate-graph`) emits all
  37 `srpc.*.cppm` providers *and* the Lion dependency providers below.

**The executor, I/O reactor and timers are Lion's, not SRPC's.** [Lion](https://github.com/stonysystems/lion)
is a Tokio-shaped async runtime whose executor and reactor are verified in Verus. It enters as pinned,
*unmodified* crates from the `third-party/lion` gitlink: `srpc` depends on `lion-reactor` and
`lion-executor` as path dependencies with `default-features = false` (so no mio, socket2 or tokio), and
they bring `lion-slab`, `lion-timer-wheel`, the `*-spec` crates and Verus's erased `vstd` from Verus git
`db81a74`. Both lanes read Lion's bytes unchanged. rustc compiles them through the `verus!` macro, whose
erasure drops specs and proofs; `--verus-exec` runs that same erasure (Verus's own `EraseAll` pass, out of
process in rusty-cpp's `rusty-cpp-verus-erase` helper) and transpiles what is left. The five generated Lion
modules (`lion_executor_spec`, `lion_slab`, `lion_timer_wheel`, `lion_reactor`, `lion_executor`, each in its
own `namespace`) compile in SRPC's file set and land in `libsrpc.a` as a **separately inventoried provider
class**: they are not canonical modules, and none of the 37-module tables, totals or the seven-place edit
apply to them. SRPC's claim about Lion is identity with Lion@pin, with Lion's own CI as the proof of record.
Never write that SRPC is "verified live": the theorem covers spawned stackless tasks under stated
assumptions, not fibers, events, the transport, the `block_on` root, or tasks that are aborted, panic or
are dropped while waiting on I/O.

Ownership is split. Verified Lion owns the executor, the reactor and the timer wheel. Canonical Rust owns
everything SRPC-specific: stackful fibers, the event family, the `PollThread` driver, the epoll OS backend
Lion runs on (`SrpcEpollBackend` in `reactor/epoll_wrapper.rs`), the TCP transport, serialization and
reliability logic. `build.rs` and CMake compile the same nine C sources plus the selected architecture's
fiber assembly from `scripts/native-kernel-sources.txt`. Those sources provide individual OS operations,
platform layouts, entropy and clock reads, and context switching. Compatibility headers import generated
modules, while `misc/serializable_support.hpp` supplies bounded C++ trait forwarding. Do not put SRPC
policy in these adapters or patch generated C++ (SRPC's or Lion's) to bypass lowering. Fix canonical Rust
or general compiler support; a Lion fix lands in `stonysystems/lion`, is re-proved there, and arrives by
pin bump. [docs/dev/lion-runtime-plan.md](docs/dev/lion-runtime-plan.md) records the migration and its
decisions (D1–D5); [the runtime notes](docs/canonical-rust-runtime.md) cover the older API migrations.

The dual-compile gate recompiles generated providers, runs its C++ importer against fresh objects and
against the production archive, and compares measured ABI and import inventories, for SRPC's providers
and, separately, for Lion's. Current expectations live in `scripts/check_srpc_crate_mode.py`; do not copy
symbol totals into documentation. The separate `srpc_runtime_parity` test compares actual Rust and
generated-C++ runtime transcripts. Both checks are needed: two C++ executions alone cannot validate Cargo
behavior.

Consequence that governs almost every edit: **a change to a `.rs` file is simultaneously a Rust change
and a C++ ABI change.** A green `cargo test` does not mean the C++ still builds or keeps its ABI. Lion's
sources are transpiler inputs too, so the same holds for a Lion pin bump.

## Commands

**Before you commit.** There is no CI — no `.github/`, nothing runs on push. This sequence *is* the
safety net, and the `Verified:` paragraph the commit convention demands is copied out of its output:

```sh
RUSTFLAGS=-Dwarnings cargo test --locked --workspace --all-targets  # -> passed/failed counts
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release             # -> configure exit code
cmake --build build --parallel 4                                    # -> build exit code (ALL pulls in both gates)
ctest --test-dir build -L srpc --output-on-failure                  # inspect the registered suites too
```

Initialize `third-party/rusty-cpp`, `third-party/lion` and `third-party/googletest` before building. The
Rust lane needs `third-party/lion` as well: Cargo resolves Lion as path dependencies, and nothing builds
without it. The gitlinks and the hard checks in `scripts/extract_srpc_rust.py`,
`scripts/check_srpc_crate_mode.py` and `scripts/check_rust_independence.py` define the current pins; a
copied revision in prose is not authoritative:

```sh
git submodule update --init --recursive
```

**Cold machine.** Several steps run Cargo `--offline` (`check_rust_independence.py`, the
`rust_source_audit.py` scanner build, `run_rpc_echo_bench.sh`), and Lion's `vstd` and the verus-erase
helper's `verus_syn`/`verus_prettyplease` come from Verus's *git* repository, not crates.io. Warm
`~/.cargo` once with network access: `cargo fetch --locked` in the SRPC root and in `third-party/rusty-cpp`
(plus the `scripts/rust_source_audit/` registry crates, below).

**Rust lane.** Cargo also needs a C compiler and archiver for the shared native kernels:

```sh
cargo test --locked --workspace --all-targets
cargo test --locked --workspace --doc
RUSTFLAGS=-Dwarnings cargo test --locked --workspace --all-targets   # what the gate actually runs
cargo clippy --locked --workspace --all-targets -- -D warnings       # a lint here breaks the C++ build

cargo test --test frame_codec_rust                                   # one test file (stem of tests/<stem>.rs)
cargo test --test stat_rust -- --exact some_test_fn_name             # one test function
```

**C++ lane** (needs Clang ≥ 22 with libc++, CMake ≥ 3.30, Ninja, Cargo, Python 3, plus `rg` (ripgrep) and a
populated local Cargo registry — see the two source-gate prerequisites under *Individual gates*):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4 --target srpc_goal0_dual_compile
ctest --test-dir build -L srpc --output-on-failure
```

Budget for it: a cold C++ lane is minutes, not seconds — CMake builds the pinned transpiler and, in a
separate cargo invocation, its `rusty-cpp-verus-erase` helper from source, builds vendored googletest, and
compiles every module BMI (SRPC's and Lion's) under `-march=native`; the battery suites are `RUN_SERIAL`
with `TIMEOUT 600` because they drive real Lion runtimes, sockets and fibers. Don't start one to answer a
Rust-only question; always budget for one before committing a canonical `.rs` change.

```sh
ctest --test-dir build -L runtime_battery --output-on-failure   # configured runtime and parity tests
ctest --test-dir build -R '^test_fiber$' --output-on-failure    # one suite (name = CMake TARGET name)
./build/test_fiber --gtest_filter='FiberTest.SleepUsZero'       # one gtest case
```

Some runtime targets are plain programs, including `test_reactor_minimal` and `test_runtime_parity`.
`--gtest_filter` applies only to gtest targets. Inspect `ctest --test-dir build -N -L srpc` and the explicit
CMake test lists rather than relying on a historical suite count.

**Benchmarks.** `rpcbench` is `EXCLUDE_FROM_ALL` — a benchmark is not a correctness gate, and it is not
registered with ctest (throughput is not pass/fail, and it binds a real TCP port). Build and run it
explicitly; the harness starts a fresh server per trial and prints one `avg_qps` line each:

```sh
cmake --build build --parallel 4 --target rpcbench
scripts/run_rpcbench.sh build/rpcbench before-my-change   # 4 modes x 3 trials
```

Modes are `fast|fiber|defer|async|fast_vec` and exercise different dispatch paths (inline / stackful
fiber / deferred reply / stackless task / vector payload), so a change can move one and not the others.
`fast_vec` needs `-v` and is omitted by default as a different workload. Read the *spread* across trials,
not the best number: on a shared machine an effect smaller than the trial-to-trial range is not an effect.
Override with `RPCBENCH_N` (seconds), `RPCBENCH_B` (packet bytes), `RPCBENCH_TRIALS`, `RPCBENCH_MODES`.

The **Rust echo benchmark** needs no C++ lane, so it is the quick way to see a driver or transport change.
`bench/src/bin/rpc_echo.rs` sends one fast-handler RPC over loopback TCP between two `PollThread`s through
the public `Server`/`Client` API and prints one `RPC_ECHO` line per run: throughput with
`RPC_ECHO_WINDOW` (default 64) requests in flight, CPU per request, and one-in-flight p50/p90/p99. It does not
replace rpcbench's dispatch-mode matrix:

```sh
scripts/run_rpc_echo_bench.sh                               # current tree, RPC_ECHO_TRIALS runs (default 5)
scripts/run_rpc_echo_bench.sh --compare <refA> <refB> [...] # alternating, same sitting, spread per build
```

Compare mode builds each ref from `git archive` plus the Lion gitlink that ref records, with today's
`bench/` copied in, and alternates the builds; `RPC_ECHO_SECONDS` and `RPC_ECHO_LATENCY_N` set the phases.

`bench/` is also the *nanosecond* benchmark, and it answers a different question: timing of the hot leaf
codecs (`frame_codec_write_header`, `sparseint_dump64`/`load64` per length class). rpcbench cannot see
effects at that scale — a sub-ns leaf change is ~0.05% of a request, far under its trial spread — so
neither substitutes for the other. Like `verify/`, `bench/` is **workspace-excluded**, so
`cargo test --workspace --all-targets` never compiles it and it adds nothing to the source gate:

```sh
scripts/run_microbench.sh                          # current tree
scripts/run_microbench.sh --compare <refA> <refB>  # A/B, alternating, same sitting
```

The compare mode is the one that answers questions: like `run_rpc_echo_bench.sh`, it builds each ref from
`git archive` plus the Lion gitlink that ref records, so pre-Lion and Lion-era refs both build. It puts
*today's* `bench/` in place of the ref's own, checking the copy before it builds, so the harness is
held constant; then it interleaves the runs. Absolute ns/op is machine- and thermal-dependent; only the
back-to-back delta means anything. A cautionary tale lives in `docs/verification.md`: a "+12% regression"
sat in that file for a while on the strength of an uncommitted harness, and vanished the moment a
committed one re-took it.

**Individual gates.** The source gate checks the test inventory, canonical inventory, compiler contracts,
native kernel and ABI-binding ownership, canonical Rust bodies, the Cargo dependency allowlist, negative
controls, Rust tests, and clippy. The extraction check needs the built transpiler
(`cmake --build build --target build_rusty_cpp_transpiler`) and runs the Verus version-coupling check
below. `test_goal0_contracts.py`'s transpiler class needs the verus-erase helper as well
(`--target build_rusty_cpp_verus_erase`) and *skips* without either; a skip is not a pass. Build the helper
on its own: built in one cargo invocation with the transpiler, it turns proc-macro2's `span-locations` on
for the transpiler too. The DSL check needs no transpiler — it still accepts a transpiler path for CMake
compatibility, but never runs it.

```sh
python3 scripts/check_test_inventory.py
python3 scripts/tests/test_test_inventory.py
python3 scripts/tests/test_goal0_standalone.py
python3 scripts/tests/test_goal0_contracts.py
python3 scripts/rust_source_audit.py
python3 scripts/check_rust_independence.py
python3 scripts/check_native_kernels.py
python3 scripts/check_native_abi_bindings.py
python3 scripts/tests/test_rust_source_audit.py
python3 scripts/tests/test_native_kernels.py
python3 scripts/tests/test_native_abi_bindings.py
python3 scripts/tests/test_rust_independence.py
python3 scripts/tests/test_runtime_parity.py
python3 scripts/extract_srpc_rust.py --check
bash scripts/srpc_dsl_check.sh
```

Two host prerequisites are easy to miss because nothing in the tree vendors them, and both fail the *source
gate*, not just the standalone script:

- `scripts/srpc_dsl_check.sh` shells out to `rg` (ripgrep) under `set -euo pipefail`. Without `rg` on
  `PATH` the command substitution exits 127, the script reports `canonical source scan failed` and
  propagates that status — it fails closed rather than reporting zero carriers.
- `scripts/rust_source_audit.py` builds its `syn` AST scanner with
  `cargo build --quiet --locked --offline --manifest-path scripts/rust_source_audit/Cargo.toml` and
  `check=True`. `--offline` means the crates in `scripts/rust_source_audit/Cargo.lock` (`syn`, `quote`,
  `proc-macro2`, `serde_json` and their transitive deps) must already be in the local Cargo registry; on a
  cold machine, warm it once with network access before running the gate offline.
  `check_rust_independence.py` has the same offline requirement for the Verus git checkout (see *Cold
  machine*).

The canonical Rust AST audit rejects missing implementations and pins reviewed constant functions in
`scripts/canonical-constant-functions.json`. It scans private and nested production bodies too.
Native source/header changes require review against `scripts/native-kernels.json` and
`scripts/native-abi-bindings.json`. These inventories have no automatic approval command. Check test
output for skips: missing compiler dependencies can skip contract tests and cannot establish acceptance.

`scripts/check_rust_independence.py` is an exact **allowlist**, not a ban. It requires `[dependencies]` to
be exactly `lion-reactor` and `lion-executor`, each `{ path = "third-party/lion/<crate>", default-features =
false }`, with no build or target dependencies, no `[patch]`/`[replace]`, and no workspace member besides
`srpc` (`[workspace] exclude` keeps Lion's crates from becoming members). In the resolved graph, everything
linked into `srpc` must be one of the eight `LION_CRATES` at its directory under the gitlink, built with no
features, or `vstd`/`verus_builtin` at exactly `VERUS_SOURCE`. The only proc-macros may be Verus's two, and
their host-only closure may come only from crates.io and Verus's parser crates. mio, flume, tokio, socket2,
futures-task and pin-project-lite must appear nowhere, including `cargo tree -e normal`, and the submodule
must sit at the gitlink commit with no local change in the compiled crates. It then copies the Cargo
inputs, canonical Rust, tests, the C/assembly kernel and the Lion crates' tracked files into a fresh tree,
and runs Rust tests and doctests there offline, with `-Dwarnings`, `CXX=/bin/false` and no C++ runtime or
compiler on its tool path.

**Verus version coupling.** The Rust lane erases Lion's `verus!` blocks with the `verus_builtin_macros`
that `Cargo.lock` resolves; the C++ lane uses the copy rusty-cpp vendors. `verify_verus_erasure_coupling`
(`extract_srpc_rust.py`, run by the extraction check and again by `check_srpc_crate_mode.py`) requires the
transpiler's `--verus-build-info` to name exactly the commit every Verus git package in `Cargo.lock`
resolves to, and the same `verus_builtin_macros` version. A Lion bump that moves Verus fails here until
rusty-cpp re-vendors the pass.

**Lion provider inventory** (the `DEPENDENCY_*` tables in `check_srpc_crate_mode.py`). The providers must
equal the transpiler's `crate-graph.json` exactly: order, the ghost-only `lion-framework-spec`, and the
unused `lion-utility-spec` and `lion-reactor-spec`. CMake's `SRPC_LION_PROVIDERS` must match in order
(`test_goal0_contracts.py`). Each provider needs zero hand slots, exact private imports, no `srpc` import,
and exact strong symbols in both its fresh object and `libsrpc.a`. Only `srpc.epoll_wrapper`
(→ `lion_reactor`), `srpc.reactor` and `srpc.tcp_channel` (→ both) may `export import` a Lion module. The
importer imports and uses `lion_reactor` and `lion_executor`, and builds a Lion runtime over
`SrpcEpollBackend`. This inventory changes only with a Lion or transpiler bump.

**Verus** (separate lanes, not wired into CMake or ctest):

```sh
VERUS_HOME=/path/to/verus-dist scripts/verify_srpc.sh   # SRPC's own specs (verify/)
VERUS_PATH=/path/to/verus-dist scripts/verify_lion.sh   # Lion's ci.sh on the gitlink commit
```

`verify_lion.sh` re-runs Lion's own CI on the commit the gitlink records (extracted with `git archive`,
so a dirty submodule cannot change what is verified). It needs exactly Verus `0.2025.11.15.db81a74` and
the Rust `1.91.0` toolchain through rustup; SRPC's own toolchain is unaffected. `VERUS_HOME` is accepted
too. `VERIFY_LION_DIR` (default `${XDG_CACHE_HOME:-~/.cache}/srpc/verify-lion`) must lie outside the
checkout, or Cargo takes SRPC's root as the Lion crates' workspace. `ci.sh` also verifies `lion-utility`
and `lion-liveness`, which pull mio and tokio, so it needs network or a warm registry. A cold run takes
about nine minutes. It is optional and non-gating, like `verify_srpc.sh`.

**Sanitizers** are a whole-configuration switch, so use a separate build dir:
`cmake -S . -B build-asan -G Ninja -DSRPC_SANITIZER=address` (`none|address|thread|undefined`).

There are two gate targets, both in `ALL`: `srpc_goal0_source_gate` (source side — inventory and DSL
checks, extraction and Verus coupling, ownership audits, the dependency allowlist, Python negative
controls, `cargo test`, `cargo clippy -D warnings`) and `srpc_goal0_dual_compile` (archive side — the
`nm`/ABI oracle in `check_srpc_crate_mode.py`, for SRPC's and Lion's providers). The `srpc` library target
depends on the source gate, so *any* C++ build runs the whole Rust suite first, and a new clippy warning
breaks the C++ build. A green source gate says nothing about ABI.

## Invariants that will bite you

**`#[cfg_attr(any(), …)]` is the emitter's directive language, and rustc never sees it.** `any()` is
always false, so these 58 attributes are invisible to `cargo build`, `cargo test` and clippy while being
the only way to state a C++ contract Rust cannot: `cpp_inherit` (19), `cpp_native_type` (9),
`cpp_namespace(::janus)` (8 — the Quorum surface, which must live in *global* `::janus`;
`srpc::janus::QuorumEvent` mangles differently and is not a substitute), `cpp_noexcept` (4),
`cpp_no_auto_traits` (3), `cpp_abi` (3), `cpp_trait_member_dispatch` (3), `cpp_declaration` (2),
`cpp_default_argument` (2), `cpp_marker_trait` (2), `cpp_no_fieldwise_ctor` (2), `cpp_abi_alias` (1).
`thread_local` is retired (0): per-thread state is real `thread_local!`, which the transpiler lowers to
`inline thread_local rusty::LocalKey<T>`. Three further `cfg_attr(any(), …)` spellings live inside `//`
comments (`base/misc.rs`, `reactor/reactor.rs` twice) and are not attributes — do not count them.
Deleting or mistyping one is silent in the Rust lane and changes the emitted module. The mirror form
`#[cfg_attr(not(any()), derive(...))]` (19 sites) is the opposite — derives rustc *does* apply but the
emitter must not see, so plain `#[derive(...)]` is not the same edit and emits C++ operators that were
deliberately withheld. (`IdempotencyKey`'s hand-written `impl PartialEq` is the only source of the
`operator==` symbol the ABI table pins, precisely because its derive is hidden behind `not(any())`.)

**`#[allow(clippy::…)]` in canonical sources are measured emitter pins, not style waivers.**
`rpc/client.rs` opens with a block recording what each family costs: taking `ptr_arg` retypes
`clientpool_select`, taking `derivable_impls` deletes `FutureAttr::default_()`, and four families whose
pins are inert under today's clippy once guarded the `DisconnectBehavior_QUEUE()` rename and a
method-signature change. Of the 22
`explicit_auto_deref` allows across the canonical dirs (19 item-scoped, plus file-scope ones in
`reactor/future.rs`, `rpc/fiber_channel.rs` and `rpc/tcp_channel.rs`), 16 are in `rpc/client.rs` alone; the
per-site comments there record which ones change emitted C++ and how. They were measured against rusty-cpp
`3e1d9505`, so re-measure after a transpiler bump before trusting one. That family's suggestions are
`MachineApplicable`, so `clippy --fix` applies them without ever seeing the consequence.
**Never run `clippy --fix` over `base/ misc/ rpc/ reactor/`.** (The module-level `#![allow(static_mut_refs)]`
pin in `reactor/reactor.rs` is retired: the statics it covered migrated to `thread_local!`.)

**`src/lib.rs` is generated — never hand-edit it.** It carries a sha256 of `rust-modules.toml` in its
header, so touching the manifest without `extract_srpc_rust.py --write` fails the gate. Never add any
other file, or any symlink, under `src/`: the census in `extract_srpc_rust.py` rejects symlinks and
orphan `.rs` files, and `test_goal0_standalone.py` asserts `src/` contains exactly `lib.rs`.

**Adding a canonical module is a seven-place edit**, and missing one is a hard error:
1. a `[[module]]` row in `rust-modules.toml` — **append at the end**. The list is historical *promotion*
   order, not a dependency order (`srpc.utils` precedes `srpc.logging`, which it imports); the gate
   topologically re-sorts at build time. What *is* enforced is that CMake's inventories match this file's
   order element-for-element, so an alphabetical insertion fails;
2. `python3 scripts/extract_srpc_rust.py --write` to regenerate `src/lib.rs`;
3. `SRPC_GOAL0_CANONICAL_MODULES`, `set(SRPC_GOAL0_SOURCE_<name> …)` and `SRPC_GOAL0_RETIRED_CARRIER_SRC`
   in `CMakeLists.txt`;
4. the hard-coded provider total `37` in `CMakeLists.txt` (`math(EXPR _SRPC_EXPECTED_INLINE_COUNT "37 - …")`
   and the two `EQUAL 37` checks) — otherwise configure aborts with
   *"Goal-0 production must contain all 37 retained module providers"*;
5. the hard-coded `37` in `scripts/tests/test_goal0_contracts.py` (`ExtractionContractTests`) — this one
   fires in the transpiler-free standalone lane, so it is the first failure you will hit;
6. the ratchet tables in `scripts/check_srpc_crate_mode.py` (`ABI_SPECS`, `EXPECTED_IMPORTS`,
   `EXPECTED_GENERATED_MODULE_SHA256`, `IMPORTER_USE_MARKERS`) **and** the ~3,700-line C++ importer
   program embedded as a Python string in that same file (`importer_source()`) — `require_importer_coverage`
   demands each module be imported there exactly once *and* actually used;
7. rows in `module-preambles.toml` / `cpp-module-index.toml` / `rust-type-map.toml` if the module needs
   C++ includes, foreign symbols, or exact legacy type spellings.

A module that imports Lion directly also changes `EXPECTED_DEPENDENCY_REEXPORTS`, since the transpiler
re-exports every Lion module a child names.

**Ordinary edits trip ABI checks too.** The current `ABI_SPECS`, provider totals, platform ownership,
and ordered `EXPECTED_IMPORTS` live in `scripts/check_srpc_crate_mode.py`. An intended public change
requires fresh generated objects and measured symbol/layout evidence before changing those expectations.
Record why the interface changed. Do not read a new count from a success message that merely echoes an
expected constant.

New standard containers or canonical module dependencies can change the generated import list. Direct
`crate::reactor` types and calls must retain their canonical dependency, using the supported
`use crate::reactor as _;` anchor where needed. Rust aliases must not be redirected to omitted facade
implementations to make an import error disappear. The generated crate must report zero hand-attention
slots; `TODO`/`UNSUPPORTED`/`skipped` generated output is rejected, in Lion's providers as in SRPC's.
Generated byte digests are advisory, unlike ABI and ownership checks.

**Canonical `.rs` files are byte-policed:** UTF-8, LF only (CRLF is rejected, not normalized), a trailing
newline, no NUL. They may only live under `base/`, `misc/`, `rpc/`, `reactor/`, and the basename must equal
the module name.

**`-march=native` is an ABI requirement, not an optimization.** Clang refuses to load a BMI whose
target-feature set differs from the importer's, so removing it produces ~133 bogus errors. Build trees are
therefore not portable across CPUs.

**`-w` hides warnings in generated code.** `srpc` compiles every provider, SRPC's and Lion's, with
`SRPC_CXXFLAGS`, which begins with `-w`, and no SRPC gate promotes a warning to an error. That is how G3
got through. The emitter lowered a `LocalKey::with` closure in Lion's `Reactor::enter` to a
`-> decltype(auto)` lambda that returned a dangling reference, and every `AsyncFd` registration failed at
run time. Clang's `-Wreturn-stack-address` flagged it, but `-w` silenced the warning. rusty-cpp's own
parity-test and crate-graph compiles now pass `-Werror=return-stack-address`; SRPC's do not. When a
generated module misbehaves at run time, recompile it without `-w` before suspecting the canonical Rust.

**Verus specs in canonical modules use `#[cfg(verus)]`, never `verus_only`.** The pinned transpiler
special-cases that exact ident (it is always false there, like Lion's `verus_keep_ghost` cfgs); renaming
it breaks the whole-crate transpile. `verify/.cargo/config.toml` forces `--cfg verus` locally because
`cargo verus` itself only sets `verus_only`. Lion uses the other style: its executable code sits inside
`verus! { }`, which `--verus-exec` erases with Verus's own pass. That pass fails closed on any other Verus
macro (`proof!`, `verus_impl!`, a `verus!` in statement position or one produced by `macro_rules!`) and on a
`verus_keep_ghost` cfg in a position it does not evaluate. Canonical SRPC modules contain no `verus!` block
and keep the attribute style. Moving them to `verus!`, which would lift the no-`proof!` rule, is an untaken
follow-on in the plan, not current practice.

**A module-scope `const` IS ABI surface.** P1815 attaches it to the module, and it lands in the object
as a strong `R` symbol regardless of use — the `SERVER_ERR_*` block, `kAsyncSlotCount`,
`kTcpWriteThroughIdleUs` and the sink capacity seeds are all pinned rows. So adding one is an ordinary
ratchet edit, not a trick to dodge: an `ABI_SPECS` row, the `EXPECTED_TOTAL_PROVIDER_SYMBOLS` bump with
its delta comment, the module's incumbent-oracle reviewed-additions row where one exists (`srpc.client`
and `srpc.reactor` have them), and `test_goal0_contracts.py`'s hard-coded totals. Two further wires, both
measured: the exported name must be **unique across all 37 modules** — two modules exporting one name into
`namespace srpc` is an import-time ambiguity for any TU importing both (it broke the dual-compile importer
and the rpcbench link alike) — and the flat-import contract rejects importing a cross-module root-level
const outright, which is why such constants are spelled per-module.

**Errno values are spelled as raw numerics** (`SERVER_ERR_INVALID_ARGUMENT = 22`, the `TCP_ERR_*` block)
so generated modules stay valid alongside `errno.h`. Syscall numbers and build flags are the *opposite*:
`SYS_gettid` and `REUSE_FIBER` must never be Rust constants — their values are arch- and
build-dependent, so they go behind the plain-C seam (`srpc_reactor_gettid`, `srpc_reactor_reusing_fiber`).

**Bumping the transpiler pin means four edits**: the gitlink, plus the literal in
`scripts/extract_srpc_rust.py`, `scripts/check_srpc_crate_mode.py`, and `scripts/tests/test_goal0_standalone.py`.
The Verus coupling must still hold: a transpiler that vendors a different Verus fails the extraction check.

**Bumping the Lion pin** gets its own commit (`build: bump lion <old> -> <new>`) and is more than the
gitlink:
- re-resolve `Cargo.lock`; the `--locked` gates reject a stale one;
- if Lion adds, drops or moves a crate, update `LION_CRATES` in `check_rust_independence.py`;
- if Lion moves its Verus revision, update `VERUS_SOURCE` there and `EXPECTED_VERUS_VERSION` and
  `EXPECTED_RUST_TOOLCHAIN` in `scripts/verify_lion.sh`. rusty-cpp must re-vendor the erasure, and its
  pin must be bumped first, or the coupling check fails;
- re-measure the Lion provider inventory (`DEPENDENCY_PROVIDERS`, `_GHOST_ONLY`, `_UNUSED`,
  `_PRIVATE_IMPORTS`, `_ABI`) from fresh objects, and keep CMake's `SRPC_LION_PROVIDERS` in the same order;
- run `scripts/verify_lion.sh` on the new commit;
- keep the build warning-free. Lion's path crates compile **uncapped** under the gate's
  `RUSTFLAGS=-Dwarnings`, because Cargo caps lints only for non-path dependencies. `[workspace] exclude`
  only keeps them from being members, and clippy still lints only `srpc`. A new rustc lint or a Lion change
  can therefore fail SRPC's gate, so "Lion is warning-free on SRPC's toolchain" is a standing upstream
  requirement.

Never patch the submodule to get past any of these: the independence check rejects local changes, and
the identity claim would no longer hold.

## Testing

**Rust lane.** Cargo discovers `tests/*_rust.rs`, whose integration tests import the actual `srpc`
library, never a `#[path]` copy or a test-local `mod` implementation. The one sanctioned `#[path]` use runs
the other way: `tests/helpers/{event_wake_state,tcp_cork}.rs` are crate-internal `#[cfg(test)]` modules
that `reactor/reactor.rs` and `rpc/tcp_channel.rs` include so they can read private state. They never
reach the generated C++. `build.rs` links the shared native kernels. Runtime tests must not replace fiber
switches, clocks, sockets, or worker scheduling with inert symbols. Isolated fault injection must test a
stated native contract and be paired with real-kernel coverage.

Runtime tests run on real Lion runtimes over `SrpcEpollBackend`, as production does. `lion_runtime_rust`,
`lion_os_backend_rust` and `lion_foreign_wake_rust` cover the runtime, the `OsBackend`/`OsInterrupt`
forwarding and cross-thread wakes. `epoll_backend_rust` holds the backend to Lion's `OsBackend` contract.
`pollthread_lion_rust` covers the driver: jobs, stackless spawns, fiber sleeps and event wakes, the
`add_proxy` adapter, shutdown, wake latency and idle CPU. `tcp_transport_rust` covers the reader, writer
and accept tasks, write-through, and close/EOF ordering (the cork's interval test is
`tests/helpers/tcp_cork.rs`). `reactor_{wake_on_change,deadline,composite,ping}_rust` cover the
wake-on-change event model. The older suites still cover real TCP requests, a suspended handler sharing
its service with a fast request, timer ordering, foreign wake dispatch on the owner thread, retained wake
lifetime, fiber receive/close, and concurrent connection teardown. Serialization tests recover actual
payloads through canonical archives, holders and registries. One host setting matters:
`tcp_transport_rust`'s read-budget test needs `net.core.rmem_max` ≥ 4 MiB. Below that it passes without
reaching the budget, and says so only on stderr.

The Rust suite also includes property tests for wire round trips, malformed input, and stream chunking.
Run documentation tests separately, since `--all-targets` does not run compile-fail documentation checks.
Internal synchronization layouts may differ between languages; C++ ABI measurements belong in the
C++ gate, not in invented Rust-size equivalents. Keep tests for real public wire/ABI contracts.

**C++ lane.** CMake explicitly lists test sources. `tests/test-inventory.json` must account for every
`tests/**/*.cc` source, including exclusions with reasons and named Rust coverage. Configuration and
the source gate reject unaccounted sources; configuration also checks actual targets, default-build
membership and CTest registration. See [test-coverage.md](docs/test-coverage.md).
Use `ctest --test-dir build -N -L srpc` to inspect the configured inventory, then run
`ctest --test-dir build -L srpc --output-on-failure`. Missing googletest fails configuration when
`BUILD_TESTING=ON`. Vendored rusty-cpp tests have a separate inventory.
Historical test files may still depend on the upstream Mako layout. Their presence in `tests/` does not
prove they compile or run; check the actual CMake target and include paths.

`srpc_runtime_parity` executes `tests/runtime_parity_rust.rs` and `tests/runtime_parity_test.cc`, rejects
missing or malformed transcripts, checks independent expected results, and compares the two languages.
Its timing keys (timer order, deadlines, owner-thread completion) pin owner-driven resumption; review
them against the design rather than editing expectations until they pass.
The C++ dual-compile importer and ABI check remain separate. Run
`scripts/run_sanitizer_battery.sh [address|thread|undefined]` in separate configurations for runtime,
channel, ownership, and native-boundary changes. TSan matters most for Lion's trusted glue, which Lion's
CI does not run. A successful ordinary build does not establish sanitizer acceptance.

**Verus lane.** `verify/` is a workspace-excluded crate that `#[path]`-links the real sources and runs
`cargo verus verify` against them; five modules carry specs today — `base/basetypes.rs`, `misc/stat.rs`,
`rpc/errors.rs`, `rpc/frame_codec.rs` and `rpc/internal_protocol.rs`, each with a matching `#[path]` line in
`verify/src/main.rs`. A
canonical module may carry any contract that proves with **no in-body proof steps** — the transpiler's
preflight rejects opaque macros, so no in-body `proof!`. (`internal_protocol.rs` uses purely definitional
`ensures r == <wire-bit expression>`; `stat.rs` uses `requires old(self)…` / `ensures final(self)…` and still
stays in the module.) Only theorems needing `by (bit_vector)` move to `verify/src/*_proofs.rs`, which is
never transpiled. Adding a spec is a two-file edit: annotate the module, then add a `#[path]` line to
`verify/src/main.rs` — hand-maintained, nothing cross-checks it. Per `docs/verification.md`, **always run a
negative control**: perturb the body, confirm it goes red, revert. A green that never went red proves nothing.
Lion is verified separately, by its own CI (`scripts/verify_lion.sh`, above).

## Runtime architecture

Layering is `base/` → `misc/` → `reactor/` → `rpc/`, within one flat `srpc` crate, on top of
`lion-reactor` and `lion-executor`. `reactor/reactor.rs` imports the pollable contract from
`rpc/pollable_proxy.rs`. Its callers use canonical `crate::reactor` types and functions. Cargo uses the
Rust standard library, the reviewed C/assembly kernel and the allowlisted Lion closure; no facade package
or generated C++ runtime enters that dependency graph. C-layout declarations, fibers, events and wake
admission remain canonical Rust.

**Request path.** Generated proxy → `Client::request` → `ClientConnection::request` →
`clientconn_request_via_channel` (circuit-breaker gate → stale-request expiry → offline-queue check →
`Future::create(xid)` into `pending_fu_` → serialize `v64 xid | i32 rpc_id | args`) →
`ChannelConnectionProxy::send_frame` → **the TCP backend appends the 4-byte header** and the frame to the
connection's outbound buffer. On the buffer's empty→non-empty edge it wakes the connection's **writer
task**. A sender on another thread instead writes the frame through itself, under the outbound lock, when
the connection's last `send(2)` is at least `kTcpWriteThroughIdleUs` (20 µs) old: the *cork*, which keeps
batching on busy connections and latency on idle ones → the server's **reader task** (`rpc/tcp_channel.rs`,
*not* `server.rs`) reads until `EAGAIN` into its `FrameStreamReader`, which re-frames and fires `on_frame`
→ `sconn_decode_request_and_dispatch` → fast RPCs dispatch inline in the reader's poll on the poll thread,
everything else starts a stackful fiber there → `sconn_reply` writes
`v64 xid | v32 error | v64 server_instance_id | payload`. A reply sent inside the reader's poll wakes the
writer through the thread's local ready queue, so one drain carries every reply of that poll → the
client's reader task → `clientconn_decode_response_and_notify` resolves the async slot
(`xid % kAsyncSlotCount`, 16384) first, then the `pending_fu_` map. A reply matching neither is silently
dropped — the normal outcome after a timeout, and it leaves no trace.

**Wire format** (`rpc/internal_protocol.rs`, `rpc/frame_codec.rs`): 4-byte **native-endian** header — bit 31
is the extended-header flag, bits 0-30 the payload size. `kMaxFramePayloadSize` (64 MiB) is a
*stream-integrity* bound, not a resource policy: without it a desynced stream returns `NeedMoreBytes`
forever and the connection wedges silently. It must stay ≤ `i32::MAX - 4`. Note the TCP *send* path
(`tcpconn_send_frame`) open-codes the header rather than calling `frame_codec_write_header`, so a
header-layout change means editing both places.

**Channels** (`rpc/channel.rs`): two implementations — TCP (`rpc/tcp_channel.rs`) and in-memory
(`rpc/inmemory_channel.rs`, which is frameless and synchronous, so it can never reproduce a framing bug).
`FiberChannel` is *not* an implementation; it adapts callback delivery into a fiber-blocking `recv_frame()`.
TCP is auto-installed by `Client::connect` / `Server::start`; to use in-memory you must
`set_channel_factory` *before* connect/start.

**`PollThread`: one OS thread, one Lion runtime, one driver task.** The thread builds its runtime over
`SrpcEpollBackend` itself, because `Runtime` is `!Send`. It then blocks in `block_on` on one `spawn_local`
task, the **driver** (`PollDriverTask`). Woken through `PollDriverWake`, a pending flag plus its Lion
waker, the driver:
- drains `PollCommand`s and applies removals;
- runs ready jobs, in submission order;
- calls `run_loop(false, true)`, the same drain a thread with no loop runs. That drain serves event pings,
  the per-thread ready queue and the deadline map, and resumes fibers;
- sleeps on a Lion timer until `event_next_deadline_us`, rounded up to whole milliseconds (Lion's clock).

There is no polling interval, with one exception: a `Job` whose `Ready()` is false has no wake, so it is
re-checked every millisecond. Commands, pings, the ready queue's WAIT→READY edge, the stackless wake
ingress and an earlier deadline each wake the driver on their own edge; TCP frames never pass through it
(connect and listen hand their socket over by job). TCP connections run as a reader task and a writer
task over one `lion_reactor::AsyncFd`, and each listener as an accept task. A stackless task that is still
pending after its inline first poll becomes a Lion `spawn_local` task. An `add_proxy` pollable gets an
adapter task (`PollFdTask`). A task on a poll thread aborts the process if its poll unwinds
(`PollTaskUnwindAbort`).

**Fibers and events stay SRPC's.** The `Reactor` survives as the lazily created per-thread fiber and event
registry. Fibers are mmap'd stacks (1 MiB default + guard page) from `reactor/srpc_fiber.c`, switched by
`reactor/fiber_context_{x86_64,aarch64}.S`; the field order of `srpc_fiber_ctx` in `reactor/srpc_fiber.h`
*is* the ABI contract with that assembly. Events wake on change. `set()`, `vote_*` or a direct `test()`
moves an owner-thread event WAIT→READY and queues it on the owner's ready queue. A fiber **resumes only in
the owner's drain, never inside `set()`**: Mako's quorum code writes state after voting and relies on
this. Timers live in a per-reactor deadline map. Predicate events (`FiberChannel`) are re-tested when
pinged. `event_ping` runs on any thread: publish, then ping. A foreign-thread `set()` is therefore not a
wake; route it through a Job or a ping, or the waiter wakes only at its deadline, if it has one. A thread
with no `PollThread` keeps `run_loop`: `Fiber::create_run` (which runs the body to its first yield, then
drains once) and `Reactor::continue_fiber` stay synchronous, a waiting fiber resumes only when someone
pumps `run_loop`, and its stackless tasks use the `Reactor`'s own executor and wake-ticket ingress.
Per-thread state is real thread-local storage in both Rust and generated C++. Generated C++ runs stackless
futures on the compiler's coroutine runtime.

**Reliability layers** (circuit breaker, heartbeat, reconnect policy, connection state machine, request
queue, metrics) are embedded by value in `ClientConnection`. Only four configs are staged on `Client` and
applied at `connect` (keepalive, heartbeat, circuit breaker, reconnect policy). The request queue is the
trap: its `BufferingConfig` is *not* staged — `Client::set_buffering_config` silently no-ops until a
connection exists, so it must be called *after* `connect`. `LoadBalancer` is used only by `ClientPool`.

## Native and C++ adapters

`srpc.hpp` and compatibility headers import generated modules. Their include/import ordering matters
for libc++ module declarations; keep textual includes before named-module imports.
`misc/serializable_support.hpp` supplies C++ ADL and individual STL operations. The exported
`misc/serializable_adapters.hpp` epilogue supplies erased trait dispatch. Canonical Rust owns byte
handling, collection loops, errors and concrete holders. Compiler attributes use inert `cfg_attr`
markers and require no Cargo package or C++ marker header.
Global C declarations enter generated modules through `module-preambles.toml`, including
`reactor/srpc_epoll.h` and `reactor/srpc_fiber.h`.

`reactor/srpc_epoll.c` is six token-carrying leaves for `SrpcEpollBackend`:
- `srpc_epoll_create`;
- `srpc_epoll_ctl_token`, which stores a `u64` token in `epoll_event.data`;
- `srpc_epoll_wait_tokens`, which returns tokens and flags in two plain arrays of capacity 1–100, so no
  record layout is shared;
- `srpc_epoll_eventfd_{create,signal,drain}`, for the cross-thread interrupt.

Each leaf makes one system call and returns its result or `-errno`. EINTR retries, EAGAIN, the reserved
token 0, the mio-compatible flag mapping and timeout rounding are canonical Rust. The fd-keyed seam
(`srpc_epoll_open`/`_ctl`/`_wait` and `struct srpc_poll_event`) is retired; do not bring back a shared
event record.

There is no remaining production inline-Rust DSL carrier. `scripts/srpc_dsl_check.sh` enforces that
absence, and `scripts/check_native_kernels.py` rejects handwritten C++ implementation files under the
canonical directories. Native sources and headers are reviewed and pinned. Adding an adapter requires
an explicit contract and proof that SRPC policy still has a canonical Rust owner.

## Code generation (`pylib/`)

`.rpc` IDL → C++ header and Python stub, via a yapps-2 parser. Not wired into CMake at all; outputs are
checked in, and `tests/benchmark_service.rpc` is the only input. Three traps:

**`pylib/simplerpcgen/rpcgen.py` is the live generator and has been hand-edited since generation.**
`rpcgen.g` is a stale grammar whose epilogue lacks `load_existing_rpc_codes`, the `existing_codes` argument,
and the `archive` flag. **Never regenerate `rpcgen.py` from `rpcgen.g`** — it would drop the id
stabilization below. (There is no yapps compiler vendored anyway; `pylib/yapps/` is runtime-only.)

**RPC method ids are `random.randint(...)`, stabilized only by scraping them back out of the previously
generated `.h`.** Never delete the generated header before regenerating, or every id changes and wire
compatibility silently breaks. Renaming a service function reassigns its id for the same reason.

**`bin/rpcgen` does not exist here.** Drive the generator through `simplerpcgen` with `pylib/` on
`sys.path`; `tests/rpcgen_typed_structs_test.py --repo .` uses that entry point. Generated service handlers
and dispatch wrappers are const-callable; user overrides must match and synchronize mutable state.
`rpcgen_compile_test.py` still names upstream `src/deptran/*.rpc` paths absent from this checkout.

## Conventions

**Commits:** `<scope>: <lowercase imperative>` where scope is the canonical module basename
(`frame_codec:`, `stat:`, `client:`) or an area (`build:`, `docs:`, `tests:`, `verify:`, `gate:`, `goal0:`).
`srpc:` for tree-wide changes. The legacy `rrr:` prefix is retired — do not reuse it. Because there is no CI,
bodies carry the audit trail: a narrative of the defect and a `Verified:` paragraph with *measured* numbers
(test counts, configure/build exit codes, the ABI symbol count). A minority of test-touching commits (46 of
the 402 that touch `tests/`) also add a `Tests:` paragraph naming the new test — `d81de59` and `21ce10a` are
the recent examples, while `0e51bce` changed `tests/stat_rust.rs` without one. Transpiler pin bumps get their
own commit, `build: bump rusty-cpp <old> -> <new>`, and so do Lion pin bumps, `build: bump lion <old> -> <new>`.

**Style:** `//` line comments only, with long "why this constant exists" blocks as the house norm. Most
constants are `SCREAMING_SNAKE_CASE` (`TCP_ERR_AGAIN`, `SERVER_ERR_INVALID_ARGUMENT`); 15 keep C++-style
`k`-prefixed camelCase — mostly framing, reactor and TCP (`kFrameHeaderSize`, `kResponseSizeMask`,
`kDefaultStackBytes`, `kTcpWriteThroughIdleUs`) but also `kAsyncSlotCount` in `client.rs`,
`kDefaultDrainTimeoutMs` in `server.rs` and `kRequestQueue*Error` in `request_queue.rs`. Match the
surrounding file. `unsafe_code` is denied crate-wide; eight files carry a file-scope
`#![allow(unsafe_code)]` (`reactor/{reactor,fiber}.rs`,
`rpc/{client,server,tcp_channel,inmemory_channel,fiber_channel}.rs`, `misc/any_message.rs`) and elsewhere
`unsafe` gets a narrow per-item `#[allow(unsafe_code)]` — never relax the crate-level deny. There is no
rustfmt/clippy/clang-format config.

**`.apas` in the repo root is an agent-harness session file, untracked and ignored via `.gitignore`
(`/.apas`).** Never commit it: `git add -A` skips it only because of that ignore rule, so do not `git add -f`
it or loosen the rule.

## Historical documents

[lion-runtime-plan.md](docs/dev/lion-runtime-plan.md) is the record of the move onto Lion. Its decisions
(D1–D5), upstream prerequisites (U1–U9), transpiler track (T1–T9) and SRPC phases (S0–S8) each carry
result notes with the measured numbers, commits and remaining items; S8's notes hold the accepted
numbers and the performance record. The migration is done and accepted (2026-10-04). Still open: S6
(cooperative client waits; optional, not taken, and the same-PollThread nested-RPC limitation is
recorded there) and rusty-cpp's T8 (what its own gate must show, the owner's decision). U1b (Lion's
container speed) was judged unnecessary for SRPC.

[translation-parity-audit.md](docs/translation-parity-audit.md) records the pre-repair baseline and its
original findings. Its source line numbers, counts, and removed facade paths refer to the audited
revision. [canonical-rust-runtime.md](docs/canonical-rust-runtime.md) describes the ownership split
between Lion and canonical Rust, the native kernels, and the facade-removal and API migration
contracts; its validation tables are historical, for the trees they name. This file and the plan win
where they disagree.
[facade-and-runtime-remaining.md](docs/dev/facade-and-runtime-remaining.md) records the facade
retirement and its 2026-09-13 acceptance; a dated note there records the D5 allowlist that replaced
its "no production Rust dependencies" status.

`RUST_CANARY.md` and `reactor/CANONICAL_CHECKPOINT.md` contain older inventories and compiler blockers.
`docs/srpc-book.md` documents the current native Rust APIs; `docs/srpc-cpp-book.md` covers generated
C++ APIs and translation. Older book revisions contain mutable service/channel and reply-guard
examples that no longer match the sources. Use `git show` at the recorded baseline when investigating
a historical claim.

When prose and code disagree, `CMakeLists.txt`, `rust-modules.toml`, `Cargo.toml`, and the current gates
in `scripts/` define the build contracts. Record measured results from the revision actually being
accepted.
