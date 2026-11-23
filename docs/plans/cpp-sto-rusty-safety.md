# C++ STO rusty-safety: deferred investigation and plan

## Current decision — 2026-10-24

The user-approved work is [native Rust protocol alignment](rust-sto-rustycpp-alignment.md),
not C++-safety-first conversion or a shared generated C++ STO core. Native Rust
remains native. The real core/storage/coordinator/participant/replication paths
are implemented and undergoing final runtime qualification; see the
[current architecture contract](../architecture/rust-sto.md).

**The broad C++ ownership/checker campaign below is deferred.** It is neither
a prerequisite for native alignment nor completed by narrow rusty migrations
in touched C++ bridges. There is no current instruction to convert STO to
generated C++, retire the C++ backend, or resume performance qualification.
Performance remains user-stopped; no throughput or cost parity is claimed.

## Historical proposal and findings — 2026-10-16

The remaining sections preserve the earlier safety-first proposal and its
executed investigation. Their phase instructions, source findings and exit
criteria are historical/deferred, not the current implementation sequence or
current source guarantees. In particular, references to the then-local-only
Rust integration do not describe today's implemented native protocol. The
failed checker controls below remain failed; no later native result qualifies
that checker or establishes broad C++ memory/concurrency safety.

Checker acceptance is not a complete proof: native pointer, atomic, allocator
and RCU obligations require their own review and executable validation.
rusty-cpp translates Rust to C++, not C++ to Rust; any independently authorized
future conversion would require its own scope and evidence.

## 1. What "rusty-safe STO" means

Partition the in-scope code into three explicit categories:

- **Checked safe logic:** ownership, bounds, borrowing, state transitions, and
  ordinary control flow use supported representations and actually enter
  the checker's analyses with the correct production compile configuration.
- **Audited native kernels:** small, named unsafe operations with explicit
  allocation, lifetime, alignment, aliasing, synchronization, and reclamation
  preconditions. Their safe callers establish those preconditions.
- **Not yet covered:** unannotated bodies, unsupported constructs, incomplete
  parses, and unaudited unsafe operations. These remain outstanding work,
  not a successful check or an accepted permanent exclusion.

The target is a checked protocol/ownership layer over a small audited native
boundary—not zero raw pointers, zero unsafe operations, or a high annotation
count. A wrapper marked safe is not sound if its accepted inputs can violate
its native kernel's preconditions. `@bridge`, whole-function `@unsafe`, or a
trusted-library call transfers a proof obligation; it does not discharge it.

### Scope

Start with the live production C++ STO dependency closure: `Transaction`,
`TransItem`/`TransProxy`, `Packer`/`TransactionBuffer`, version operations,
`MassTrans`, RCU/worker registration, mbta/local ABI, participants, and replay.
Include the reference datatype consumers affected by core interfaces, including
`TBox`, `TCounter`, and proxy users in vector/predicate code.

Phase 1 must classify the remaining files under `src/mako/sto` as production,
public/reference, test/benchmark, or dormant. Do not claim the entire directory
safe by checking only a scalar demo or silently excluding a public datatype.
Dormant-code retirement is a separate decision, not an automatic cleanup here.

Update, 2026-11-22: that decision has been taken. The dormant and
test/benchmark files (`TCounter`, `TVector`, `TArray`, the predicate types,
`RBTree`, the queues, Boosting, the `unit-t*` programs and the benchmark
programs; 64 files) were deleted. The last commit that has them is `fd3ed37e1`;
read one with, for example, `git show fd3ed37e1:src/mako/sto/TCounter.hh`. Of
the reference datatypes named above only `TBox` remains (used by
`tests/test_silo_runtime.cc`). `src/mako/sto` now holds 37 files.

At the time of this proposal, it excluded changes to the native Rust STO's
naming, storage layout, backend selection and public behavior. That historical
scope restriction does not constrain the separately approved native alignment.

## 2. Findings from the 2026-10-16 investigation

### The investigated checker gate was not a reliable STO acceptance gate

Source investigation on 2026-10-16 found:

- Root CMake defaults `ENABLE_BORROW_CHECKING` to OFF. Even with it enabled,
  the STO registration sets both `MAKO_ROOT_SRCS` and `STO_SRCS` to empty
  lists (`CMakeLists.txt:1953-1993`). Adjacent Masstree/Deptran checks are not
  proof of direct STO coverage.
- Unannotated functions default to unsafe, and the borrow-analysis loop skips
  unsafe/unannotated bodies. Unsafe blocks also bypass important ownership
  checks. Contrary comments in the implementation do not change that behavior.
- The parser suppresses ordinary libclang errors and some fatal module errors;
  it can continue with a partial AST. An empty violation list is therefore
  insufficient evidence that the intended program was analyzed.
- The current CLI does accept `--compile-commands` and module response files,
  so the old root comment about missing BMI support is not the whole story.
  However, argument extraction drops some real compiler options, missing
  entries can fall back to defaults, and module recovery can remove inputs.
- The effective CMake integration may reuse an already-existing checker
  executable without rebuilding it for source changes. `BORROW_CHECK_FATAL`
  is not a strict-parser switch. Selected bodies, exclusions, tool identity,
  and exact compile configuration must be part of the gate.

References:
[`CMakeLists.txt`](../../CMakeLists.txt),
[`RustyCppSubmodule.cmake`](../../third-party/rusty-cpp/cmake/RustyCppSubmodule.cmake),
[`checker CLI`](../../third-party/rusty-cpp/src/main.rs),
[`parser`](../../third-party/rusty-cpp/src/parser/mod.rs),
[`borrow analysis`](../../third-party/rusty-cpp/src/analysis/mod.rs).

### Concrete C++ ownership boundaries

| Area | Current ownership/lifetime | Refactor direction |
| --- | --- | --- |
| Transaction item storage | Embedded 512 live items plus stable overflow chunks; first directory entry aliases embedded storage | Explicit noncopyable owner; preserve fixed capacity, addresses, chunking, and live-slot reset |
| TransactionBuffer | Owns arena chunks and erased destruction records; normal clear retains a chunk | Single-owner arena with a proved constructed-prefix/destruction contract |
| TransItem | Owns `extra`; owner, key, and packed read/write words have other owners or inline-value representations | Distinct typed handles/views around private packed representation |
| TransProxy | Temporary raw borrower, currently copyable and capable of exposing mutable references | Scoped item access tied to the same active attempt; no escape across reset/relocation |
| StringWrapper | Borrows a caller string, including through const_cast; shares an erased packer with owned strings | Explicit owned versus borrowed value contract and caller-specific lifetime/constness |
| MassTrans rows | Tree/RCU-owned; private inserted rows additionally carry abort-removal obligations | Epoch-protected row handles and explicit transfer/retirement responsibilities |
| Worker/transaction | Stable process-wide IDs and intentionally long-lived runtime state | Explicit thread-affine ownership/admission; preserve existing lifetime and ID policy |
| Participant | State and locks survive RPCs; INSTALL can be irreversible before terminal cleanup | Distinct prepared/installed/terminal obligations; no generic rollback-on-unwind shortcut |
| Local ABI/replay | Stable facade storage or synchronous record-at-a-time borrows; uncertainty may quarantine owners | Preserve exact last-consumer and release boundaries, not universal copying |

## 3. Invariants that this preparation must not change

- Preserve supported C++ isolation, predicate, RYW, duplicate-read, capacity,
  and topology behavior. The historical native Rust integration had a narrower
  non-opaque/local-only scope; that is not today's native protocol contract.
- Preserve datatype-specific publication: MassTrans retains row locks during
  install; TBox/TCounter publish/unlock during install. Safety work must model
  these existing policies, not standardize their order as a side effect.
- Preserve coordinator and participant irreversible decisions, definite aborts,
  terminal exhaustion, hook rejection, fail-stop, and cleanup uncertainty.
  Never retry indeterminate work or perform abort cleanup after INSTALL.
- Preserve current destruction timing. TransactionBuffer payloads normally
  survive commit/abort until the next start; flag removal is not destruction.
  Do not introduce earlier per-item destruction merely to resemble Rust Drop.
- Preserve private-insert cleanup, relocation/read-observation transfer,
  same-key composition, counts, physical deletion, and SV/MV retirement.
- Preserve wire/storage layouts, canonical-write/replication encodings,
  timestamp domains, lock ordering, memory orders, fences, and spin policy.
- Preserve native/RCU worker registration and never-reused IDs. No automatic
  TLS destructor or earlier reclamation for deliberately process-lifetime state.
- Avoid new per-transaction/item allocation, payload copies, refcounts,
  virtual calls, fences, or lock contention solely to satisfy the checker.
  Existing necessary scratch copies and bounded storage remain intentional.

## 4. Implementation phases

### Phase 0 — Establish an honest checker gate

1. Build the checker from the current approved, recorded source with matching
   libclang/compiler/resource headers/libc++ and production BMI artifacts.
   Rebuild it when relevant source/configuration changes; do not trust a stale
   executable merely because its path exists.
2. Preserve the real compile command, including shell quoting, forced includes,
   defines, target/system-header options, response files and modules. Missing
   entries, missing inputs, parse errors and recovery that discards modules
   must fail the gate rather than produce a partial-AST success.
3. Record the actual selected bodies and skip reasons. Cover user headers and
   relevant template instantiations; explicitly account for constructor,
   destructor, operator and callback analysis limitations.
4. Add matched valid/invalid ownership controls in a TU, included user header,
   and template body. A negative must fail for its intended borrow/move error,
   not an unrelated parse failure. Include malformed-source/missing-BMI
   controls and unsafe/bridge coverage-loss controls.
5. Register real STO translation units, starting with `Transaction.cc` and the
   live `mako_local_abi.cc` inclusion context, plus the first ownership slice.
   Order their module dependencies correctly without a build/check cycle.

Use existing checker tests and production CMake patterns where they are sound;
fix generic checker defects upstream with regressions and a clean reachable
pin. Do not locally patch a binary, suppress diagnostics, or enable all of Mako
at once to manufacture a nominal all-green result.

**Exit:** a reproducible, nonempty, fail-closed safety gate whose checked scope
is explicit. This gate can initially report real outstanding violations;
it must not falsely call excluded code safe.

### Phase 1 — Inventory owners, borrows, and native proof obligations

- Trace each relevant field/value to its owner, borrowers, last valid use,
  mutation authority, and cleanup/retirement event. Include exceptional paths.
- Inventory all callers before changing exported item/proxy/packer interfaces.
  Distinguish live storage consumers from reference datatypes and dormant code.
- Establish a bounded native-kernel register: arena layout/destruction,
  pointer tagging, atomic_ref version access, Masstree cursor surgery,
  concurrent payload copying, RCU retirement, and ABI byte views.
- Record baseline sizes/strides, address stability, allocations/copies, and
  actual supported configuration profiles. Reuse existing behavior suites.

**Exit:** each in-scope value has one ownership story and each unsafe boundary
has a concrete proof obligation; unknown lifetime assumptions remain visible.

### Phase 2 — First real ownership slice: TransactionBuffer

Target `Packer.hh`, `Packer.cc`, and unavoidable ownership declarations in
`Transaction.hh`.

- Make arena and transaction ownership explicitly noncopyable/nonmovable unless
  a needed move is separately proved. Transaction contains aliases to its own
  embedded storage, so a default move is not a safe ownership conversion.
- Encapsulate chunk allocation, construction records, traversal and destruction.
  Establish that clear visits only successfully constructed objects.
- Audit the current reserve-before-construction sequence under constructor
  failure; prove supported alignment, payload object lifetime, UniqueKey
  addressing/type discrimination, and exactly-once destruction.
- Preserve geometric growth, retained chunk reuse, stable addresses, interning,
  exception categories, and next-start destruction timing. No Box per payload.

These are source-identified proof obligations, not claims that an application
failure has been reproduced. Keep any demonstrated correctness repair separate
from mechanical ownership reshaping so its behavior and regression are clear.

**Exit:** the arena's safe surface is checked, its small raw kernel is audited,
and actual clear/destruction/throw/alignment/reuse scenarios pass in Docker.

### Phase 3 — Typed packed state and scoped item access

- Hide owner/flags, row-versus-node keys, inline words, and arena references
  behind allocation-free types/accessors with explicit validity contracts.
  Keep packed layout, equality/hash/order, and physical item identity intact.
- Prefer value reads for inline words where a mutable reference is not needed;
  isolate any remaining type punning and tagged-pointer decoding.
- Narrow TransProxy access to an operation scope and the same active attempt.
  Two freely copyable wrappers around the same mutable item are not exclusive
  Rust-style borrows. Prevent reference escape and access after reset/abort.
- Use stable handles and reacquire a short borrow after relocation/rebinding.
  Do not add repeated lookup scans or replace stable chunks with a moving vector.
- Give optional item access an explicit transaction witness rather than
  rediscovering a possibly different transaction from TLS. Migrate all affected
  predicate/vector/reference consumers, not just MassTrans.

**Exit:** typed item operations are checked at real caller sites; the only
untyped decoding is inside documented kernels, and reuse/relocation cannot
leave an accepted safe API with stale or overlapping mutable access.

### Phase 4 — Make staged-value and callback borrows explicit

- Separate transaction-owned keys/values, borrowed immutable input, private
  inserted rows, published epoch-protected rows, and callback scratch views.
- Narrow borrowed-string constness at the appropriate adapter boundary. Do not
  globally make every string packer const: reference datatypes can legitimately
  move from their owned write payloads.
- Prove the owner remains stable through the actual last consumer for each
  caller family: native mbta, the local ABI, RPC participants, and replay.
- Specifically exercise multiple existing-key participant writes with distinct
  values, reused request scratch, and delayed INSTALL. Source inspection shows
  reused `obj_v` in the server batch loop and a deferred borrowed update path;
  the supported staging lifetime must be proved before it is labeled safe.
- Preserve local-ABI fixed stable storage and quarantine after uncertain cleanup,
  synchronous replay ordering, and required scan-callback scratch isolation.
  Do not solve every borrow with an extra string copy or per-row Arc.

**Exit:** every staged value and exported byte view has a proved lifetime and
mutation contract, including deferred participant installation and failure.

### Phase 5 — Clarify terminal, worker, and epoch ownership

- Make active/local/participant/installed/terminal obligations explicit using
  existing state and narrow noncopyable guards or capabilities where useful.
  Preserve public outcomes and exceptions rather than inventing a new API.
- Distinguish ordinary abort from cleanup after irreversible INSTALL. A generic
  RAII destructor must not roll back a published participant or repeat uncertain
  cleanup. Preserve the sticky cleanup-in-progress witness and quarantine.
- Keep native abort before shard-client destruction, resolve prior participant
  work before reset, and preserve empty participant RCU start/end behavior.
- Model STO epoch publication/quiescence and Masstree RCU entry/exit separately.
  Keep worker-owned retirement queues single-owner; do not turn them into shared
  queues or replace epoch-protected rows with per-access reference counting.

**Exit:** commit, abort, drop, worker exit, participant reset, and exceptional
cleanup retain their distinct ordering and exactly-once responsibilities.

### Phase 6 — Finish narrow kernels and ownership-equivalent rusty migrations

Keep native surgery small and explicit:

- arena/flexible-allocation object lifetime and destruction;
- owner/node tag encode/decode and inline-word representation;
- aligned atomic_ref access and exact synchronization;
- cursor insert/replace/remove and retirement ownership transfer;
- SV atomic-byte and MV immutable-head snapshot copying;
- epoch-qualified callback execution and synchronous ABI/log byte views.

Move surrounding ordinary control flow into checked functions. Do not hide a
whole commit loop or entire MassTrans class behind one unsafe annotation.

Use existing rusty types when ownership really matches:

| Candidate | Required condition |
| --- | --- |
| Unique allocation -> `rusty::Box` | Correct deallocator, actual sole ownership, and unchanged lifetime; custom arena/RCU allocation is not automatically Box-compatible |
| Shared lifetime -> `rusty::Arc` | Sharing is required; no added per-row/per-access reference counts |
| Optional value -> `rusty::Option` | Preserve absence/state meaning and hot representation |
| Owned reusable sequence -> `rusty::Vec` | Migrate its boundary without bridge copies; preserve reserve/reuse and required address stability |
| Worker callbacks -> `rusty::Function` | Move-only semantics match actual assignment/invocation; no added hot allocation |
| Mutex/thread owner -> rusty equivalent | Preserve synchronization, callback replacement, and explicit joining |

Do not replace shared atomics or mutexes with Cell/RefCell: the current
`rusty::Cell` explicitly is not thread-safe. Do not heap-allocate fixed item,
write-set, or stable-string arrays merely to use Vec. The epoch advancer copies
its callback under a mutex and invokes outside it; move-only Function is not a
spelling replacement. The current advancer joins at shutdown; a rusty thread
handle that detaches on destruction must be explicitly joined.

**Exit:** no obsolete ownership mechanism remains in the changed boundary;
remaining std/native types have a real boundary or semantic reason, not an
unexamined blanket exemption.

### Phase 7 — Close coverage and verify conversion readiness

- Expand checked safe logic monotonically. At completion, no in-scope body is
  silently unannotated, lost to parsing, or classified safe solely through an
  unsafe/bridge annotation. Register native kernels with their concrete proofs.
- Preserve checker negative controls across header, template, configuration,
  compiler and module changes. A named aggregate target is not coverage proof.
- Run existing STO/MakoLocal, scan/relocation, participant/replay, hook/quarantine,
  epoch, sanitizer and concurrency scenarios through Docker. Extend existing
  tests for newly identified ownership boundaries; avoid annotation-text tests.
- Run the actual affected storage paths, not only reference TBox tests. Cover
  capacity boundaries (512, 32768, exhaustion), destruction and constructor
  failure, non-SSO strings, buffer reuse, row growth, phantoms and publication.
- Measure allocations, copied bytes, item stride/footprint, version operations,
  callback cost and throughput under matched builds/workloads. A Rusty type
  spelling is not evidence of zero overhead. Do not reopen the stopped benchmark
  campaign or change defaults/services as a side effect of this project.
- Review how each checked boundary would be expressed as ordinary Rust:
  owned arena/frame, borrowed item view, typed key/payload, explicit state,
  epoch witness, and a narrow unsafe/native kernel. Unsupported language/compiler
  features remain separate work; do not create a third handwritten STO engine.

**Exit:** production C++ STO is demonstrably checked within a declared scope,
its native trust boundary is explicit and validated, supported behavior/cost is
preserved, and a later Rust conversion can proceed one real boundary at a time.
Only then revisit canonical-source unification with the existing native Rust STO.

## 5. Deferred first implementation batch (not current sequencing)

1. Establish the strict checker/configuration/coverage controls and explicit STO
   gate from Phase 0; complete the ownership inventory for the first slice.
2. Refactor the TransactionBuffer ownership/construction boundary from Phase 2,
   with focused behavior tests and narrow audited raw operations.
3. Extend that proved slice into packed items and scoped proxies, rather than
   running a repository-wide STL substitution or annotation sweep.

Do not begin with whole-core transpilation, datatype renaming, a new lock
protocol, automatic cleanup after uncertain publication, or blanket `@unsafe`.
The first milestone is a real, non-vacuous checked C++ ownership slice—not an
annotation-only green build and not a new Rust implementation.

## Appendix: executed checker baseline — 2026-10-16

Built and exercised the then-current checker in Docker on `zoo-002`, with the
worktree mounted read-only. The clean checker source was
`20707e98a371fee772729ae0a6b516e2d360ddd7`; the fresh frozen Cargo build succeeded.
The binary reported `rusty-cpp-checker 0.1.1` and SHA-256
`435956520ec075dc13e72e1f112bc91fb9ea5f9bad7abfb0201a4c9e9b9e603b`.

| Executed input | Observed result | Meaning for this plan |
| --- | --- | --- |
| Safe function with two live mutable aliases | Exit 1, conflicting-mutable-borrow diagnostics | This particular negative control was detected |
| Same aliasing body marked unsafe | Exit 0, no violations | Unsafe annotation removes this check; not a repair |
| Malformed safe function | Exit 0, no violations; parser errors printed as suppressed warnings | Strict parse failure is a required gate fix |
| Valid safe function borrowing a local and returning an `int` by value | Exit 1, incorrectly reported returning a reference to a local | Matched valid controls must catch checker false positives as well as missed violations |
| `Packer.cc`, `thread_registration.cc`, `Transaction.cc` with the retained q11 compile database | Each exited 1 with missing response-file warnings and missing standard-header fatal errors | This retained build context is insufficient for a production safety baseline; no STO ownership verdict follows |

The last three attempts used the existing database without rewriting it. Its
response-file paths reference the absent `native/canonical` build context.
Restore a coherent build/configuration in Phase 0 rather than interpreting
these input-resolution failures as source-code borrow violations.

Receipts, exact commands, control sources, binary/source identities and
stdout/stderr are retained outside the repository at:

`/home/users/shuai/mako-cpp-sto-safety-investigation-20261016/`

These probes establish concrete checker limitations. They do not qualify
production STO, demonstrate complete analyzer coverage, or replace the future
behavior/concurrency/performance validation described above.
