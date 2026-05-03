#pragma once

// Workstream N L10b: `SerializableEnvelope<TypeList>` — closed-set
// polymorphic carrier.
//
// Replaces `MarshallDeputy` for closed-set polymorphism. Holds a
// `SerializableProxy` whose underlying type T is one of the entries
// in `TypeList::Ts...`. Wire format `[v32 kind][payload bytes]` —
// byte-for-byte identical to `MarshallDeputy` post-L9, so any field
// migrating from `MarshallDeputy` to `SerializableEnvelope<MyList>`
// has no on-the-wire change as long as `MyList` reproduces the same
// kind-to-type mapping.
//
// Compared to MarshallDeputy:
//   * No `Marshallable` virtual base, no `inner_sp_data_` field —
//     the carrier is just a `SerializableProxy`.
//   * No runtime `MarshallDeputy::reg_initializer` registry — kind →
//     factory is `TypeList::create_at(pos)`, decided at compile time
//     (L10a).
//   * Type-safe: `pack<T>` / `unpack<T>` / `is_a<T>` static_assert
//     that T is in the TypeList.
//
// Wire format:
//   [v32 kind] [payload bytes from proxy->save(ar)]
//
// Aliasing semantics:
//   * `pack(value)` — VALUE-SEMANTIC: proxy holds a copy of `value`.
//     Mutations on the caller's instance after pack do NOT reflect
//     in the encoded payload.
//   * `pack_aliased(shared_ptr<T>)` — ALIASED: proxy holds a
//     `SerializableSharedPtrAdapter<T>` which retains the caller's
//     `shared_ptr<T>`. Mutations through the caller's pointer are
//     visible to the encoded payload, and `unpack<T>()` returns a
//     pointer aliasing the same instance.
//
// Copy semantics:
//   The proxy is held behind a `std::shared_ptr<SerializableProxy>`,
//   so SerializableEnvelope is COPYABLE.  Copies share the same
//   underlying proxy (and therefore the same payload value); mutations
//   through one envelope's `unpack<T>()->...` are visible to other
//   envelopes that share the proxy.  Mirrors `MarshallDeputy`'s
//   shared_ptr-based copy semantics so call sites that did
//   `req.cmd = some_md;` continue to work after migration.
//
// Usage example (closed-set TypeList already defined in
// `deptran/mako_commands.h`):
//
//   namespace janus {
//   using Command = rrr::SerializableEnvelope<MakoCommands>;
//   }
//
//   // Sender:
//   auto cmd = std::make_shared<TpcCommitCommand>();
//   cmd->tx_id_ = 42;
//   janus::Command outgoing = janus::Command::pack_aliased(cmd);
//
//   // Receiver:
//   janus::Command incoming;
//   ar >> incoming;
//   if (auto* commit = incoming.unpack<TpcCommitCommand>()) {
//     // ... use commit ...
//   }

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "../base/all.hpp"
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
  // The deputy must hold a Serializable payload (SerializableMarshallableAdapter
  // wrapping a SerializableProxy); pure legacy `Marshallable` payloads
  // are not supported here (those types should already have migrated
  // to Serializable in Phase 4).
  //
  // Aliases the adapter via shared_ptr so the resulting envelope shares
  // ownership with any other deputy/envelope referring to the same
  // adapter — same lifetime semantics as `MarshallDeputy`'s
  // `inner_sp_data_` shared_ptr.
  SerializableEnvelope(const MarshallDeputy& md) { assign_from(md); }

  SerializableEnvelope& operator=(const MarshallDeputy& md) {
    assign_from(md);
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
    env.proxy_ = std::make_shared<SerializableProxy>(
        make_serializable_proxy<T>(value));
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
    env.proxy_ = std::make_shared<SerializableProxy>(
        make_serializable_proxy<SerializableSharedPtrAdapter<T>>(
            std::move(sp)));
    return env;
  }

  // -- Typed accessors ---------------------------------------------------
  // Recover the carried payload as a `T*`, or nullptr if the carried
  // type is not T (or the envelope is empty). Aliases the proxy's
  // owned T; valid for the lifetime of the proxy (i.e., as long as
  // any envelope holding the same shared_ptr is alive).
  template<typename T>
  T* unpack() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (!has_value()) return nullptr;
    return unpack_impl<T>();
  }

  template<typename T>
  const T* unpack() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (!has_value()) return nullptr;
    return const_cast<SerializableEnvelope*>(this)->unpack_impl<T>();
  }

  // Recover the carried payload as a `shared_ptr<T>` aliasing the
  // proxy's owned T.  The aliasing constructor keeps the underlying
  // proxy (and its payload) alive for the lifetime of the returned
  // shared_ptr.  Drop-in for the legacy `marshallable_cast<T>(md)`
  // shared_ptr-returning pattern; most call sites use this when they
  // need to outlive the Command/envelope.
  template<typename T>
  std::shared_ptr<T> unpack_shared() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    T* raw = unpack<T>();
    if (raw == nullptr) return nullptr;
    // Aliasing ctor: keeps the proxy_ shared_ptr alive while the
    // returned pointer aliases the T inside the proxy.
    return std::shared_ptr<T>(proxy_, raw);
  }

  template<typename T>
  std::shared_ptr<const T> unpack_shared() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    const T* raw = unpack<T>();
    if (raw == nullptr) return nullptr;
    return std::shared_ptr<const T>(proxy_, raw);
  }

  // True iff the carried payload is a T.
  template<typename T>
  bool is_a() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::is_a<T>: T is not in TypeList.");
    return unpack<T>() != nullptr;
  }

  // -- Discriminator + state ---------------------------------------------
  // Kind value of the carried payload (== TypeList position). Returns
  // 0 (UNKNOWN) on an empty envelope.
  int32_t kind() const {
    if (!has_value()) return 0;
    return (*proxy_)->kind();
  }

  bool has_value() const noexcept {
    return proxy_ != nullptr && proxy_->has_value();
  }
  explicit operator bool() const noexcept { return has_value(); }

  // -- Wire ops ----------------------------------------------------------
  // Wire format: [v32 kind] [payload bytes].  Same as MarshallDeputy
  // post-L9; migrating a field from MarshallDeputy to
  // SerializableEnvelope<MyList> with a TypeList that preserves the
  // kind→type mapping is a zero-byte wire change.
  void save(BinaryWriteArchive& ar) const {
    verify(has_value() &&
           "SerializableEnvelope::save: empty envelope cannot be encoded.");
    ar << v32((*proxy_)->kind());
    (*proxy_)->save(ar);
  }

  void load(BinaryReadArchive& ar) {
    v32 kind_v;
    ar >> kind_v;
    proxy_ = std::make_shared<SerializableProxy>(
        TypeList::create_at(kind_v.get()));
    (*proxy_)->load(ar);
  }

  // -- Proxy access for advanced users -----------------------------------
  // Direct access to the underlying proxy. Most callers should prefer
  // unpack<T> / is_a<T> for type-checked recovery. This accessor is
  // for code that needs to forward an opaque carrier without unpacking.
  // Aborts via verify() on an empty envelope.
  SerializableProxy& proxy() {
    verify(has_value());
    return *proxy_;
  }
  const SerializableProxy& proxy() const {
    verify(has_value());
    return *proxy_;
  }

 private:
  // Migration compat helper: extract the adapter from a deputy.
  void assign_from(const MarshallDeputy& md) {
    if (!md.has_marshallable()) {
      proxy_.reset();
      return;
    }
    auto adapter = std::dynamic_pointer_cast<SerializableMarshallableAdapter>(
        md.inner());
    verify(adapter != nullptr &&
           "SerializableEnvelope::assign_from(MarshallDeputy): deputy "
           "holds a non-Serializable Marshallable.  Migrate the payload "
           "type to Serializable (Phase 4) before passing through Command.");
    // Aliasing ctor: the resulting shared_ptr keeps the adapter alive
    // and points at its proxy member.  Mutations through this proxy
    // are visible to any other shared_ptr aliasing the same adapter.
    proxy_ = std::shared_ptr<SerializableProxy>(adapter,
                                                &adapter->proxy_mut());
  }

  template<typename T>
  T* unpack_impl() {
    // Try value-semantic shape first (pack(value) — proxy holds T directly).
    if (auto* p = proxy_cast<T>(&**proxy_)) {
      return p;
    }
    // Try aliased shape (pack_aliased(shared_ptr<T>) — proxy holds an
    // adapter that holds the shared_ptr<T>).
    if (auto* sp =
            proxy_cast<SerializableSharedPtrAdapter<T>>(&**proxy_)) {
      return sp->typed().get();
    }
    return nullptr;
  }

  // shared_ptr indirection for copyability (mirrors MarshallDeputy's
  // pattern).  Copies of SerializableEnvelope share the same proxy
  // and therefore the same payload value.
  std::shared_ptr<SerializableProxy> proxy_;
};

// Migration compat: `marshallable_cast<T>` overload for envelopes.
// Lets existing `marshallable_cast<T>(req.cmd)` call sites keep
// compiling after `req.cmd` migrates from `MarshallDeputy` to
// `SerializableEnvelope<MyList>`.  Returns `shared_ptr<T>` aliasing
// the proxy's owned T; same semantics as the legacy
// `marshallable_cast<T>(MarshallDeputy&)` path that this replaces.
template<typename T, typename TypeList>
inline std::shared_ptr<T> marshallable_cast(
    SerializableEnvelope<TypeList>& env) {
  return env.template unpack_shared<T>();
}

template<typename T, typename TypeList>
inline std::shared_ptr<T> marshallable_cast(
    const SerializableEnvelope<TypeList>& env) {
  // unpack on a const env — aliases via const proxy access.
  // We need a non-const T* because shared_ptr<T> aliasing.
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
