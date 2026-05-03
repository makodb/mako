#pragma once

// Workstream N Phase 3b — Marshallable ↔ Serializable adapters.
//
// During Phase 4 we migrated per-command-type implementations from the
// old `Marshallable` (virtual to_marshal/from_marshal) interface to
// the new `Serializable` (save/load/kind via SerializableProxy)
// interface.  Production payload types now implement the Serializable
// triplet; the Marshallable base survives only as the in-memory shape
// that `MarshallDeputy::inner()` and the ~30 internal scheduler/server
// APIs taking `shared_ptr<Marshallable>` still consume — closing that
// gap is the L10f / L10g milestone.
//
// This header provides one adapter and a couple of free helpers that
// bridge Serializable types into the legacy Marshallable shape:
//
//   SerializableMarshallableAdapter  Wraps a SerializableProxy and
//                                    presents it as a Marshallable
//                                    (save→to_marshal, load→from_marshal).
//                                    Uses the Phase 3a Marshal↔Archive
//                                    bridges; both directions work.
//
// Free helper:
//   as_marshallable(SerializableProxy) -> shared_ptr<Marshallable>
//       (full bidirectional via 3a bridges)
//
// Workstream N L10d-prep (2026-05-03): dropped the reverse-direction
// machinery — `MarshallableSerializableAdapter`,
// `as_serializable(shared_ptr<Marshallable>)`, and the
// `MarshallDeputy::serializable()` lazy accessor that was its only
// production caller.  Production now uses `janus::Command`
// (`SerializableEnvelope<MakoCommands>`) which has its own
// proxy-shaped save path that drives the proxy directly with no
// `Marshallable→Serializable` adaptation layer needed.

#include <memory>
#include <utility>

#include "../base/all.hpp"
#include "marshal.hpp"
#include "marshal_archive.hpp"

namespace rrr {

// ---- Serializable → Marshallable adapter -----------------------------
//
// Inherits Marshallable. Holds a SerializableProxy. Forwards
// to_marshal/from_marshal through the Phase 3a Marshal bridges.
class SerializableMarshallableAdapter : public Marshallable {
 public:
  explicit SerializableMarshallableAdapter(SerializableProxy proxy)
      : Marshallable(proxy->kind()), serializable_(std::move(proxy)) {}

  // @safe - delegates through MarshalSink to the underlying
  // SerializableProxy's save method. Bytes flow:
  //   Marshallable::to_marshal(out) → SerializableMarshallableAdapter
  //   → MarshalSink(out) → BinaryWriteArchive → SerializableProxy::save
  Marshal& to_marshal(Marshal& out) const override {
    MarshalSink sink(&out);
    BinaryWriteArchive writer(&sink);
    serializable_->save(writer);
    return out;
  }

  // @safe - delegates through MarshalSource to the SerializableProxy's
  // load method. Bytes flow in the opposite direction.
  Marshal& from_marshal(Marshal& in) override {
    MarshalSource source(&in);
    BinaryReadArchive reader(&source);
    serializable_->load(reader);
    return in;
  }

  // Expose the inner SerializableProxy so callers can downcast to the
  // underlying T via `pro::proxy_cast<T*>(&adapter.proxy_mut())`. Used
  // by `serializable_cast<T>` below.
  //
  // Workstream N Phase 4e-2: removed the unused
  // `const SerializableProxy& proxy_view() const` accessor.  All
  // existing callers (`serializable_cast<T>` overloads) need the
  // mutable form to invoke `proxy_cast` against the proxy_indirect
  // accessor; nothing read-only consumed `proxy_view`.
  SerializableProxy& proxy_mut() { return serializable_; }

 private:
  // mutable so the const-only `to_marshal` can call `serializable_->save`
  // through pro::proxy's non-const operator-> (the proxy is mutable
  // even when the underlying convention is const-qualified).
  mutable SerializableProxy serializable_;
};

// ---- Free helpers ----------------------------------------------------

// Wrap a SerializableProxy as a shared_ptr<Marshallable>.
// Both save and load directions work via the Phase 3a bridges.
inline std::shared_ptr<Marshallable> as_marshallable(
    SerializableProxy proxy) {
  return std::make_shared<SerializableMarshallableAdapter>(
      std::move(proxy));
}

// Workstream N L10d-prep: dropped `MarshallableSerializableAdapter`
// + `as_serializable(shared_ptr<Marshallable>)` — the only production
// caller was `MarshallDeputy::serializable()` which itself went away
// when production code switched to `janus::Command` (which has its
// own proxy-shaped save path that avoids the M→S→M→S adapter stacking
// this helper used to short-circuit).

// Wrap a `shared_ptr<T>` typed payload as a `shared_ptr<Marshallable>`
// via the Serializable adapter chain. Intended as the Phase 4
// replacement for `wrap_typed_marshallable<T>(...)` when T has been
// migrated from Marshallable to Serializable.
//
// Semantics: COPY. The new SerializableProxy owns a fresh T copy-
// constructed from `*typed`. The caller's `typed` shared_ptr is left
// pointing to its original instance, which is now SEPARATE from the
// proxy's instance — mutations on one don't reflect in the other.
//
// For Phase 4 migrations of stateless command types
// (`TpcNoopCommand`, etc.) this is fine: there's nothing to alias.
// For types with embedded synchronization state (e.g.
// `TpcEmptyCommand`'s `event` member), use `wrap_serializable_aliased`
// below instead — it preserves the caller's `shared_ptr` aliasing.
template<typename T>
inline std::shared_ptr<Marshallable> wrap_serializable(
    std::shared_ptr<T> typed) {
  verify(typed != nullptr);
  return as_marshallable(make_serializable_proxy<T>(*typed));
}

// Aliased Serializable adapter: holds a `shared_ptr<T>` and forwards
// `save` / `load` / `kind` to the pointed-to T. The proxy and the
// caller's `shared_ptr<T>` reference the SAME T instance — mutations
// through one are visible to the other.
//
// Used by `wrap_serializable_aliased`. Construction takes the
// shared_ptr by value (move-able), then exposes the inner shared_ptr
// via `typed()` so `serializable_cast<T>` can recover it.
template<typename T>
class SerializableSharedPtrAdapter {
 public:
  explicit SerializableSharedPtrAdapter(std::shared_ptr<T> typed)
      : typed_(std::move(typed)) {
    verify(typed_ != nullptr);
  }

  int32_t kind() const { return typed_->kind(); }
  void save(BinaryWriteArchive& ar) const { typed_->save(ar); }
  void load(BinaryReadArchive& ar) { typed_->load(ar); }

  std::shared_ptr<T> typed() const { return typed_; }

 private:
  std::shared_ptr<T> typed_;
};

// Wrap a `shared_ptr<T>` typed payload as a `shared_ptr<Marshallable>`
// PRESERVING aliasing: the resulting Marshallable's underlying data is
// the SAME T instance as `*typed`. Mutations through `typed` after the
// wrap are visible to the encoded value (and vice versa for the
// underlying T's state).
//
// Required for command types with embedded sender↔apply
// synchronization (e.g., `TpcEmptyCommand`'s `event` member, where
// the apply path on the leader needs to call `Done()` on the sender's
// instance to wake `Wait()`).
template<typename T>
inline std::shared_ptr<Marshallable> wrap_serializable_aliased(
    std::shared_ptr<T> typed) {
  verify(typed != nullptr);
  return as_marshallable(
      make_serializable_proxy<SerializableSharedPtrAdapter<T>>(
          std::move(typed)));
}

// ---- MarshallDeputy ↔ Serializable registration --------------------
//
// Phase 4 prep: register a Serializable type T (any non-Marshallable
// type that has the `save`/`load`/`kind` triplet) under `kind` so that
// `MarshallDeputy::operator>>` can decode an instance of T from the
// wire. The factory creates a fresh T-backed SerializableProxy and
// wraps it as a Marshallable via `SerializableMarshallableAdapter`,
// which routes its `to_marshal` / `from_marshal` calls through the
// Phase 3a `MarshalSink` / `MarshalSource` bridges to T's `save` /
// `load`. T is therefore byte-for-byte indistinguishable on the wire
// from a Marshallable subclass implementing the same fields.
//
// L6-pivot (2026-05-01): the prior dispatch used a `SerializableConcept`
// C++20 concept to constrain the bridge templates.  That concept was
// retired (see `marshal_archive.hpp` for the rationale) — the
// templates now dispatch on `!std::is_base_of_v<Marshallable, T>`
// alone and trust the proxy library to reject wrong-shaped T at
// `pro::make_proxy<>` instantiation time.
//
// Usage (mirrors `MarshallDeputy::reg_initializer<T>(kind)` for
// Marshallable types):
//
//   static int reg_my_cmd =
//       rrr::reg_serializable_in_deputy<MyCommand>(MyCommand::kKind);
//
// Write-side construction: `MarshallDeputy(make_shared<MyCommand>())`
// works directly — the `MarshallDeputy(shared_ptr<T>)` template ctor
// (constrained on `!is_base_of_v<Marshallable, T>`) routes through
// `wrap_typed_marshallable` → `wrap_serializable` → `as_marshallable`
// to land a `SerializableMarshallableAdapter` in `inner_sp_data_`.
// No dedicated `MarshallDeputy(SerializableProxy)` ctor exists; Phase
// 3f-2/3 added the parallel lazy `serializable_` field via the
// existing `as_marshallable` entry path rather than a new ctor.
template<typename T>
inline int reg_serializable_in_deputy(int32_t kind) {
  // Workstream N Phase 5b-9: factory returns
  // `shared_ptr<Marshallable>` directly — no MarInitializerState
  // wrapper. `MarshallDeputy::set_marshallable` derives kind from
  // the result.
  return MarshallDeputy::reg_initializer(kind, [kind]() -> std::shared_ptr<Marshallable> {
    auto proxy = make_serializable_proxy<T>();
    verify(proxy->kind() == kind);
    return as_marshallable(std::move(proxy));
  });
}

// ---------------------------------------------------------------------------
// L6-pivot TypeList POC (2026-05-02): central-list-with-declaration-
// order discriminants, mirroring Rust's `enum Foo { ... }` + bincode
// idiom.
//
// User types inherit from `rrr::Serializable<MyType, AllPayloads>`,
// where `AllPayloads = rrr::TypeList<MyType, OtherType, ...>` is a
// central forward-declared list of every polymorphic payload type.
// Each type's wire kind = its index in the list (constexpr int32_t).
//
// Properties:
//   * Cross-compiler / cross-machine deterministic — kinds depend
//     only on TypeList declaration order, which is in source.
//   * Zero collision risk — distinct types get distinct indices.
//   * Rename-stable — renaming a type doesn't change its position
//     in the list, so its kind is unchanged.
//   * Backward-compat by appending — adding a new type at the END of
//     the TypeList assigns it the next available kind; existing types
//     keep theirs.  Reordering or removing is a wire-break.
//
// Replaces:
//   * Manual `static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_X`
//     (per-type constant + central int enum) — collapses into the
//     single TypeList declaration.
//   * The earlier FNV-1a-of-typeid POC — replaced because hashing is
//     implementation-dependent (mangled name format) and rename-fragile.
//
// Usage:
//
//   namespace janus {
//   class EmptyGraph;          // forward decls
//   class TpcCommitCommand;
//
//   using AllPayloads = rrr::TypeList<EmptyGraph, TpcCommitCommand>;
//
//   class EmptyGraph : public rrr::Serializable<EmptyGraph, AllPayloads> {
//    public:
//     void save(BinaryWriteArchive&) const {}
//     void load(BinaryReadArchive&) {}
//     // kind() and static_kind() inherited; auto-derived from list index.
//   };
//   }
//
//   // In .cc:
//   static int _reg = rrr::reg_serializable_in_deputy<EmptyGraph>();
//
// The no-arg `reg_serializable_in_deputy<T>()` overload picks up T's
// kind from `T::static_kind()` (which delegates to the TypeList).
template<typename T>
inline int reg_serializable_in_deputy() {
  return reg_serializable_in_deputy<T>(T::static_kind());
}

// Empty-default payload list — types that inherit
// `Serializable<T>` without an explicit list resolve `index_of<T>()`
// to 0 (= MarshallDeputy::UNKNOWN), which surfaces as a `verify(0)`
// at `set_marshallable` time rather than a silent wrong kind.
struct DefaultPayloadList {
  template<typename T>
  static constexpr int32_t index_of() noexcept { return 0; }
};

template<typename Derived, typename PayloadList = DefaultPayloadList>
struct Serializable {
  // Instance-level `kind()` — picked up by `SerializableMemKind` in
  // the proxy's facade dispatch.  Required by the Serializable contract:
  //   `save(BinaryWriteArchive&) const`, `load(BinaryReadArchive&)`,
  //   `kind() const -> int32_t`.
  int32_t kind() const noexcept {
    return PayloadList::template index_of<Derived>();
  }

  // Class-level kind for `md.kind_ == MyType::static_kind()`
  // comparisons in user code without needing an instance.
  static int32_t static_kind() noexcept {
    return PayloadList::template index_of<Derived>();
  }
};

// Cast a `shared_ptr<Marshallable>` (typically from
// `MarshallDeputy::inner()`) back to the underlying Serializable type
// `T`. Returns nullptr if the value is null, or if it does not wrap
// `T` via either `SerializableMarshallableAdapter` shape:
//
//   - Value-semantic: proxy holds T directly (created by
//     `make_serializable_proxy<T>`, via `wrap_serializable` or the
//     `reg_serializable_in_deputy` factory).
//   - Aliased: proxy holds `SerializableSharedPtrAdapter<T>`, which
//     in turn holds a `shared_ptr<T>` (created by
//     `wrap_serializable_aliased`).
//
// The returned `T*` aliases the proxy's owned T; valid for the
// lifetime of the SerializableMarshallableAdapter (typically as long
// as the MarshallDeputy holding it).
//
// Phase 4 migrations replace `marshallable_cast<T>(md.inner())` with
// `serializable_cast<T>(md.inner())` for types moved from
// `Marshallable` to `Serializable`. The two coexist during the
// migration window: the old call site stays working for unmigrated
// types, the new one for migrated ones.
template <typename T>
inline T* serializable_cast(const std::shared_ptr<Marshallable>& value) {
  if (value == nullptr) return nullptr;
  auto adapter = std::dynamic_pointer_cast<SerializableMarshallableAdapter>(
      value);
  if (adapter == nullptr) return nullptr;
  // `proxy_cast` is a friend function injected by
  // `pro::skills::indirect_rtti`. Found via ADL on the
  // proxy_indirect_accessor returned by `*proxy`. Returns nullptr if
  // the wrapped type is not T (no exception).
  // Try value-semantic shape first.
  if (auto* p = proxy_cast<T>(&*adapter->proxy_mut())) {
    return p;
  }
  // Try aliased shape.
  if (auto* sptr_adapter = proxy_cast<SerializableSharedPtrAdapter<T>>(
          &*adapter->proxy_mut())) {
    return sptr_adapter->typed().get();
  }
  return nullptr;
}

template <typename T>
inline T* serializable_cast(Marshallable* value) {
  if (value == nullptr) return nullptr;
  auto* adapter = dynamic_cast<SerializableMarshallableAdapter*>(value);
  if (adapter == nullptr) return nullptr;
  if (auto* p = proxy_cast<T>(&*adapter->proxy_mut())) {
    return p;
  }
  if (auto* sptr_adapter = proxy_cast<SerializableSharedPtrAdapter<T>>(
          &*adapter->proxy_mut())) {
    return sptr_adapter->typed().get();
  }
  return nullptr;
}

template <typename T>
inline T* serializable_cast(const MarshallDeputy& md) {
  return serializable_cast<T>(md.inner());
}

// ---- MarshallDeputy ↔ BinaryWriteArchive / BinaryReadArchive --------
//
// Phase 4 enabler: lets command types containing a `MarshallDeputy`
// field write `ar << md` / `ar >> md` in their `save` / `load`
// methods, byte-for-byte equivalent to the legacy `Marshal` operator
// pair.
//
// Wire format: `[kind: int32_t] [payload: <bytes from inner's save>]`.
// Same as the existing `operator<<(Marshal&, const MarshallDeputy&)`.
//
// READ side restriction: the BinaryReadArchive's source must be a
// MarshalSource (i.e., the wrapper around an `rrr::Marshal`). This
// is the common case in the RPC framework — incoming bytes are
// buffered into a `Marshal` (Request::m) and read out via a
// MarshalSource. Other source types abort because the
// MarshallDeputy wire format has no length prefix at the
// payload-bytes level, so streaming-from-arbitrary-source can't
// know when from_marshal has finished consuming.

inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar,
                                      const MarshallDeputy& md) {
  verify(md.kind_ != MarshallDeputy::UNKNOWN);
  verify(md.has_marshallable());
  // Workstream N L9: kind serialized as v32 (1 byte for values 0-63,
  // up to 5 bytes for full int32 range) instead of raw 4-byte int32.
  // Matches the legacy `operator<<(Marshal&, MarshallDeputy&)` after
  // its L9 migration (see marshal.hpp).
  ar << rrr::v32(md.kind_);
  // Workstream N L10d-prep: drive payload bytes through inner's
  // `to_marshal` via a temp Marshal — the lazy `md.serializable()`
  // proxy cache went away with the rest of the bridge machinery
  // production stopped using.  This path is now test-only (production
  // uses `janus::Command` which has its own save path that drives the
  // proxy directly with no temp Marshal allocation), so the extra
  // copy is irrelevant.
  Marshal tmp;
  md.inner()->to_marshal(tmp);
  char buf[4096];
  while (true) {
    size_t got = tmp.read(buf, sizeof(buf));
    if (got == 0) break;
    ar.write_bytes(buf, got);
  }
  return ar;
}

inline BinaryReadArchive& operator>>(BinaryReadArchive& ar,
                                     MarshallDeputy& md) {
  // Recover the underlying Marshal from the source via proxy_cast.
  // (proxy_cast is found via ADL on the proxy_indirect_accessor.)
  auto* mark_adapter =
      proxy_cast<MarshalSourceAdapter>(&*ar.source());
  verify(mark_adapter != nullptr &&
         "operator>>(BinaryReadArchive, MarshallDeputy) requires the "
         "archive's source to be a MarshalSource. Wrap the wire bytes "
         "in a Marshal first; the streaming-from-arbitrary-source case "
         "needs a length-prefixed wire format which we don't have.");
  Marshal* m = mark_adapter->source()->marshal();
  verify(m != nullptr);
  // Delegate to the existing legacy operator>> which reads kind and
  // dispatches to the registered factory + from_marshal.
  *m >> md;
  return ar;
}

// ---- Phase 4a-prep: Serializable overloads of marshallable_cast and
//      wrap_typed_marshallable. -----------------------------------
//
// These complement the legacy overloads in `marshal.hpp`. Types
// migrated from Marshallable to Serializable (no Marshallable
// inheritance, no TypedMarshallableAdapter trait) hit the bridge
// overload via overload resolution on the `requires` clauses in both
// places. Call sites continue to use `marshallable_cast<T>(...)` and
// `wrap_typed_marshallable(make_shared<T>())` regardless of T's
// migration state.

// `wrap_typed_marshallable<T>` for Serializable types — routes
// through `wrap_serializable<T>`. Call sites that did
// `wrap_typed_marshallable(make_shared<TpcCommitCommand>())` continue
// to compile and produce identical wire bytes after TpcCommitCommand
// is migrated to Serializable.
//
// L6-pivot (2026-05-01): SerializableConcept dropped from the
// requires-clause.  T is assumed to be Serializable (save/load/kind);
// if not, the failure surfaces inside `pro::make_proxy<>` rather than
// at this constraint.  Removes the C++20-concept boilerplate that was
// redundant with what the proxy library already enforces.
template<typename T>
  requires (!std::is_base_of_v<Marshallable, T>)
inline std::shared_ptr<Marshallable> wrap_typed_marshallable(
    std::shared_ptr<T> typed) {
  return wrap_serializable(std::move(typed));
}

// `marshallable_cast<T>` for Serializable types — routes through
// `serializable_cast<T>`, then synthesizes a `shared_ptr<T>` aliasing
// the underlying SerializableMarshallableAdapter via the shared_ptr
// aliasing constructor. This preserves the legacy
// `shared_ptr<T> = marshallable_cast<T>(value)` call shape; the
// returned shared_ptr extends the lifetime of `value`'s control block
// (the SerializableMarshallableAdapter).
//
// L6-pivot (2026-05-01): SerializableConcept dropped from the
// requires-clause.  Wrong-T calls now silently return nullptr at
// runtime via the proxy_cast probe in `serializable_cast<T>` rather
// than failing at this constraint — the trade-off accepted to remove
// the redundant concept layer.
template<typename T>
  requires (!std::is_base_of_v<Marshallable, T>)
inline std::shared_ptr<T> marshallable_cast(
    const std::shared_ptr<Marshallable>& value) {
  T* p = serializable_cast<T>(value);
  if (p == nullptr) return nullptr;
  // Aliasing ctor: keeps `value` (the SerializableMarshallableAdapter)
  // alive while pointing to T inside its proxy.
  return std::shared_ptr<T>(value, p);
}

}  // namespace rrr
