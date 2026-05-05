#pragma once

// Workstream N L10b: `SerializableEnvelope<TypeList>` — closed-set
// polymorphic carrier.
//
// Wire format `[v32 kind][payload bytes]` — byte-for-byte identical
// to the legacy `MarshallDeputy` post-L9.
//
// Storage shape (post-L10f-2 step 5):
//   * `inner_`: `pro::proxy<SerializableFacade>` value member (no
//     `shared_ptr<Marshallable>` — Marshallable retired in this
//     same release).  The proxy heap-boxes the underlying T
//     internally; copying the envelope deep-copies T via the
//     facade's `support_copy<nontrivial>` skill.
//
// Aliasing semantics:
//   * `pack(value)` — VALUE-SEMANTIC: proxy owns a copy of `value`.
//   * `pack_aliased(shared_ptr<T>)` — ALIASED: the proxy holds the
//     caller's `shared_ptr<T>`.  Mutations through the caller's
//     pointer remain visible via the encoded payload, and copies
//     of the envelope share the underlying T (the shared_ptr's
//     refcount survives the proxy copy).
//
// Lifetime caveats:
//   * `unpack<T>()` returns a raw `T*` aliasing the envelope's
//     proxy-owned T.  The pointer is valid for the lifetime of the
//     envelope (or any envelope copy that shares the T via
//     `pack_aliased`).
//   * `unpack_shared<T>()` returns a `shared_ptr<T>` with a no-op
//     deleter — the returned pointer aliases the envelope-owned T,
//     and the **caller is responsible for keeping the envelope
//     alive** while the shared_ptr is in use.

#include <cstdint>
#include <memory>
#include <utility>

#include "../base/all.hpp"
#include "marshal_archive.hpp"

namespace rrr {

template<typename TypeList>
class SerializableEnvelope {
 public:
  SerializableEnvelope() = default;

  // Templated ctor for `shared_ptr<T>` where T is in TypeList.  Stores
  // an aliased shared_ptr<T> inside the proxy — `unpack_shared<T>()`
  // returns the same shared_ptr<T> instance, and the underlying T
  // outlives any envelope copy via the shared_ptr's refcount.
  template<typename T>
  SerializableEnvelope(std::shared_ptr<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope(shared_ptr<T>): T is not in TypeList. "
                  "Add T to the TypeList declaration.");
    verify(sp != nullptr);
    inner_ = pro::make_proxy<SerializableFacade,
                             details::SerializableSharedPtrHolder<T>>(
        std::move(sp));
    refresh_kind();
  }

  // Templated assignment: same aliased-storage semantics.
  template<typename T>
  SerializableEnvelope& operator=(std::shared_ptr<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::operator=(shared_ptr<T>): T is not "
                  "in TypeList.");
    verify(sp != nullptr);
    inner_ = pro::make_proxy<SerializableFacade,
                             details::SerializableSharedPtrHolder<T>>(
        std::move(sp));
    refresh_kind();
    return *this;
  }

  // -- Factories ---------------------------------------------------------
  // VALUE-SEMANTIC: stores a fresh shared_ptr<T> holding a copy of
  // `value`.  Internally identical to `pack_aliased(make_shared<T>(value))`
  // — gives `unpack_shared<T>` proper refcounted ownership at the
  // cost of one extra heap allocation per pack.  Callers don't see
  // the shared_ptr directly.
  template<typename T>
  static SerializableEnvelope pack(const T& value) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::pack<T>: T is not in TypeList.");
    SerializableEnvelope env;
    env.inner_ = pro::make_proxy<SerializableFacade,
                                 details::SerializableSharedPtrHolder<T>>(
        std::make_shared<T>(value));
    env.refresh_kind();
    return env;
  }

  // ALIASED: proxy retains the caller's `shared_ptr<T>`. Mutations
  // through the caller's pointer remain visible to the encoded
  // payload; `unpack<T>()` returns a pointer aliasing the same
  // instance.  `unpack_shared<T>()` returns a refcount-shared
  // shared_ptr<T>.
  template<typename T>
  static SerializableEnvelope pack_aliased(std::shared_ptr<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::pack_aliased<T>: T is not in "
                  "TypeList.");
    verify(sp != nullptr);
    SerializableEnvelope env;
    env.inner_ = pro::make_proxy<SerializableFacade,
                                 details::SerializableSharedPtrHolder<T>>(
        std::move(sp));
    env.refresh_kind();
    return env;
  }

  // -- Typed accessors ---------------------------------------------------
  // Recover the carried payload as a `T*`, or nullptr if the carried
  // type is not T (or the envelope is empty). Aliases the envelope-
  // owned T.
  template<typename T>
  T* unpack() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (!inner_.has_value()) return nullptr;
    if (auto* p = proxy_cast<T>(&*inner_)) return p;
    if (auto* h = proxy_cast<details::SerializableSharedPtrHolder<T>>(&*inner_)) {
      return h->ptr.get();
    }
    return nullptr;
  }

  template<typename T>
  const T* unpack() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (!inner_.has_value()) return nullptr;
    if (auto* p = proxy_cast<T>(&*inner_)) return p;
    if (auto* h = proxy_cast<details::SerializableSharedPtrHolder<T>>(&*inner_)) {
      return h->ptr.get();
    }
    return nullptr;
  }

  // Recover the carried payload as `shared_ptr<T>`.
  //   * For `pack_aliased`-constructed envelopes: returns the original
  //     shared_ptr<T> (refcount-shared with the caller's pointer).
  //   * For `pack`-constructed envelopes: returns a `shared_ptr<T>`
  //     with a no-op deleter — the pointer aliases the envelope-owned
  //     T and the caller is responsible for keeping the envelope alive.
  template<typename T>
  std::shared_ptr<T> unpack_shared() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    if (!inner_.has_value()) return nullptr;
    if (auto* h = proxy_cast<details::SerializableSharedPtrHolder<T>>(&*inner_)) {
      return h->ptr;
    }
    if (auto* p = proxy_cast<T>(&*inner_)) {
      // No-op deleter: caller responsibility for envelope lifetime.
      return std::shared_ptr<T>(p, [](T*){});
    }
    return nullptr;
  }

  template<typename T>
  std::shared_ptr<const T> unpack_shared() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    if (!inner_.has_value()) return nullptr;
    if (auto* h = proxy_cast<details::SerializableSharedPtrHolder<T>>(&*inner_)) {
      return std::const_pointer_cast<const T>(h->ptr);
    }
    if (auto* p = proxy_cast<T>(&*inner_)) {
      return std::shared_ptr<const T>(p, [](const T*){});
    }
    return nullptr;
  }

  // True iff the carried payload is a T.
  template<typename T>
  bool is_a() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::is_a<T>: T is not in TypeList.");
    return unpack<T>() != nullptr;
  }

  // -- Discriminator + state ---------------------------------------------
  int32_t kind() const {
    return inner_.has_value() ? inner_->kind() : 0;
  }

  bool has_value() const noexcept { return inner_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  // Identity comparison.  For empty envelopes: equal iff both empty.
  // For non-empty: equal iff they wrap proxies whose indirect
  // accessor refers to the same instance — i.e., same underlying T*
  // via the proxy library's indirect_rtti dispatch.  Two
  // `pack_aliased(sp)` envelopes copied from the same source share
  // a shared_ptr<T> and thus compare equal; `pack(v)` copies own
  // their own T and compare unequal.
  bool operator==(const SerializableEnvelope& other) const noexcept {
    if (!inner_.has_value() && !other.inner_.has_value()) return true;
    if (!inner_.has_value() || !other.inner_.has_value()) return false;
    return &*inner_ == &*other.inner_;
  }
  bool operator!=(const SerializableEnvelope& other) const noexcept {
    return !(*this == other);
  }

  // -- Wire ops ----------------------------------------------------------
  // Wire format: [v32 kind] [payload bytes].  Same as MarshallDeputy
  // post-L9.
  void save(BinaryWriteArchive& ar) const {
    verify(has_value() &&
           "SerializableEnvelope::save: empty envelope cannot be encoded.");
    ar << v32(inner_->kind());
    inner_->save(ar);
  }

  void load(BinaryReadArchive& ar) {
    v32 kind_v;
    ar >> kind_v;
    inner_ = SerializableRegistry::create(kind_v.get());
    inner_->load(ar);
    refresh_kind();
  }

 public:
  // Public `kind_` field — cached snapshot of `inner_->kind()`.
  // Refreshed by every state-changing entry point.  Lets
  // `cmd.kind_ == X` direct-field access patterns continue to compile.
  int32_t kind_{0};

 private:
  void refresh_kind() noexcept {
    kind_ = inner_.has_value() ? inner_->kind() : 0;
  }

  pro::proxy<SerializableFacade> inner_;
};

// Migration compat: `marshallable_cast<T>` overload for envelopes.
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

// Legacy `Marshal&` archive operators.  Wire format identical to the
// archive path: `[v32 kind] [payload bytes]`.  Used by procedure.cc
// TxReply / classic/tpc_command.cc TpcCommitCommand archive operators
// in the legacy RPC reply path that still drives `Marshal&`.
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
