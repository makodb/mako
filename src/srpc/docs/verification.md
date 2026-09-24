# Formal verification of srpc with Verus

srpc's canonical sources are Rust (rustc compiles them; rusty-cpp translates the
same bytes into the production C++). Because the source of truth is Rust, we can
run [Verus](https://github.com/verus-lang/verus) — an SMT-backed verifier for
Rust — directly on the real modules and machine-check functional contracts.

This document is the standing reference for that work: how the harness is wired,
how to run it, what is proven today, and how to prove the next thing.

## TL;DR

```sh
# with a Verus dist (ships both `cargo-verus` and `verus`) on PATH, or:
VERUS_HOME=/path/to/verus-x86-linux scripts/verify_srpc.sh
# => vstd  2045 verified, 0 errors
#    srpc  51 verified, 0 errors
```

## The route: `cargo verus verify` over an excluded `verify/` harness

An earlier attempt concluded `cargo verus verify` was "unusable" with the
prebuilt dist. **That was wrong about the cause.** cargo-verus does *not* build
the dist's `builtin_macros/` (the crate with unresolved `{ workspace = true }`
deps). It sets `RUSTC_WRAPPER=verus` plus a set of `__VERUS_DRIVER_*` env vars
and runs a plain `cargo check`, resolving `vstd`, `verus_builtin`, `verus_syn`,
`verus_prettyplease`, `verus_state_machines_macros` as ordinary **crates.io
registry** crates (their versions match the dist's `version.txt`). The only real
blocker was that srpc was not shaped as a Verus package.

The `verify/` package supplies that shape without perturbing production:

- **Workspace-excluded** (`exclude = ["verify"]` in the root `Cargo.toml`). It
  never enters the production `cargo build` or the rusty-cpp transpile, and the
  srpc crate itself keeps **no `vstd` dependency**. `verify/` has an empty
  `[workspace]` table so it is its own single-package root.
- **Verifies the real sources in place.** `verify/src/main.rs` pulls each
  specced module with `#[path = "../../<module>.rs"] mod ...;` — the same bytes
  rustc compiles and rusty-cpp translates, not an extracted copy.
- **Gate discipline that keeps production byte-identical.** Specs live behind
  `#[cfg(verus)]` / `#[cfg_attr(verus, ...)]`. `verus` is the *same* cfg the
  rusty-cpp transpiler strips, so production never sees the annotations or the
  `use vstd::prelude::*;`. cargo-verus only turns on `verus_only`, so
  `verify/.cargo/config.toml` forces `--cfg verus` **locally** to activate them:

  ```toml
  [build]
  rustflags = ["--cfg", "verus"]
  ```

  The extra cfg is inert for the vstd / verus_* dependency crates (built with
  `--cap-lints allow`).

> ⚠️ **Do not rename the in-file gate to `verus_only`.** The pinned transpiler
> (`third-party/rusty-cpp`) special-cases only the ident `verus`; a
> `#[cfg(verus_only)]` in a canonical module breaks the whole-crate transpile
> (`--crate Cargo.toml` → build failure). Keep `verus` in the module; force it
> in `verify/`. This is also why the harness needs no transpiler change and no
> gitlink pin bump.

### Why not the standalone driver?

The dist also ships prebuilt `libvstd.rlib` / `libverus_builtin.rlib`, and the
bare `verus <file> --cfg verus` driver links them with no cargo at all. That was
the first unblock, and it still works as a fallback, but only for **dependency-
free** leaf files. The cargo-verus route resolves real dependencies and is the
supported path, so it is what `scripts/verify_srpc.sh` uses.

## What is proven today

Five modules carry in-place, machine-checked contracts (`misc/stat.rs`,
`rpc/internal_protocol.rs`, `rpc/errors.rs`, `base/basetypes.rs`, `rpc/frame_codec.rs`).
`scripts/verify_srpc.sh` reports `51 verified, 0 errors` for the srpc crate (plus
vstd's own proofs, which are the standard library's, not srpc's). See "The
provable surface" below for the per-module breakdown.

### `misc/stat.rs` — `AvgStat::sample` first-sample seeding

> For a fresh stat (`n_stat_ == 0 && sum_ == 0`) and any `s` with
> `i64::MIN < s < i64::MAX`, after `sample(s)`: `min_ == s && max_ == s`.

```rust
#[cfg_attr(verus, verus_spec(
    requires old(self).n_stat_ == 0, old(self).sum_ == 0,
             s > i64::MIN, s < i64::MAX,
    ensures  final(self).min_ == s, final(self).max_ == s,
))]
pub fn sample(&mut self, s: i64) { ... }
```

The other `AvgStat` methods are `#[verus_verify(external_body)]`: their
signatures are trusted, their bodies not checked. **Honest scope:** this pins the
*first-sample seeding* invariant — exactly the bug that shipped — **not** the
general "min_/max_ are the true extrema over an arbitrary stream," which needs a
loop/recursive spec and is not done.

### `rpc/internal_protocol.rs` — header-codec round trip

The wire header packs a 31-bit payload size and a 1-bit extended-header flag into
one `i32`. Both directions are proven, with Verus bit-vector reasoning:

1. **Decoded size is always non-negative and in range** — `response_payload_size`
   returns a value in `[0, kResponseSizeMask]` for *every* input. This is exactly
   the fact `frame_codec::peek_header` relied on by comment ("payload_size is
   never negative"); it is now a checked theorem.

2. **Encode/decode is a lossless round trip on the valid domain** — for any
   `payload_size >= 0` and any `flag`:
   `response_payload_size(encode_response_size(payload_size, flag)) == payload_size`
   **and** `response_has_extended_header(encode_response_size(...)) == flag`.
   The `payload_size >= 0` precondition is real: encode masks the top bit off, so
   a negative size genuinely cannot round-trip, and the spec says so rather than
   silently truncating.

Where the proof lives is deliberate. `internal_protocol.rs` carries only
**definitional** contracts on each function (`r == <the exact wire-bit
expression>`), which need no in-body proof. The bit-vector reasoning for the two
theorems above lives in `verify/src/internal_protocol_proofs.rs`, which calls the
real functions and reasons from their contracts. It sits in the harness, not the
srpc crate, because the C++ transpiler's preflight rejects any opaque macro
(including Verus's `proof!`) inside a body it must translate — so in-body
`proof! { assert(..) by (bit_vector) }` is confined to files that are never
transpiled. Definitional contracts are the transpile-safe surface a verified
module exposes; the proofs that consume them live in verify/.

## Bugs this effort fixed

| Commit | Bug | Under proof? |
| --- | --- | --- |
| `0e51bce` | `AvgStat` seeded `max_`/`min_` from zero-init fields, so an all-positive stream left `min_` stuck at 0 and an all-negative stream left `max_` stuck at 0 — neither the true extremum. | ✅ guarded by the `sample` contract |
| `b8be721` | `frame_codec` did not bound the frame size, so a desynchronised stream (short write, mid-frame reconnect, upstream bug) was read as a garbage-length header and the connection wedged silently instead of erroring. | ⚠️ fixed by hand; the assumption it leans on (`response_payload_size` is never negative) is now proven — see `internal_protocol` above. The `peek_header` size *bound* itself is now proven too (T6). |
| `e376fd6` | The client did not drain its disconnect buffer on teardown. | ❌ fixed by hand |

The `0e51bce` bug is instructive: it had been pinned as *correct* in six
implementation-derived oracles (a Rust test and a C++ runtime contract both
asserted `max == 0`). Every oracle derived from the buggy code blessed the bug.
The Verus spec was the only artifact that could not be satisfied by the wrong
implementation — which is exactly why it surfaced it. The negative control
confirms the spec bites: reintroduce the seed bug and `cargo verus verify` fails
with `postcondition not satisfied`.

## Adding the next leaf

1. Put the specs in the real module, gated on `#[cfg(verus)]` /
   `#[cfg_attr(verus, verus_spec(...))]`. Mark every sibling method the verifier
   should trust rather than check with `#[cfg_attr(verus, verus_verify(external_body))]`.
   For `&mut self`, use `old(self)` in `requires` and `final(self)` in `ensures`.
2. Add one line to `verify/src/main.rs`:
   `#[path = "../../<module>.rs"] mod <name>;`
3. If the module needs `vstd` lemmas, keep `#[cfg(verus)] use vstd::prelude::*;`
   at its top — stripped in production, active under the harness.
4. `scripts/verify_srpc.sh` and confirm `N verified, 0 errors`.
5. **Always run a negative control**: perturb the body so the spec should fail,
   confirm it goes red, then revert. A green that never went red proves nothing.

Only files that compile as leaves (no `crate::` / `rusty::` items the verifier
can't model) drop straight into the harness. Deeper modules need those types
given Verus models or marked external first.

**In-body proof vs. the transpiler.** A canonical srpc module is transpiled to
C++, and the transpiler's preflight rejects any opaque macro — including Verus's
`proof!` — inside a body it must translate. So a spec that needs bit-vector or
other in-body proof steps cannot keep them in the module. The pattern (see
`internal_protocol.rs` + `verify/src/internal_protocol_proofs.rs`): give the
module function a **definitional** contract (`r == <its exact expression>`, which
proves with no in-body steps and transpiles cleanly), then prove the real theorem
in a `verify/src/*_proofs.rs` driver that calls the function and does the
`proof! { assert(..) by (bit_vector) }` there. Files under `verify/src/` are never
transpiled, so they may use any Verus proof construct. A spec whose contract
proves without in-body steps (like `stat.rs`'s `sample`) can stay entirely in the
module.

## Recommended next property

**Lift the round trip one layer up, into `rpc/frame_codec.rs`.** The
`internal_protocol` proofs are the foundation; the next step is to make the frame
header itself — the thing actually on the wire — carry the same guarantees:

1. **`frame_codec_write_header` → `frame_codec_peek_header` round trip.** A header
   written for a `payload_size` in `[0, kMaxFramePayloadSize]` peeks back as
   `Complete` with the same `payload_size` and `extended_header_flag`. This builds
   directly on the `internal_protocol` round trip (write_header calls
   `encode_response_size`, peek_header calls `response_payload_size` /
   `response_has_extended_header`).

2. **`FrameHeader::total_frame_size()` never overflows or goes negative** for a
   peeked header. `peek_header` guarantees `payload_size <= kMaxFramePayloadSize`
   and `kMaxFramePayloadSize <= i32::MAX - kFrameHeaderSize`, so the
   `saturating_add` provably never saturates and the result stays positive —
   which is what makes the `as usize` casts in `next_frame` / `consume_frame`
   safe. This is the property that turns the hand-fixed `b8be721` stream-integrity
   bound into a proven one.

Both are still leaf-friendly: `peek_header` and `write_header` operate on plain
`&[u8]` / `&mut [u8]` slices, no `FrameStreamReader` cursor or raw-pointer
`append`. Property 2 needs the `kMaxFramePayloadSize` bound stated as a fact.

Beyond that, the harder tier is `FrameStreamReader` itself (buffer/cursor
invariants, the raw-pointer `append` and `consume_frame` compaction) — valuable
but it needs Verus models for the cursor and `unsafe` pointer reasoning, so it is
a project rather than a leaf.

## Verus proof target list, and the tooling walls each hits

Grounded in what Verus can discharge in this repo: pure/definitional properties
of the actual shipped functions (contracts in the canonical source behind
`#[cfg(verus)]`, bit-vector theorems in `verify/src/*_proofs.rs`). Concurrent /
stateful properties (deliver-once, the pending-future map, the reactor) are out
of scope — they need ghost/tokenized state Verus cannot carry in canonical
sources, and would prove a model rather than the code.

The list below was worked through end to end and **every target on it is now
proven**. Each was initially blocked by a distinct wall; the walls and how each
was cleared are recorded here, because the workaround -- not the contract -- is
the reusable part.

**Watch for false greens:** the `verify/` crate `#[path]`-links the real sources
and runs *only* `cargo verus verify`; it never invokes the rusty-cpp transpiler.
A target can therefore verify green in `verify/` and still be un-shippable because
adding its `#[cfg(verus)]` content breaks the whole-crate transpile — which is
exactly how the errors bug below hid until the full `cmake --build` ran. A target
is only real once `cmake --build` still produces the 1967-symbol archive with the
contract in place.

Toolchain note: `verify/Cargo.toml` pins `vstd = "=0.0.0-2026-08-30-0159"`, the
latest stable Verus release (`0.2026.08.30`, rustc toolchain 1.97.1). Bump the
`vstd` pin and the Verus dist together (a `vstd` newer than the `verus` binary
panics compiling vstd), and prefer a stable release over the `rolling`
prereleases. The full lane reports **51 verified, 0 errors** on this dist; the
proofs' logic is dist-independent (the walls below were confirmed unchanged from
08-09 through the 09-06 rolling build).

### The provable surface (green)

Modules carrying `#[cfg(verus)]` contracts today, all proven:

- `rpc/internal_protocol.rs` (+ `verify/src/internal_protocol_proofs.rs`) — the
  response-header codec: `response_payload_size` is in the 31-bit range, and
  `encode_response_size` → `response_payload_size`/`response_has_extended_header`
  round-trips size and flag for every non-negative payload. Bit-vector proofs.
- `misc/stat.rs` — the running accumulator's `requires`/`ensures`.
- `rpc/errors.rs` (+ `verify/src/errors_proofs.rs`) — the error-classification
  predicates (see the first target below). Added once the transpiler bug that
  blocked it was fixed.
- `base/basetypes.rs` — the sparse-int length bounds `sparseint_val_size(v) ∈
  1..=9 ∧ ≠ 8` and `sparseint_buf_size(b) ∈ 1..=9` (T1–T3), **and the full
  encode/decode round trip `load64(dump64(v)) == v` for every `i64`, all eight
  length classes (T4)** — see the SparseInt targets below. The `≠ 8` and the
  round trip are the length-8 wire fix machine-checked. Proofs in
  `verify/src/basetypes_proofs.rs`.
- `rpc/frame_codec.rs` (+ `verify/src/frame_codec_proofs.rs`) — the frame header:
  the peek bound (T6) and the write->peek round trip (T5). See the frame_codec
  target below, including the one trusted axiom T5 rests on.

Total: **51 verified, 0 errors.**

Two transpiler-audit bugs had to be fixed to get here, both the same class: a
check auditing `#[cfg(verus)]` items that are absent from the transpiled program.
Before the first, only modules exporting **no enums and no derives** could host
contracts; before the second, only modules that **import nothing from a sibling**
could. With both fixed, any leaf module can carry `#[cfg(verus)]` contracts. The
remaining Verus-tooling limits (associated fns, `from_ne_bytes`) are real but
were worked around in the source -- see each target below.

### Targets and their walls

- **errors classification** (`get_error_category`, `is_connection_error`,
  `is_timeout_error` in `rpc/errors.rs`) — **DONE.** Pure integer-range
  predicates, and a real bug class (edit one range, forget the matching branch)
  that `errors_rust.rs` only samples by example. `errors_proofs.rs` proves
  disjointness of the connection/timeout classes and predicate/categorizer
  consistency, via `#[verifier::external_type_specification]` wrappers over the
  enums.

  This was blocked, and the block was a *transpiler* bug, not a Verus one. The
  moment any `#[cfg(verus)]` item (even a bare `use vstd::prelude::*;`) appeared
  in `errors.rs`, the whole-crate transpile aborted with *"cpp_default_argument
  cannot prove that item attribute `allow(non_camel_case_types)` is free of
  macro-generated bindings in module `errors`"*. Root cause: the default-argument
  audit's `collect_signature_type_model` built its glob-import model **without
  evaluating cfg**, so the verification-only `vstd` glob was counted as a live
  glob and marked the module macro-tainted — which then blocked the audit of the
  module's own real `#[derive(...)]` enums. That is why, before the fix,
  internal_protocol and stat (no enums, no derives) were the only annotatable
  modules. Fixed in rusty-cpp by skipping definitely-false-cfg items in the model
  builder, mirroring the guard the attribute-audit loop already had (pin bumped
  `5b61f96` → `358b351c`). The generated `.cppm` is unchanged and the ABI stays
  at 1967 — the verus content still lowers to nothing.
- **T1–T3 SparseInt bounds** (`base/basetypes.rs`) — **DONE.**
  `sparseint_val_size(v) ∈ 1..=9 ∧ ≠ 8` and `sparseint_buf_size(b) ∈ 1..=9`. The
  `≠ 8` is the length-8 wire fix machine-checked (negative control: returning 8
  fails the postcondition). **The wall was real and required a source change to
  clear.** `SparseInt::val_size`/`buf_size` are *associated* (impl) functions,
  and the `verus_spec(r => ensures …)` return-binding macro emits an unqualified
  self-call (`#receiver_token#fn_ident(#args)` with an empty receiver →
  `E0425 cannot find function`), confirmed in the 08-30 macro source
  (`builtin_macros/src/syntax.rs:1107`) and unfixed through the 09-06 rolling
  build. Since no Verus version fixes it, the length logic was **extracted into
  free functions** `sparseint_val_size`/`sparseint_buf_size` (which the macro
  handles), with the methods delegating — so the proven code is the shipped code.
  Cost: +2 provider symbols (ABI `1967 → 1969`, an ordinary ratchet edit). The
  bodies are the original logic verbatim; Verus discharges the bound over the
  inclusive-range `.contains()` checks directly.
- **T4 SparseInt round trip** `load64(dump64(v)) == v` for every `i64` — **DONE.**
  Eight theorems in `verify/src/basetypes_proofs.rs`, one per length class, each
  encoding a value with the real `sparseint_dump64` and decoding with the real
  `sparseint_load64` and proving the result equals the original (bit-vector, incl.
  sign extension). Negative control: perturbing a marker mask in the body fails a
  postcondition. Getting here needed three source changes, each behind
  `#[cfg(verus)]`-transparent restructuring: (a) **slices** — `dump64`/`load64`
  reshaped from raw pointers to `&mut [u8]`/`&[u8]` (Verus reasons about a slice's
  `Seq` view, not raw-pointer memory; also a safety win — bounds-checked); (b)
  **free functions** — `sparseint_dump64`/`load64`, since the `verus_spec` macro
  cannot spec associated fns (the methods delegate); (c) **unrolled, functional**
  bodies — canonical code cannot carry loop invariants (a `verus!` block the
  transpiler rejects), so the loops became straight-line per-length branches, each
  a single expression that matches its definitional contract without an in-body
  `bit_vector` hint. The composing round-trip theorems live in the proofs file
  where `bit_vector` is allowed. ABI cost: +2 provider symbols (1969 → 1971).
- **T5–T6 frame_codec** (`rpc/frame_codec.rs`) — **DONE**, by moving the
  unsupported call behind a trusted boundary rather than waiting on vstd. Verus
  still cannot process `i32::from_ne_bytes`/`to_ne_bytes` (vstd does not specify
  them, and `assume_specification` cannot match their const-generic array
  signature — confirmed on 08-09, 08-30 and 09-06). The workaround is the shape
  vstd's own bytes.rs uses: isolate each std byte call in a tiny
  `#[verus_verify(external_body)]` helper (`header_word_from_bytes`,
  `store_header_word`), so the rest of the codec verifies normally.

  **T6 (the peek bound)** `Complete ⇒ 0 ≤ payload_size ≤ kMaxFramePayloadSize` —
  the reason the reader's `as usize` casts are safe. It costs **no semantic
  trust**: the read helper deliberately carries NO `ensures`, so nothing is
  assumed about what the leader bytes decode to; the bound comes from the
  function's two range guards alone. Negative control: delete the upper guard and
  the postcondition fails.

  **T5 (write→peek round trip)** recovers both the size and the flag for every
  in-range size and either flag. Its bit-packing half was already proven in
  `internal_protocol_proofs`; what T5 adds is the 4-byte marshalling, and *that*
  rests on **one trusted, target-conditional axiom**: the helpers' `ensures`
  state the little-endian decomposition, which is what native-endian marshalling
  is on the little-endian targets srpc supports — the same assumption
  `tests/wire_golden_rust.rs` already encodes in its byte vectors. It could not be
  stated as the endian-agnostic `from(to(x)) == x`, because that needs one exec
  helper called inside the other's spec, which Verus disallows. Negative control:
  store `encoded + 1` and the postcondition fails.

  Landing this needed a second transpiler fix (pin `358b351c` → `2abea1dc`):
  frame_codec is the first specced module that also **imports from a sibling**,
  so two more `cpp_abi` `use`-visitors audited the `#[cfg(verus)]` vstd glob that
  is not in the transpiled program at all. Both now apply the cfg-absent guard the
  file already used elsewhere. ABI `1971 → 1973`: the two helpers are
  module-internal (emitted without `export`) but still land as strong symbols in
  the object, so they are ordinary ratchet rows.

  **Measured cost: none that reproduces.** An earlier revision of this document
  reported a **+12%** regression on `frame_codec_write_header` (~2.89 -> ~3.24
  ns/op, 20M iterations x 4 runs, `68dfaf1` vs `c213501`), and reasoned at length
  about instruction layout to explain it. That number came from a throwaway
  harness that was never committed, and it does not survive contact with one that
  is.

  Re-measured with `bench/` -- the same two commits, the same machine, the same
  sitting, built alternately so thermal drift lands on both sides, with
  `black_box` on every operand and `opt-level=3 / lto / codegen-units=1` pinned
  in the profile:

      68dfaf1 (before)    min 2.606, 2.625 ns/op
      c213501 (after)     min 2.580, 2.603 ns/op
      run-to-run spread   0.082 - 0.118 ns

  The "after" side is marginally *faster*, by roughly a quarter of the noise
  floor. There is no regression, and the instruction-layout account the old
  paragraph offered was rationalising an artifact. The likeliest cause of the
  original figure is a loop the optimiser was free to treat differently between
  the two versions -- but that harness is gone, so it stays labelled a guess.

  The lesson is the one worth keeping: an uncommitted benchmark is not evidence,
  and a number nobody can re-take will be believed anyway, including by the
  person who took it. Reproduce this one with
  `scripts/run_microbench.sh --compare 68dfaf1 c213501`.

  The C++ lane, which is what ships, remains unmeasured at this resolution, and
  rpcbench cannot close that gap: a sub-nanosecond leaf effect is ~0.05% of a
  request at the qps it reports, far under its trial spread.

Net: the self-paced pass shipped every target on the list — errors, SparseInt
T1–T3, the SparseInt T4 round trip, and frame_codec T5/T6 (10 → 51 verified). It
took two transpiler fixes (both the same bug class: a check auditing
`#[cfg(verus)]` items that are absent from the transpiled program) and a sequence
of `#[cfg(verus)]`-transparent restructurings (free-function extraction, slices,
unrolled functional bodies, and `external_body` isolation of unsupported std
calls). The version question is settled: no available Verus (through the 09-06
rolling build) fixes the associated-fn `verus_spec` macro or adds `from_ne_bytes`
support — both were worked around in the source instead. The only trusted claim
in the whole lane is T5's little-endian marshalling axiom; everything else is
proven.
