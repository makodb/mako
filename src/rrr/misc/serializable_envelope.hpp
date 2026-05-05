#pragma once

// Workstream N L10b: `SerializableEnvelope<TypeList>` — closed-set
// polymorphic carrier.
//
// Replaces `MarshallDeputy` for closed-set polymorphism. The internal
// storage is `shared_ptr<Marshallable>` (a SerializableMarshallableAdapter
// for Phase-4-migrated Serializable types) — same shape as
// `MarshallDeputy::inner_sp_data_`, so code that takes
// `shared_ptr<Marshallable>` keeps working when call sites pass
// `env.inner_marshallable()` instead of `md.inner()`.  This makes the
// L10c-cmds migration mostly mechanical: the .rpc field type changes
// from `MarshallDeputy` to `Command`, and call sites swap in the
// matching accessors — without touching the deeper internal APIs that
// pass `shared_ptr<Marshallable>` between scheduler / server / coord.
//
// Wire format `[v32 kind][payload bytes]` — byte-for-byte identical
// to `MarshallDeputy` post-L9, so the field type swap has no on-the-
// wire impact for matched kind→type mappings.
//
// Compared to MarshallDeputy:
//   * No runtime `MarshallDeputy::reg_initializer` registry — kind →
//     factory is `TypeList::create_at(pos)`, decided at compile time
//     (L10a).  When this header is included from a TU that doesn't
//     have all TypeList types complete, callers can avoid
//     instantiating `load()` and rely solely on the conversion-from-
//     MarshallDeputy path during the transition.
//   * Type-safe: `pack<T>` / `unpack<T>` / `is_a<T>` static_assert
//     that T is in the TypeList.
//
// Aliasing semantics (mirror the existing bridge helpers):
//   * `pack(value)` — VALUE-SEMANTIC: proxy owns a copy of `value`.
//   * `pack_aliased(shared_ptr<T>)` — ALIASED: mutations on the
//     caller's `shared_ptr<T>` after `pack` remain visible.
//
// Copy semantics: COPYABLE.  Copies share the same underlying
// `shared_ptr<Marshallable>` and therefore the same payload — same
// pattern as `MarshallDeputy`.

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "../base/all.hpp"
#include "marshal.hpp"
#include "marshal_archive.hpp"
#include "marshal_serializable_bridge.hpp"

namespace rrr {

template<typename TypeList>
class SerializableEnvelope {
 public:
  SerializableEnvelope() = default;

  // -- Migration compat: implicit conversion from MarshallDeputy ---------
  // Lets `req.cmd = md;` patterns continue to compile after a field
  // migrates from `MarshallDeputy` to `SerializableEnvelope<MyList>`.
  // Aliases the deputy's `inner_sp_data_` so the resulting envelope
  // shares ownership and payload state with the deputy.
  SerializableEnvelope(const MarshallDeputy& md) : inner_(md.inner()) {
    refresh_kind();
  }

  SerializableEnvelope& operator=(const MarshallDeputy& md) {
    inner_ = md.inner();
    refresh_kind();
    return *this;
  }

  // -- Migration compat: implicit construction from shared_ptr<Marshallable>
  // Mirrors `MarshallDeputy(shared_ptr<Marshallable>)` ctor — lets call
  // sites that pass `shared_ptr<Marshallable>` (e.g., `app_next_(slot,
  // ins->cmd)` where app_next_ expects a Command and `ins->cmd` is a
  // shared_ptr<Marshallable>) work without explicit wrapping.
  SerializableEnvelope(std::shared_ptr<Marshallable> sp)
      : inner_(std::move(sp)) {
    refresh_kind();
  }

  // L10f-prep1: `cmd = sp;` form (where sp is shared_ptr<Marshallable>)
  // would otherwise be ambiguous between the Command(sp) ctor + the
  // default operator=(Command&&), and operator=(MarshallDeputy&&) via
  // MarshallDeputy(sp).  Provide a direct overload to disambiguate.
  SerializableEnvelope& operator=(std::shared_ptr<Marshallable> sp) {
    inner_ = std::move(sp);
    refresh_kind();
    return *this;
  }

  // Templated ctor for `shared_ptr<T>` where T is a Marshallable
  // subclass — supports `make_shared<Command>(make_shared<TestMarshallable>(...))`
  // (test fixtures that wrap concrete Marshallable subclasses).
  template<typename T>
    requires std::is_base_of_v<Marshallable, T>
  SerializableEnvelope(std::shared_ptr<T> sp)
      : inner_(std::static_pointer_cast<Marshallable>(std::move(sp))) {
    refresh_kind();
  }

  // L10f-2 step 4 (2026-05-04): templated ctor for non-Marshallable
  // Serializable types.  Routes through `wrap_typed_marshallable<T>`
  // (its `!is_base_of_v<Marshallable, T>` overload in
  // marshal_serializable_bridge.hpp) so call sites that did
  // `Command(wrap_typed_marshallable(sp))` can drop the wrap and write
  // `Command(sp)` directly — including the implicit-conversion sites
  // (`coo->Submit(sp)`, `cmd_field = sp;`).
  template<typename T>
    requires (!std::is_base_of_v<Marshallable, T>)
  SerializableEnvelope(std::shared_ptr<T> sp)
      : inner_(wrap_typed_marshallable(std::move(sp))) {
    refresh_kind();
  }

  // L10f-2 step 4 (2026-05-05): direct templated assignment for
  // non-Marshallable T.  Without this, `cmd_field = sp;` (sp is
  // shared_ptr<T> for some non-Marshallable Serializable T) is
  // ambiguous between two implicit user-defined conversions:
  // shared_ptr<T> → Command (via the templated ctor above) and
  // shared_ptr<T> → MarshallDeputy (via MarshallDeputy's matching
  // templated ctor).  A direct exact-match overload outranks both
  // user-defined conversion sequences and resolves the ambiguity.
  template<typename T>
    requires (!std::is_base_of_v<Marshallable, T>)
  SerializableEnvelope& operator=(std::shared_ptr<T> sp) {
    inner_ = wrap_typed_marshallable(std::move(sp));
    refresh_kind();
    return *this;
  }

  // -- Migration compat: matches `MarshallDeputy::set_marshallable` ------
  // Lets `cmd.set_marshallable(sp)` patterns continue to compile.
  void set_marshallable(std::shared_ptr<Marshallable> sp) {
    inner_ = std::move(sp);
    refresh_kind();
  }

  // Templated overload for non-Marshallable Serializable types (mirrors
  // `MarshallDeputy::set_marshallable<T>(shared_ptr<T>)` template).  Routes
  // through `wrap_typed_marshallable<T>` which goes through the bridge
  // for Serializable T's.
  template<typename T>
    requires (!std::is_base_of_v<Marshallable, T>)
  void set_marshallable(std::shared_ptr<T> typed) {
    inner_ = wrap_typed_marshallable(std::move(typed));
    refresh_kind();
  }

  // -- Migration compat: matches `MarshallDeputy::has_marshallable` ------
  bool has_marshallable() const noexcept { return has_value(); }

  // -- Migration compat: `inner()` alias for `inner_marshallable()` ------
  // Matches `MarshallDeputy::inner()` so `md.inner()` callers swap
  // verbatim.  Returns the same shared_ptr by reference; no copy.
  const std::shared_ptr<Marshallable>& inner() const noexcept {
    return inner_;
  }
  std::shared_ptr<Marshallable>& inner() noexcept { return inner_; }

  // -- Factories ---------------------------------------------------------
  // VALUE-SEMANTIC: proxy owns a copy of `value`.
  template<typename T>
  static SerializableEnvelope pack(const T& value) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::pack<T>: T is not in TypeList. "
                  "Add T to the TypeList declaration.");
    SerializableEnvelope env;
    env.inner_ = as_marshallable(make_serializable_proxy<T>(value));
    env.refresh_kind();
    return env;
  }

  // ALIASED: proxy retains the caller's `shared_ptr<T>`. Mutations
  // through the caller's pointer remain visible to the encoded
  // payload; `unpack<T>()` returns an aliasing pointer.
  template<typename T>
  static SerializableEnvelope pack_aliased(std::shared_ptr<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::pack_aliased<T>: T is not in "
                  "TypeList. Add T to the TypeList declaration.");
    verify(sp != nullptr);
    SerializableEnvelope env;
    env.inner_ = wrap_serializable_aliased(std::move(sp));
    env.refresh_kind();
    return env;
  }

  // -- Typed accessors ---------------------------------------------------
  // Recover the carried payload as a `T*`, or nullptr if the carried
  // type is not T (or the envelope is empty). Aliases the proxy's
  // owned T.
  template<typename T>
  T* unpack() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    return serializable_cast<T>(inner_);
  }

  template<typename T>
  const T* unpack() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    return serializable_cast<T>(inner_);
  }

  // Recover the carried payload as a `shared_ptr<T>` aliasing the
  // proxy's owned T.  Drop-in for the legacy `marshallable_cast<T>(md)`
  // shared_ptr-returning pattern.
  template<typename T>
  std::shared_ptr<T> unpack_shared() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    T* raw = serializable_cast<T>(inner_);
    if (raw == nullptr) return nullptr;
    // Aliasing ctor: keeps inner_ alive while the returned pointer
    // aliases the T inside the adapter's proxy.
    return std::shared_ptr<T>(inner_, raw);
  }

  template<typename T>
  std::shared_ptr<const T> unpack_shared() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    const T* raw = serializable_cast<T>(inner_);
    if (raw == nullptr) return nullptr;
    return std::shared_ptr<const T>(inner_, raw);
  }

  // True iff the carried payload is a T.
  template<typename T>
  bool is_a() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::is_a<T>: T is not in TypeList.");
    return serializable_cast<T>(inner_) != nullptr;
  }

  // -- Discriminator + state ---------------------------------------------
  int32_t kind() const {
    return inner_ != nullptr ? inner_->kind() : 0;
  }

  bool has_value() const noexcept { return inner_ != nullptr; }
  explicit operator bool() const noexcept { return has_value(); }

  // Workstream N L10f-2 step 1 (2026-05-04): identity comparison.
  // Two envelopes compare equal iff they wrap the same payload
  // instance (same shared_ptr).  Lets call sites replace
  // `a.inner_marshallable() != b.inner_marshallable()` with
  // `a != b`.
  bool operator==(const SerializableEnvelope& other) const noexcept {
    return inner_ == other.inner_;
  }
  bool operator!=(const SerializableEnvelope& other) const noexcept {
    return !(*this == other);
  }

  // -- inner_marshallable() — migration compat ---------------------------
  // Direct access to the underlying `shared_ptr<Marshallable>` —
  // matches `MarshallDeputy::inner()` so call sites that pass
  // `md.inner()` to internal APIs taking `shared_ptr<Marshallable>`
  // can swap to `env.inner_marshallable()` with no other change.
  // No allocation; the field IS the shared_ptr.
  const std::shared_ptr<Marshallable>& inner_marshallable() const noexcept {
    return inner_;
  }

  std::shared_ptr<Marshallable>& inner_marshallable() noexcept {
    return inner_;
  }

  // -- Wire ops ----------------------------------------------------------
  // Wire format: [v32 kind] [payload bytes].  Same as MarshallDeputy
  // post-L9.
  void save(BinaryWriteArchive& ar) const {
    verify(has_value() &&
           "SerializableEnvelope::save: empty envelope cannot be encoded.");
    ar << v32(inner_->kind());
    // Fast path: if the inner is a SerializableMarshallableAdapter
    // (the standard shape after pack/pack_aliased / a Phase-4
    // Serializable wrapped via `as_marshallable`), drive save through
    // its proxy directly — avoids the temp-Marshal copy that the
    // legacy `Marshallable::to_marshal` path would incur.
    auto* adapter =
        dynamic_cast<SerializableMarshallableAdapter*>(inner_.get());
    if (adapter != nullptr) {
      adapter->proxy_mut()->save(ar);
      return;
    }
    // Fallback: legacy Marshallable subclass with a custom
    // `to_marshal`.  Drain through a temp Marshal.
    Marshal tmp;
    inner_->to_marshal(tmp);
    char buf[4096];
    while (true) {
      size_t got = tmp.read(buf, sizeof(buf));
      if (got == 0) break;
      ar.write_bytes(buf, got);
    }
  }

  void load(BinaryReadArchive& ar) {
    v32 kind_v;
    ar >> kind_v;
    // Workstream N L10c-cmds: dispatch via the runtime
    // `MarshallDeputy::create_initializer` registry (populated at
    // static-init time by each TypeList type's
    // `reg_serializable_in_deputy<T>()` call).  Avoids forcing
    // every TU that includes `mako_commands.h` (and transitively
    // `rcc_rpc.h`) to have all TypeList types complete — the L10a
    // compile-time `TypeList::create_at(pos)` requires that, but
    // the include graph (paxos_worker.h → coordinator.h → rcc_rpc.h)
    // makes it impossible to ship those headers from rcc_rpc.rpc
    // without circular dependencies.
    //
    // The L10a `TypeList::create_at(pos)` path is still available
    // for tests and direct callers that already have the types in
    // scope (and want compile-time dispatch); it is exercised by
    // the `TypeListFactory.*` tests in `rpc_marshal_archive_test`.
    inner_ = MarshallDeputy::create_initializer(kind_v.get());
    refresh_kind();
    // For Phase-4-migrated Serializable types, `inner_` is a
    // `SerializableMarshallableAdapter`; drive load through its
    // proxy directly so the bytes go through the BinaryReadArchive
    // path.  Legacy Marshallable types fall back to from_marshal.
    auto* adapter =
        dynamic_cast<SerializableMarshallableAdapter*>(inner_.get());
    if (adapter != nullptr) {
      adapter->proxy_mut()->load(ar);
      return;
    }
    // Fallback for legacy Marshallable subclasses — unused for
    // Phase-4-migrated payloads, but kept for completeness.
    auto* mark_adapter =
        proxy_cast<MarshalSourceAdapter>(&*ar.source());
    verify(mark_adapter != nullptr);
    inner_->from_marshal(*mark_adapter->source()->marshal());
  }

 public:
  // -- Migration compat: public `kind_` field matching MarshallDeputy ---
  // Cached snapshot of `inner_->kind()`; refreshed by every state-
  // changing entry point.  Lets `cmd.kind_ == X` direct-field access
  // patterns continue to compile after a field migrates from
  // `MarshallDeputy` to `SerializableEnvelope<MyList>`.  Writers that
  // mutate `kind_` directly are not supported (the field is
  // overwritten on the next state change); migrate to `set_marshallable`
  // or pack/load instead.
  int32_t kind_{0};

 private:
  void refresh_kind() noexcept {
    kind_ = inner_ != nullptr ? inner_->kind() : 0;
  }

  std::shared_ptr<Marshallable> inner_;
};

// Migration compat: `marshallable_cast<T>` overload for envelopes.
// Lets existing `marshallable_cast<T>(req.cmd)` call sites keep
// compiling after `req.cmd` migrates from `MarshallDeputy` to
// `SerializableEnvelope<MyList>`.  Returns `shared_ptr<T>` aliasing
// the inner T; same semantics as the legacy
// `marshallable_cast<T>(MarshallDeputy&)` path that this replaces.
template<typename T, typename TypeList>
inline std::shared_ptr<T> marshallable_cast(
    SerializableEnvelope<TypeList>& env) {
  return env.template unpack_shared<T>();
}

template<typename T, typename TypeList>
inline std::shared_ptr<T> marshallable_cast(
    const SerializableEnvelope<TypeList>& env) {
  return const_cast<SerializableEnvelope<TypeList>&>(env)
      .template unpack_shared<T>();
}

template<typename T, typename TypeList>
inline std::shared_ptr<T> marshallable_cast(
    SerializableEnvelope<TypeList>* env) {
  if (env == nullptr) return nullptr;
  return env->template unpack_shared<T>();
}

// Free archive operators — let SerializableEnvelope ride directly in
// rpcgen-emitted RPC struct fields the same way any other Serializable
// type does.
template<typename TypeList>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar,
                                      const SerializableEnvelope<TypeList>& env) {
  env.save(ar);
  return ar;
}

template<typename TypeList>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar,
                                     SerializableEnvelope<TypeList>& env) {
  env.load(ar);
  return ar;
}

// Workstream N L10f-5 (2026-05-04): legacy `Marshal&` archive
// operators.  Wire format identical to `MarshallDeputy`'s:
// `[v32 kind] [payload bytes]`.  Lets us replace `MarshallDeputy
// view_md` with `Command view_md` in code paths that still drive the
// legacy `Marshal&` (e.g., TxReply / TpcCommitCommand archive
// operators in the legacy RPC reply path).
//
// L10f-2 step 2 (2026-05-04): drive the payload through a
// `BinaryWriteArchive` backed by a `MarshalSink` instead of
// `inner_->to_marshal(m)`.  Same bytes either way (the proxy's
// `save(BinaryWriteArchive&)` is the canonical serializer); this
// version avoids the Marshallable virtual dispatch and is the path
// that survives once the `Marshallable`-shaped `inner_` storage
// goes away.
template<typename TypeList>
inline Marshal& operator<<(Marshal& m,
                           const SerializableEnvelope<TypeList>& env) {
  verify(env.has_value());
  MarshalSink sink(&m);
  BinaryWriteArchive ar(&sink);
  env.save(ar);
  return m;
}

template<typename TypeList>
inline Marshal& operator>>(Marshal& m,
                           SerializableEnvelope<TypeList>& env) {
  MarshalSource source(&m);
  BinaryReadArchive ar(&source);
  env.load(ar);
  return m;
}

}  // namespace rrr
