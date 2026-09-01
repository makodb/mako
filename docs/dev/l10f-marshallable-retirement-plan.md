# L10f / L10g — Retire `Marshallable` + `MarshallDeputy`

**Status:** L10f-1 through L10f-5 parts 1-3 landed 2026-05-04.

After prep6u..prep6bd the `shared_ptr<Marshallable>` interface
surface was already limited to (a) the `SimpleRWCommand` interface
itself, (b) the `WitnessLog` debug struct, and (c) framework
boundary points.  This session landed:

* **L10f-1**: `CmdData` no longer inherits `Marshallable`; new
  `SimpleRWCommand(const SimpleCommand&)` ctor; dead
  `MarshallDeputy::CONTAINER_CMD` enum value removed.
* **L10f-2 part 1**: `SimpleRWCommand(const Command&)` is now the
  primary ctor; legacy `shared_ptr<Marshallable>` overload delegates.
* **L10f-3**: `SimpleRWCommand` static methods (GetCmdID,
  GetCombinedCmdID, GetCommandMsTime, GetCommandMsTimeElaps, GetKey,
  NeedRecordConflictInOriginalPath, Conflict) flipped to make
  Command primary.  Legacy shared_ptr overloads delegate.
* **L10f-4**: `WitnessLog::cmd_` migrated from
  `shared_ptr<Marshallable>` to `janus::Command`.
* **L10f-5 part 1**: Added legacy `Marshal&` operator<<>> for
  `SerializableEnvelope` so `TxReply` and `TpcCommitCommand`
  archive operators can replace `MarshallDeputy view_md` with
  `janus::Command view_md`.  All three production users migrated.
* **L10f-5 part 2**: 5 `verify(cmd_.kind_ != MarshallDeputy::UNKNOWN)`
  assertions in Submit-style coordinators replaced with
  `verify(cmd_.has_value())`.
* **L10f-5 part 3**: Remaining `using srpc::MarshallDeputy;` aliases
  documented as bridge/test/facade-only.

**Remaining work (L10f-5 final + L10f-6):**

The MarshallDeputy class itself + the bridge helpers
(`wrap_typed_marshallable`, `marshallable_cast` for
shared_ptr<Marshallable>, `as_marshallable`, `wrap_serializable`,
`SerializableMarshallableAdapter`, `MarshallableSerializableAdapter`)
still exist.  Dropping them requires:

1. Migrating `AnyMessage` (open-set polymorphic envelope, still a
   `Marshallable` subclass) — needs a Serializable replacement, or
   keep `Marshallable` minimally for AnyMessage only.
2. Migrating test fixtures (`TestMarshallable`, `CanaryMarshallable`,
   `TestCommand` in src/srpc/tests/) — replace with Serializable-based
   fixtures, or accept that `Marshallable` survives only for tests.
3. Switching `Command::inner_` storage from `shared_ptr<Marshallable>`
   to a non-Marshallable type (e.g., `shared_ptr<SerializableProxy>`
   or direct value storage per type).  This is the L10f-2 part 2
   that was deferred.
4. Removing `Command::inner_marshallable()` accessor (and the 8
   inline header forwarders in RW_command.h).
5. Removing the bridge helpers themselves.

This is a multi-day effort with deep test-coverage requirements.

## Current state

* `Marshallable` is still the abstract base for command types. Defines
  `kind_` field, virtual `to_marshal/from_marshal` (`verify(0)`
  default — actual serialization handled elsewhere now).
* `MarshallDeputy` is the legacy wire envelope. Almost entirely
  retired in production — still used by some test fixtures and the
  `marshal_serializable_bridge.hpp` helpers.
* `Command = SerializableEnvelope<MakoCommands>` is the new
  closed-set polymorphic carrier. Stores
  `shared_ptr<Marshallable> inner_` internally to bridge with legacy
  shared_ptr-shaped APIs.
* Production command types: most inherit
  `srpc::Serializable<T, MakoCommands>` directly. Only `CmdData`
  (and its subclasses `SimpleCommand`, `TxData`) still inherit
  `srpc::Marshallable`.

## What "retire Marshallable" means concretely

1. `Marshallable` base class deleted.
2. `MarshallDeputy` deleted (or shrunk to test-only).
3. `Command::inner_marshallable()` accessor deleted; `Command`'s
   internal storage flips from `shared_ptr<Marshallable>` to
   something else (likely `shared_ptr<SerializableProxy>` or
   `shared_ptr<void>` with a kind tag).
4. `marshal_serializable_bridge.hpp` helpers deleted (`as_marshallable`,
   `wrap_serializable`, `wrap_typed_marshallable`, `as_serializable`,
   `MarshallableSerializableAdapter`).
5. `CmdData` / `SimpleCommand` / `TxData` no longer inherit
   `Marshallable` — either inherit `Serializable` or become standalone
   non-polymorphic classes.

## Phased plan

### L10f-1: Migrate `CmdData` off `Marshallable` (~150 LOC)

`CmdData` provides metadata fields + virtual methods (`Merge`,
`GetPartitionIds`, `Reset`, `inn_id()`, `type()`). None of these
relate to `Marshallable`'s `kind_`/`to_marshal`/`from_marshal`. The
inheritance is purely so `shared_ptr<CmdData>` can flow through
`shared_ptr<Marshallable>` channels.

Steps:
1. **Audit**: every site that holds `shared_ptr<CmdData>` and
   crosses into a `shared_ptr<Marshallable>` API. Document which
   APIs need `CmdData` shape and which need `Marshallable` shape.
2. **Sub-leaf 1a**: split `CmdData` into a non-polymorphic struct
   (just the data fields) and remove the inheritance. The virtual
   methods (`Merge`, `GetPartitionIds`, `Reset`, `inn_id()`,
   `type()`) move to a separate interface or get removed.
3. **Sub-leaf 1b**: `SimpleCommand` and `TxData` change inheritance.
   * `SimpleCommand` becomes `Serializable<SimpleCommand, MakoCommands>`
     (it's a wire payload — it serializes as part of `VecPieceData`).
   * `TxData` is a pure server-side container (never on the wire) —
     remove inheritance entirely; it doesn't need `Marshallable`.
4. **Sub-leaf 1c**: Adjust the dynamic_cast escape hatch in
   `service.cc:500` (`dynamic_cast<TxData*>(...)`). Either:
   * Add a non-Marshallable lookup keyed by some other identifier.
   * Add `TxData` to the `MakoCommands` TypeList (if it carries on
     the wire).
   * Use a separate map from cmd_id → `shared_ptr<TxData>`.

**Risk:** Some legacy paths assume `shared_ptr<SimpleCommand>` is
implicitly convertible to `shared_ptr<Marshallable>`. Each such
site must be audited and updated — the migration sweep we did in
prep6 already cleared most.

### L10f-2: `Command` storage swap (~80 LOC)

Currently `Command::inner_` is `shared_ptr<Marshallable>`. After
L10f-1, no command type inherits `Marshallable`, so this storage
shape becomes meaningless.

Steps:
1. **Sub-leaf 2a**: Audit all uses of `Command::inner_marshallable()`.
   After prep6, the remaining sites are:
   * `WitnessLog` ctor (debug-only, conditional)
   * `dynamic_cast<TxData*>(...)` in `service.cc:500`
   * shared_ptr identity check in `classic/scheduler.cc:119`
   * Debug `%p` print in `raft/server.cc:1611`
   * Bridge calls to `set_marshallable` in `copilot/server.cc`
   * Internal forwarders in `RW_command.h` (delegate to the
     `shared_ptr<Marshallable>` overloads of the static methods)
   Each must move off `inner_marshallable()` before storage flips.
2. **Sub-leaf 2b**: Flip `Command::inner_` to
   `shared_ptr<SerializableProxy>`. Update `kind_`, `pack/unpack`,
   `save/load`, `marshallable_cast<T>(Command&)` overload to use
   the proxy directly. Wire format unchanged (the proxy already
   round-trips the same bytes as `Marshallable::to_marshal`).

**Risk:** the proxy field exists in parallel today via
`SerializableMarshallableAdapter` / `wrap_serializable`. We're
flipping which is the primary; need to ensure the unpack path
still works for round-tripped commands.

### L10f-3: Migrate `RW_command.h` static methods + `SimpleRWCommand` ctor (~50 LOC)

After L10f-1/2, no production type implements `Marshallable`. The
SimpleRWCommand interface still takes `shared_ptr<Marshallable>` as
the legacy shape — it can either:

* **Delete the legacy overloads entirely** (keep only the Command
  overloads added in prep6ay/6bb/6bc).
* **Or** flip the primary to `const Command&` and let the legacy
  shape go through `Command{shared_ptr<...>}` if any caller still
  needs it (none should, after L10f-1).

Steps:
1. **Sub-leaf 3a**: Delete the `shared_ptr<Marshallable>`-taking
   ctor and static methods. Promote the Command overloads to be
   the canonical implementation (move bodies from the old
   shared_ptr versions into the Command versions).
2. **Sub-leaf 3b**: Drop the `static_pointer_cast<Marshallable>`
   workarounds at `rule/coordinator.cc:55` and `copilot/server.cc:998`
   — pass the shared_ptr<DerivedT> directly, which now resolves
   unambiguously to the Command ctor (Command's templated ctor is
   the only available conversion).

### L10f-4: Migrate `WitnessLog` debug struct (~20 LOC)

`WitnessLog` is conditional on `WITNESS_LOG_DEBUG` (commented out
in default builds). Currently its `cmd_` field is
`shared_ptr<Marshallable>` and ctor takes the same.

Steps:
1. Migrate `cmd_` field to `janus::Command` and ctor to
   `const Command&`.
2. Update the four constructions in `scheduler.cc` to pass Command
   directly (drop `cmd_env.inner_marshallable()` lifts).
3. The `c` argument at line 551 (`shared_ptr<TpcCommitCommand>`)
   needs the same `Command{c}` explicit wrap or
   `static_pointer_cast` we used for SimpleRWCommand/Conflict.

### L10f-5: Drop `MarshallDeputy` (~300 LOC, net negative)

`MarshallDeputy` has narrow remaining use:
* Production code: `kind_` constants (`CONTAINER_CMD`, `UNKNOWN`,
  `ANY_MESSAGE`) referenced from `command.h`, `procedure.h`,
  `rcc_rpc.h`, etc. These are just int constants — move them
  somewhere stable (e.g., `constants.h` or `mako_commands.h`).
* Tests: `TestMarshallable` and friends still construct
  `MarshallDeputy` directly. Migrate these tests to use Command
  with a TestSerializable proxy, OR keep `MarshallDeputy` as a
  test-only legacy shim in `src/srpc/tests/`.
* Bridge: `as_marshallable` / `wrap_serializable` / etc. all
  reference `MarshallDeputy` indirectly via the legacy registry.
  Once production no longer needs the bridge, delete it.

Steps:
1. **Sub-leaf 5a**: Move `MarshallDeputy::kind_` constants to a
   stable home (likely `mako_commands.h` since they're TypeList-related).
2. **Sub-leaf 5b**: Delete the `marshal_serializable_bridge.hpp`
   helpers that `Marshallable` users no longer need
   (`as_marshallable`, `wrap_serializable*`, `as_serializable`,
   `SerializableMarshallableAdapter`,
   `MarshallableSerializableAdapter`). Any tests still calling them
   migrate to direct Command construction.
3. **Sub-leaf 5c**: Delete `MarshallDeputy` class itself. Drop
   the `reg_initializer` machinery (replaced by the closed-set
   TypeList).

### L10f-6: Drop `Marshallable` base class (~50 LOC)

After L10f-1..5, no production type and few tests still reference
`Marshallable`. Final cleanup:

1. **Sub-leaf 6a**: Migrate or delete the remaining test fixtures
   (`TestMarshallable`, `CanaryMarshallable`, `TestCommand`,
   `AnyMessage`). Replace with Serializable-based test fixtures
   if their behavior is still needed.
2. **Sub-leaf 6b**: Delete `Marshallable` class definition.
3. **Sub-leaf 6c**: Drop the `verify(0)`-default
   `to_marshal/from_marshal` virtuals and any related boilerplate.

### L10g: Drop legacy `Marshal& operator<<` for polymorphic types (~50 LOC)

These two operators on `MarshallDeputy` (in `marshal.hpp:1470` and
`:1481`) become dead with L10f's `MarshallDeputy` removal. Auto-falls-out.

## Estimated scope

| Phase | LOC | Files | Risk |
|-------|-----|-------|------|
| L10f-1 | ~150 | ~5 | medium (CmdData inheritance touches Tx/SimpleCommand/many call sites) |
| L10f-2 | ~80 | ~3 | medium (Command storage flip; wire format must be preserved) |
| L10f-3 | ~50 | 2 (RW_command.{h,cc}) | low |
| L10f-4 | ~20 | 1 (scheduler.cc/h) | low (debug-only) |
| L10f-5 | ~300 (net negative) | ~10 | medium (test migration) |
| L10f-6 | ~50 | ~3 | low |
| L10g | ~50 | 1 (marshal.hpp) | low |

Total: ~700 LOC churned, **~600+ LOC removed net**.

## Sequencing notes

* L10f-1 → L10f-3: must come before L10f-2 (so `Command` storage
  flip has nothing left to support).
* L10f-4 stays last among the migration leaves; trivial cleanup
  but blocks the final Marshallable-base removal.
* L10f-5 → L10f-6: order matters — `MarshallDeputy` references
  `Marshallable` indirectly; drop deputy first, then base.
* L10g auto-follows L10f-5 once `MarshallDeputy` is gone.

## Wire-format invariant

Across **every** sub-leaf, encoded bytes for any closed-set type must
match what the legacy `MarshallDeputy + Marshallable::to_marshal`
path produced. Test coverage for this:

* `test_rpc_marshal_archive` — primitives + containers byte-compat.
* The L10c `serializable_envelope` round-trip tests — closed-set
  polymorphic types byte-compat.
* The 101-test integration suite — smoke coverage of the wire-format-
  sensitive paths under load.

## Recommended starting point

**L10f-1a + 1b together as a single leaf**: split CmdData and
migrate SimpleCommand/TxData inheritance. This unblocks every
subsequent step. Estimated: ~150 LOC, 1 day of careful work +
test cycles.
