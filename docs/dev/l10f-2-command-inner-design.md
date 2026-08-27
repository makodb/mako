# L10f-2 part 2 — `Command::inner_` storage redesign

This is the deferred part of L10f-2: flipping `Command`'s internal
storage so it no longer requires `Marshallable`.

## What is `Command::inner_` today?

`Command` is `SerializableEnvelope<MakoCommands>` — a closed-set
polymorphic envelope.  It carries one of ~19 payload types listed in
the `MakoCommands` `TypeList`:

```
LogEntry, TpcPrepareCommand, TpcCommitCommand, VecPieceData,
BulkPaxosCmd, BulkPrepareLog, HeartBeatLog, SyncLogRequest,
SyncLogResponse, SyncNoOpRequest, PaxosPrepCmd, TpcEmptyCommand,
TpcNoopCommand, TpcBatchCommand, VecRecData, ViewData,
SimpleRWCommand, KeyCmdBatchData, ReplicatedDBCommand
```

Storage shape:

```cpp
template<typename TypeList>
class SerializableEnvelope {
  int32_t kind_{0};                       // public, mirrors inner_->kind()
  std::shared_ptr<Marshallable> inner_;  // private storage
};
```

In practice, `inner_` always points to a `SerializableMarshallableAdapter`
— a bridge object that wraps a `pro::proxy<SerializableFacade>` (the
proxy library facade over the closed-set payload types) and exposes
it as a `Marshallable` so the field type still compiles.  The
adapter's `to_marshal/from_marshal/kind` methods all delegate to
the proxy.

So the real shape today is:

```
Command::inner_ : shared_ptr<Marshallable>
  └── SerializableMarshallableAdapter (just a Marshallable wrapper)
       └── pro::proxy<SerializableFacade>
            └── actual T (TpcCommitCommand / VecPieceData / ...)
```

The Marshallable layer is **always** redundant — it just exists so
`inner_` has a non-Marshallable-free type.

## Roles `inner_` plays internally

Looking at uses inside `serializable_envelope.hpp`:

| Site | Role | Marshallable-specific? |
|------|------|------------------------|
| Constructors | Set storage | Storage type only |
| `inner_->kind()` | Get type tag | No (proxy can do it) |
| `inner_->to_marshal(m)` | Marshal& path | Yes (Marshallable virtual) |
| `inner_->from_marshal(m)` | Marshal& path | Yes |
| `dynamic_cast<SerializableMarshallableAdapter*>(inner_.get())` | Adapter fast path | Bridge-specific |
| `serializable_cast<T>(inner_)` | Unpack | Already works on adapter |
| `pack/unpack/unpack_shared` | Typed access | Already through proxy |

The Marshallable-specific paths are:
1. The `to_marshal/from_marshal` virtuals (used by Marshal&
   operators added in L10f-5 part 1).
2. The `dynamic_cast` fast path that detects the adapter.

## External consumers of `inner_marshallable()`

| Site | Use | Replacement |
|------|-----|-------------|
| `service.cc:500` | `dynamic_cast<TxData*>` escape hatch | TxData not in TypeList; needs separate per-tx-id lookup |
| `classic/scheduler.cc:119` | Identity check (shared_ptr equality) | Compare proxy pointers, or add `Command::operator==` |
| `raft/server.cc:1611` | `%p` debug print | Print proxy pointer |
| `RW_command.h` (8 inline forwarders) | Legacy shared_ptr<Marshallable> static method overloads | Drop overloads — all callers can use Command directly |

## Design options

### Option A: `pro::proxy<SerializableFacade>` directly

```cpp
class SerializableEnvelope {
  int32_t kind_{0};
  std::shared_ptr<pro::proxy<SerializableFacade>> inner_;
};
```

**Pros:**
- Drops `Marshallable` dependency entirely from Command.
- No bridge layer (`SerializableMarshallableAdapter` goes away).
- `kind()`, `save()`, `load()` all dispatch through the proxy facade
  natively.
- Wire format unchanged.

**Cons:**
- Open-set types (`AnyMessage`) don't fit — they're declared as
  Marshallable subclasses, not Serializable.  But Command has never
  been used for AnyMessage — that's a separate envelope.  Not a
  blocker.
- The legacy `Marshal&` operators on Command (added in L10f-5 part 1)
  currently call `inner_->to_marshal(m)`.  Need to either:
  (a) Add a Marshal& operation to the proxy facade, or
  (b) Drain the BinaryWriteArchive/BinaryReadArchive bytes through a
      temp buffer.
- `inner_marshallable()` accessor goes away.  Callers must migrate.

### Option B: Variant-style storage

```cpp
class SerializableEnvelope {
  int32_t kind_{0};
  std::variant<std::shared_ptr<LogEntry>, std::shared_ptr<TpcCommitCommand>, ...> inner_;
};
```

**Pros:**
- Direct typed access (no proxy indirection).
- Compile-time exhaustive dispatch.

**Cons:**
- Requires every TypeList type to be complete at Command's
  declaration site.  This is exactly the forward-decl problem L10c
  solved by going to a runtime registry.  Re-introducing the
  compile-time requirement is a regression.
- Can't easily support `unpack_shared<T>()` aliasing semantics
  (`shared_ptr<T>` extending `inner_`'s control block).
- 19+ alternatives in the variant — large compile-time overhead.

### Option C: Keep `shared_ptr<Marshallable>` but shrink Marshallable

Make `Marshallable` a minimal interface — just `kind()`, `save()`,
`load()` — and accept that it still exists as a name.

**Pros:**
- Minimal code churn.
- Compatible with existing infrastructure.

**Cons:**
- Doesn't actually "retire" Marshallable as the user requested.
- Leaves a vestigial base class around that's only used for type
  erasure inside Command.

### Option D: Hand-rolled type erasure

```cpp
struct CommandStorage {
  std::shared_ptr<void> ptr;
  int32_t kind;
  void (*save)(const void*, BinaryWriteArchive&);
  void (*load_into)(void*, BinaryReadArchive&);
};
```

**Pros:**
- No Marshallable, no proxy library dependency.
- Minimal struct.

**Cons:**
- Reinvents what the proxy library already provides.
- Loses proxy-level type safety.

## Recommendation: Option A

**Direct proxy storage** is the cleanest end state and aligns with
the proxy-based architecture L10c established.  It removes the
adapter layer that's always redundant in practice.

### Implementation sketch

#### Step 1: Audit external `inner_marshallable()` consumers

The 8 inline forwarders in `RW_command.h` go away as part of
L10f-3 (drop legacy shared_ptr overloads).  Already mostly done —
the static methods + ctor are flipped; just need to delete the
shared_ptr overloads after migrating their callers (mongodb is
excluded from build, so deleting them only requires migrating the
2 sites in scheduler.cc that pass `wrap_typed_marshallable(c)`).

For the other 4 consumers:
* **service.cc:500** (dynamic_cast<TxData*>): add a separate
  `tx_id → shared_ptr<TxData>` map on the scheduler and key the
  view-data lookup off that.
* **classic/scheduler.cc:119** (identity check): add
  `bool Command::operator==(const Command&)` that compares proxy
  pointers; replace `tx->cmd_.inner_marshallable() != cmd_env.inner_marshallable()`
  with `tx->cmd_ != cmd_env`.
* **raft/server.cc:1611** (%p print): print the proxy pointer
  address.  The proxy library probably has a way to access the
  underlying pointer; if not, just remove the %p from the debug log.
* **WitnessLog**: already migrated (uses Command directly now).

#### Step 2: Add a `Marshal&` path to the proxy facade

The legacy `Marshal&` operators on Command (procedure.cc TxReply,
tpc_command.cc TpcCommitCommand) need a way to drive serialization
from the proxy without going through Marshallable.

Options:
* (a) Add `to_marshal(Marshal&) const` / `from_marshal(Marshal&)`
  to `SerializableFacade`.  Each Serializable type implements them
  by default via temp buffer through BinaryWriteArchive.
* (b) Drop the `Marshal&` operators on Command and migrate the 3
  call sites to `BinaryWriteArchive&` instead.  Requires changing
  the legacy reply path (`fu->get_reply() >> reply`) to use a
  BinaryReadArchive bridge — non-trivial since the srpc framework
  exposes Marshal& at that boundary.

(a) is less invasive.

#### Step 3: Flip `Command::inner_` storage

```cpp
class SerializableEnvelope {
  int32_t kind_{0};
  std::shared_ptr<pro::proxy<SerializableFacade>> inner_;
};
```

Update internal methods:
* Constructors: instead of accepting `shared_ptr<Marshallable>`,
  accept `pro::proxy<SerializableFacade>` directly (or
  `shared_ptr<T>` for non-Marshallable T).
* `kind()`: `return inner_ ? inner_->kind() : 0`.
* `save()`: drive via proxy directly (already does this for
  the adapter fast path; just remove the dynamic_cast).
* `load()`: dispatch via the runtime registry, then return the
  proxy directly.
* `unpack<T>()` / `unpack_shared<T>()`: route through
  `serializable_cast` on the proxy.

Drop:
* `inner_marshallable()` accessor.
* `set_marshallable(shared_ptr<Marshallable>)` — replaced by
  proxy-taking equivalents.
* `SerializableMarshallableAdapter` (bridge wrapper) — no longer
  needed.

#### Step 4: Migrate callers to non-Marshallable APIs

* `wrap_typed_marshallable<T>(shared_ptr<T>)` returns
  `shared_ptr<Marshallable>`; replace with `make_command<T>(...)` or
  use `Command{shared_ptr<T>}` with the templated ctor.
* `marshallable_cast<T>(shared_ptr<Marshallable>)` for legacy
  callers — drop, force migration to `marshallable_cast<T>(Command&)`.

#### Step 5: Drop bridge helpers + Marshallable

Once nothing references `SerializableMarshallableAdapter` or
`shared_ptr<Marshallable>`-taking helpers, delete:
* `marshal_serializable_bridge.hpp` (or shrink to just the
  Serializable proxy machinery).
* `Marshallable` class definition (if AnyMessage is migrated or
  kept as a separate path).
* The `MarshallDeputy` class (already nearly retired in L10f-5
  parts 1-3).

## Estimated scope

| Step | LOC | Risk |
|------|-----|------|
| 1. Audit + migrate 4 inner_marshallable consumers | ~50 | Low (small, isolated changes) |
| 2. Add Marshal& path to facade | ~30 | Low (additive) |
| 3. Flip Command::inner_ storage | ~80 | Medium (touches every Command method) |
| 4. Migrate wrap_typed_marshallable / marshallable_cast | ~100 | Medium (many callers) |
| 5. Drop bridge + Marshallable | ~400 (net negative) | Low (delete-only) |

Total: ~660 LOC churned, ~400+ LOC removed net.

## Wire-format invariants

Across every step, encoded bytes must stay byte-for-byte identical
to today's `[v32 kind] [payload bytes]` shape.  Validation:
* `test_rpc_marshal_archive` — primitive + container byte-compat.
* `test_rpc_marshallable_proxy` — closed-set polymorphic byte-compat
  (currently uses `MarshallDeputy`/`Marshallable`-based fixtures;
  needs migration but coverage carries through).
* 101-test integration suite — smoke coverage of wire-format-
  sensitive paths.

## Recommended starting point

**Step 1 + Step 2 first**: migrate the 4 `inner_marshallable()`
consumers, then add the Marshal& path to the proxy facade.  These
are independent and reversible — they unblock the storage flip
without committing to it.

Then **Step 3** (storage flip) becomes a single, focused leaf —
all dependents have been moved off the legacy interface.

## Update 2026-05-04: Step 3 blocker — test fixtures

Attempted Step 3 (storage flip) and reverted.  Two failures:

* `test_rpc_marshallable_proxy` — uses `TestMarshallable` /
  `CanaryMarshallable` test fixtures that inherit `Marshallable`
  directly (not through the bridge adapter).  Constructing a Command
  from `shared_ptr<TestMarshallable>` hits the `verify(adapter !=
  nullptr)` in the new `set_from_marshallable` because there's no
  SerializableMarshallableAdapter wrapper.

* `test_rpc_log_storage` — uses `TestCommand` directly, same
  pattern.

**The dependency was hidden in the original plan.**  Test fixtures
are also Marshallable consumers, and they need to migrate (or be
wrapped via a synthesized adapter inside Command's ctors) before
Step 3 can land.

**Revised step ordering:**

* **Step 2.5** (new): migrate test fixtures
  (`TestMarshallable`, `CanaryMarshallable`, `TestCommand`,
  `AnyMessage`) off Marshallable, OR build a `MarshallableSerializableAdapter`
  (the reverse-direction bridge that was retired in L10d-prep) so
  Command can wrap arbitrary Marshallables in a Serializable
  proxy.

  This is a multi-day effort because:
  - Each fixture has its own `to_marshal/from_marshal` body that
    needs to be ported to `save/load` for the proxy facade.
  - `AnyMessage` is the open-set polymorphic envelope — its semantics
    differ from the closed-set TypeList path; it might need its own
    parallel envelope type.
  - Several test files build on the legacy fixtures.

* **Step 3** (storage flip): only after Step 2.5.

**Status pause:** Steps 1 and 2 landed in this session; Step 2.5
(test fixture migration) is the new blocker for Step 3.  The
remaining production code is fully migrated for the parts not
gated on Marshallable inheritance — Marshallable lives on inside
the test fixture realm and the bridge adapter, both of which need
their own L10f follow-ups.

## Update 2026-05-05: Step 4 landed (production helper migration)

Step 4 (`wrap_typed_marshallable` / `marshallable_cast` callers)
landed independently of Step 3 — it doesn't require the storage
flip, just an additive change to `Command`'s API.  Two commits:

* `6a125649f` — add `Command(shared_ptr<T>)` templated ctor +
  `operator=(shared_ptr<T>)` for non-Marshallable T.  The matching
  exact-template `operator=` outranks the previously-ambiguous
  pair (shared_ptr<T> → Command via the templated ctor; or
  shared_ptr<T> → MarshallDeputy → Command via the implicit
  conversion chain).

* `5ab0305ee` — drop `wrap_typed_marshallable(sp_T)` at all 16
  production call sites.  Includes flipping
  `SchedulerClassic::CheckCommitted(Marshallable&)` to
  `(const Command&)` since the only caller (copilot/server.cc:92)
  was wrapping a TpcCommitCommand into a Marshallable just to
  deref it.

Wire bytes unchanged.  Test results:
* test_rpc_marshal_archive (88 tests) ✓
* test_rpc_marshallable_proxy (21 tests) ✓
* test_rpc_log_storage ✓
* simpleTransaction ✓
* shard1Replication ✓ (sequential — parallel runs hit dbtest race)

**Step 2.5 re-assessed:** the "multi-day effort" assumption needs
revisiting.  Looking at the actual fixture usage:

* `rpc_marshallable_proxy_test.cc`: 7 tests testing pure
  MarshallDeputy/Marshallable infrastructure — these go away with
  the underlying API at Step 5, not earlier.  3 tests use
  TestMarshallable as a generic nested Serializable inside
  KeyCmdBatchData / SyncLogResponse / BulkPaxosCmd; could be
  retargeted to a real MakoCommands type (e.g. TpcEmptyCommand or
  HeartBeatLog).
* `rpc_log_storage_test.cc`: 2 tests use TestCommand as the
  LogEntry's command field; could swap to TpcEmptyCommand.
* `rpc_marshal_archive_test.cc`: 1 test uses CanaryMarshallable
  to verify `serializable_cast` returns nullptr on a non-bridge
  Marshallable — goes away with Marshallable at Step 5.
* `any_message.hpp`: production code that inherits Marshallable;
  the most substantial migration target.

## Update 2026-05-05 (later): Steps 2.5 + 5-prep landed

* `9779f5e7c` — migrated 5 fixture-using tests
  (rpc_marshallable_proxy_test.cc nested-payload tests +
  rpc_log_storage_test.cc LogEntry tests) to use real MakoCommands
  types (HeartBeatLog / TpcEmptyCommand) instead of the legacy
  fixtures.

* `e62cb7954` — deleted the 8 tests in rpc_marshallable_proxy_test.cc
  that exercised pure MarshallDeputy / Marshallable infrastructure
  (the 3 fixtures `TestMarshallable`, `CanaryMarshallable`, the 1 test
  using `CanaryMarshallable`) — they retire alongside the API.

* `fc84612c5` — migrated `AnyMessage` off Marshallable inheritance.
  The class no longer inherits Marshallable, dropped `to_marshal` /
  `from_marshal` / `try_cast(MarshallDeputy&)` and the
  `MarshallDeputy::reg_initializer<AnyMessage>(ANY_MESSAGE)`
  static-init line.  Test code (rpc_any_message_test.cc and
  rpc_marshallable_proxy_test.cc) rewritten to use the direct
  archive round-trip path that production already uses.

## Update 2026-05-05 (later still): Production .inner() callers retired

Two follow-up commits cleared the production `.inner()` consumer
list entirely:

* `65b073da0` — migrated 9 `.inner() != nullptr` /  `!.inner()` null
  checks to `.has_value()` (communicator.cc x4, none_copilot/commo.cc
  x2, rule/commo.cc, raft/raft_worker.cc, raft/test.cc).

* `7ddd5fcff` — migrated 27 typed-cast / pass-through sites:
  * `classic/scheduler.cc` (2 sites): `serializable_cast<T>(md.inner())`
    → `md.unpack<T>()`.
  * `communicator.cc` + `service.cc` (1 site each):
    `GetCmdID(md.inner())` → `GetCmdID(md)`.
  * `paxos_worker.cc`, `raft/server.cc`, `raft/test.cc` x21
    (ApplyEntry/ForwardToLearner): `(md.inner())` → `(md)` via the
    implicit `Command(shared_ptr<Marshallable>)` ctor.

`git grep '.inner()' src/deptran` returns zero hits in non-test
production code now.  `.inner_marshallable()` is also empty (only
mongodb sites remain — excluded from build).

## Update 2026-05-05 (still later): Test-side migration done

* `6287d91bd` — migrated rpc_marshallable_proxy_test round-trips
  off MarshallDeputy (helper rewritten to use Command, 5 tests
  rewritten, 1 bridge-mechanics test deleted).  12 tests remain,
  all passing; net -32 LOC.

* `c3c990049` — bulk-deleted 670 LOC of bridge / MarshallDeputy
  tests in rpc_marshal_archive_test.cc (17 tests + 4 helper
  structs + 2 static reg lines covering
  `SerializableMarshallableAdapter`, `MarshallDeputy::reg_initializer`,
  `MarshallDeputyArchiveOps`, `MarshallDeputyKindEncoding`,
  `WrapSerializableAliased`, `MarshallableCastSerializable`, etc.).
  68 tests remain (was 87).

## Remaining work (handoff, post-2026-05-05)

The end-state retirement requires a coordinated change to:

1. **Move the runtime kind→factory registry off `MarshallDeputy`**.
   `SerializableEnvelope::load` calls
   `MarshallDeputy::create_initializer(kind_v.get())`, which is
   populated at static-init by `reg_serializable_in_deputy<T>(kind)`.
   The registry concept is independent of MarshallDeputy — only
   the namespace ties it.  Extract to a free
   `srpc::SerializableProxyRegistry` (or rename the existing class
   without the `MarshallDeputy::` prefix) and update all
   registration / lookup sites.

2. **Storage flip** (`Command::inner_` and `AnyMessage::payload_`
   from `shared_ptr<Marshallable>` to `shared_ptr<SerializableProxy>`).
   Once the registry no longer hands out `shared_ptr<Marshallable>`,
   the bridge `SerializableMarshallableAdapter` becomes unused.
   `Command::inner_` callers in production are zero
   (`.inner()`/`.inner_marshallable()` retired earlier this session),
   so this is purely an internal rewrite of `serializable_envelope.hpp`
   and `any_message.{hpp,cpp}`.

3. **Drop bridge + helpers**.  `SerializableMarshallableAdapter`,
   `wrap_typed_marshallable`, `wrap_serializable`,
   `wrap_serializable_aliased`, `as_marshallable`,
   `make_serializable_proxy`,
   `marshallable_cast<T>(shared_ptr<Marshallable>)`,
   `serializable_cast<T>(shared_ptr<Marshallable>)`,
   `reg_serializable_in_deputy`.  All in
   `marshal_serializable_bridge.hpp`.

4. **Drop `MarshallDeputy`** class from `marshal.hpp`.  Production
   has zero runtime declarations; only the
   `serializable_envelope.hpp` `SerializableEnvelope(MarshallDeputy&)`
   migration-compat ctor and `marshal.hpp`'s reg_initializer registry
   remain, both retired by step 1.

5. **Drop `Marshallable`** abstract base.  Last user retires in
   step 3.

Each step is bounded but requires touching the
`serializable_envelope.hpp` / `marshal_serializable_bridge.hpp` /
`marshal.hpp` / `any_message.hpp` files together since they form a
tightly-coupled cluster.  Estimated 4-6 hours of careful migration
work; recommend doing it in a fresh session with explicit decisions
on the new `SerializableProxy` storage shape (shared_ptr<proxy>?
proxy directly with support_copy?  variant?) before code changes
start.

### Original step 1 listing (deferred — predates 2026-05-05 progress)

The largest remaining items are interconnected and substantial:

1. **Storage flip** (`Command::inner_` from `shared_ptr<Marshallable>`
   to `shared_ptr<pro::proxy<SerializableFacade>>`).  Production has
   zero `.inner()` / `.inner_marshallable()` consumers, so the flip
   is now structurally unblocked at the call-site level — the
   remaining work is internal: `serializable_envelope.hpp` itself
   (ctors, save/load fast path, the `inner()`/`inner_marshallable()`
   accessors that need to be either dropped or repointed).

2. **Test-side MarshallDeputy migration**.  ~13 tests in
   rpc_marshallable_proxy_test.cc + ~6 tests in
   rpc_marshal_archive_test.cc use `MarshallDeputy` as a wire-
   round-trip envelope.  Each can be rewritten using `Command` (whose
   Marshal& archive operators were added in L10f-2 step 2 and emit
   identical wire bytes for closed-set types).

3. **Drop bridge** (`SerializableMarshallableAdapter`,
   `wrap_typed_marshallable`, `marshallable_cast<T>(shared_ptr<
   Marshallable>)`, `as_marshallable`, `make_serializable_proxy`,
   `wrap_serializable`, `wrap_serializable_aliased`) from
   `marshal_serializable_bridge.hpp`.  Depends on (1) and (2).

4. **Drop `MarshallDeputy`** class.  Production has zero runtime
   declarations; only test code and a few source-level aliases
   (`using srpc::MarshallDeputy;`) remain.  Depends on (2).

5. **Drop `Marshallable`** abstract base.  The final retirement.

Each step is bounded but cumulatively several hours of careful
work.  Wire bytes stay invariant across all steps.

**Revised path:** Step 5 (drop Marshallable) is the natural next
single-step retirement once Step 2.5's narrow scope (3-4 tests
retargeted, plus AnyMessage migration) is done.  Step 3 (storage
flip) folds into Step 5 rather than being a separate phase.

---

## 2026-05-05 — L10f-2 step 5 lands; Marshallable + MarshallDeputy retired

Final session of the long-running L10 plan.  Six commits this session
land the entire residual scope:

1. **Storage flip** (208b76cf2): `SerializableEnvelope::inner_`
   migrated from `shared_ptr<Marshallable>` to
   `pro::proxy<SerializableFacade>` (value member; no shared_ptr
   layer).  Lifetime is preserved by holder-shaped proxies wrapping
   `shared_ptr<T>` so `unpack_shared<T>` returns the original
   refcount.  Wire format unchanged.
2. **AnyMessage payload migration** (9bd9fcde2):
   `AnyMessage::payload_` flipped from `shared_ptr<Marshallable>` to
   `pro::proxy<SerializableFacade>`, registry returns proxy directly.
3. **Bulk reg_serializable_in_deputy → SerializableRegistry::reg
   migration** (4a48bb732): all production registration sites
   migrated to the no-arg `SerializableRegistry::reg<T>()` (kind
   derived from TypeList).
4. **Bridge dropped** (cdcb8abee): `marshal_serializable_bridge.hpp`
   deleted (528 LOC).  No production code referenced it.
5. **Marshallable + MarshallDeputy retired** (this commit):
   Marshallable abstract base, MarshallDeputy concrete envelope, and
   the SpinMutex<MarContainer> registry are gone.  Raft's
   `AppendEntriesReq.cmd` field flipped to `::janus::Command`.
   Test-side `static_assert(!is_base_of_v<Marshallable, ...>)`
   guards retired (no longer expressible).

**Test verification** (all four affected tests pass after rebuild):
- test_rpc_marshal_archive: 68 tests
- test_rpc_any_message: 8 tests
- test_rpc_log_storage: 35 tests
- test_rpc_marshallable_proxy: 12 tests

**Outstanding scope:** none for L10f-2.  Wire format remained
byte-for-byte identical throughout (the `[v32 kind][payload]` shape
locked in by L9 alignment).  The `marshal.hpp` and `marshal.cpp`
files are now ~500 LOC lighter; `__dep__.h`, `raft/log_storage.hpp`,
and `raft/messages.hpp` no longer reference Marshallable or
MarshallDeputy at all.

The L10 series is complete.  Production code paths use
`janus::Command` (closed-set, kind-tagged) for replicated commands
and `srpc::AnyMessage` (open-set, name-tagged) for graph-shaped
RPC payloads.  Both serialize through
`pro::proxy<SerializableFacade>` value members; no
`shared_ptr<Marshallable>` storage anywhere.
