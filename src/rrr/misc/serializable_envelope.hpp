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
  SerializableEnvelope(const MarshallDeputy& md) : inner_(md.inner()) {}

  SerializableEnvelope& operator=(const MarshallDeputy& md) {
    inner_ = md.inner();
    return *this;
  }

  // -- Factories ---------------------------------------------------------
  // VALUE-SEMANTIC: proxy owns a copy of `value`.
  template<typename T>
  static SerializableEnvelope pack(const T& value) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::pack<T>: T is not in TypeList. "
                  "Add T to the TypeList declaration.");
    SerializableEnvelope env;
    env.inner_ = as_marshallable(make_serializable_proxy<T>(value));
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
    auto proxy = TypeList::create_at(kind_v.get());
    proxy->load(ar);
    inner_ = as_marshallable(std::move(proxy));
  }

 private:
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

}  // namespace rrr
