module;

#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>

export module rrr.serializable_envelope;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.serializable;

// @safe - SerializableEnvelope: rusty::Arc-backed sum type carried
// over the Marshal wire. The kind/has_value/operator bool/operator==
// accessors and `refresh_kind` are pure pointer-equality and integer
// reads. The dynamic_cast-heavy unpack family, `marshallable_cast`'s
// `const_cast` overload, and the four `operator<<` / `operator>>`
// archive entry points carry per-method `// @unsafe` (Marshal
// operator chains + raw `T*` returns).
export namespace rrr {


// @safe - see file header.
template<typename TypeList>
class SerializableEnvelope {
 public:
  SerializableEnvelope() = default;

  // Templated ctor for `Arc<T>` where T is in TypeList.  Stores an
  // aliased Arc<T> inside the proxy — `unpack_shared<T>()` returns a
  // refcount-shared copy of the same Arc<T>, and the underlying T
  // outlives any envelope copy via the Arc's refcount.
  template<typename T>
  SerializableEnvelope(rusty::Arc<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope(Arc<T>): T is not in TypeList. "
                  "Add T to the TypeList declaration.");
    inner_ = rusty::Option<SerializableProxy>(
        rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(
            std::move(sp)));
    refresh_kind();
  }

  // Templated assignment: same aliased-storage semantics.
  template<typename T>
  SerializableEnvelope& operator=(rusty::Arc<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::operator=(Arc<T>): T is not "
                  "in TypeList.");
    inner_ = rusty::Option<SerializableProxy>(
        rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(
            std::move(sp)));
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
    env.inner_ = rusty::Option<SerializableProxy>(
        rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(
            rusty::Arc<T>::make(value)));
    env.refresh_kind();
    return env;
  }

  // ALIASED: proxy retains the caller's `shared_ptr<T>`. Mutations
  // through the caller's pointer remain visible to the encoded
  // payload; `unpack<T>()` returns a pointer aliasing the same
  // instance.  `unpack_shared<T>()` returns a refcount-shared
  // shared_ptr<T>.
  template<typename T>
  static SerializableEnvelope pack_aliased(rusty::Arc<T> sp) {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::pack_aliased<T>: T is not in "
                  "TypeList.");
    SerializableEnvelope env;
    env.inner_ = rusty::Option<SerializableProxy>(
        rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(
            std::move(sp)));
    env.refresh_kind();
    return env;
  }

  // -- Typed accessors ---------------------------------------------------
  // INVARIANT: `inner_` is always holder-shaped — every construction
  // path above and every SerializableRegistry factory wraps the payload
  // in a SerializableSharedPtrHolder<T>; no proxy IS its payload
  // directly. The accessors below rely on this: one holder downcast,
  // no direct-SerializableBase fallback.
  // Recover the carried payload as a `T*`, or nullptr if the carried
  // type is not T (or the envelope is empty). Aliases the envelope-
  // owned T.
  // @unsafe - dynamic_cast through `inner_.get()` returning raw `T*`.
  template<typename T>
  T* unpack() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    // @unsafe - as_ptr(): mutable escape from the const-view Arc. The
    // non-const unpack keeps its historical T* contract; new code
    // should prefer the const overload or unpack_shared.
    if (auto* h = holder_of<T>()) {
      return const_cast<details::SerializableSharedPtrHolder<T>*>(h)
          ->ptr.as_ptr();
    }
    return nullptr;
  }

  // @unsafe - dynamic_cast through the held base pointer.
  template<typename T>
  const T* unpack() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (auto* h = holder_of<T>()) {
      return h->ptr.get();
    }
    return nullptr;
  }

  // Recover the carried payload as a refcount-shared `Arc<T>` (for
  // `pack_aliased` envelopes this shares the caller's original Arc;
  // for `pack` envelopes the envelope-owned copy). None on empty or
  // type mismatch. (Arc access is const-view by design, so a single
  // Option<Arc<T>> return serves both const and non-const callers —
  // no Arc<const T> variant.)
  // @unsafe - dynamic_cast through the held base pointer.
  template<typename T>
  rusty::Option<rusty::Arc<T>> unpack_shared() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    if (auto* h = holder_of<T>()) {
      return rusty::Option<rusty::Arc<T>>(h->ptr.clone());
    }
    return rusty::Option<rusty::Arc<T>>(rusty::None);
  }

  // @unsafe - dispatches to const-unpack which dynamic_casts to raw `const T*`.
  // True iff the carried payload is a T.
  template<typename T>
  bool is_a() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::is_a<T>: T is not in TypeList.");
    return unpack<T>() != nullptr;
  }

  // -- Discriminator + state ---------------------------------------------
  int32_t kind() const {
    return inner_.is_some() ? base_ptr()->kind() : 0;
  }

  bool has_value() const noexcept { return inner_.is_some(); }
  explicit operator bool() const noexcept { return has_value(); }

  // Identity comparison.  For empty envelopes: equal iff both empty.
  // For non-empty: equal iff they wrap the same SerializableBase
  // instance — i.e., copies sharing the same shared_ptr refcount.
  // Two `pack_aliased(sp)` envelopes copied from the same source
  // share a holder and thus compare equal; `pack(v)` copies own
  // their own holder and compare unequal.
  bool operator==(const SerializableEnvelope& other) const noexcept {
    if (inner_.is_none() && other.inner_.is_none()) return true;
    if (inner_.is_none() || other.inner_.is_none()) return false;
    return base_ptr() == other.base_ptr();
  }
  bool operator!=(const SerializableEnvelope& other) const noexcept {
    return !(*this == other);
  }

  // -- Wire ops ----------------------------------------------------------
  // Wire format: [v32 kind] [payload bytes].  Same as MarshallDeputy
  // post-L9.
  // @unsafe - Marshal `operator<<` chain on the binary archive.
  void save(BinaryWriteArchive& ar) const {
    verify(has_value() &&
           "SerializableEnvelope::save: empty envelope cannot be encoded.");
    rrr::Serialize_::serialize(v32(base_ptr()->kind()), ar);
    base_ptr()->save(ar);
  }

  // @unsafe - Marshal `operator>>` chain on the binary archive.
  void load(BinaryReadArchive& ar) {
    v32 kind_v;
    rrr::Deserialize_::deserialize(kind_v, ar);
    auto proxy = SerializableRegistry::create(kind_v.get());
    // @unsafe - unique-owner mutation window: the proxy is factory-
    // fresh (strong_count 1); a shared proxy here would panic loudly.
    proxy.get_mut().unwrap().load(ar);
    inner_ = rusty::Option<SerializableProxy>(std::move(proxy));
    refresh_kind();
  }

 public:
  // Public `kind_` field — cached snapshot of `inner_->kind()`.
  // Refreshed by every state-changing entry point.  Lets
  // `cmd.kind_ == X` direct-field access patterns continue to compile.
  int32_t kind_{0};

 private:
  void refresh_kind() noexcept {
    kind_ = inner_.is_some() ? base_ptr()->kind() : 0;
  }

  // Borrow the held base pointer (nullptr when empty). Const-only —
  // the Arc is const-view; mutation goes through the load() window.
  const SerializableBase* base_ptr() const noexcept {
    return inner_.is_some() ? std::as_const(inner_).unwrap().get() : nullptr;
  }

  // Downcast the held proxy to the holder for T (nullptr on miss).
  template<typename T>
  const details::SerializableSharedPtrHolder<T>* holder_of() const {
    return dynamic_cast<const details::SerializableSharedPtrHolder<T>*>(
        base_ptr());
  }

  rusty::Option<SerializableProxy> inner_{rusty::None};
};

// Migration compat: `marshallable_cast<T>` overload for envelopes.
// Returns Option<Arc<T>> — None on empty envelope / type mismatch.
// (unpack_shared is const now; the old const_cast overload collapsed.)
template<typename T, typename TypeList>
inline rusty::Option<rusty::Arc<T>> marshallable_cast(
    const SerializableEnvelope<TypeList>& env) {
  return env.template unpack_shared<T>();
}

template<typename T, typename TypeList>
inline rusty::Option<rusty::Arc<T>> marshallable_cast(
    SerializableEnvelope<TypeList>* env) {
  if (env == nullptr) return rusty::Option<rusty::Arc<T>>(rusty::None);
  return env->template unpack_shared<T>();
}

// Free archive operators — let SerializableEnvelope ride directly in
// rpcgen-emitted RPC struct fields the same way any other Serializable
// type does.
// Phase 8 batch 4: serde free functions own the envelope wire format; the
// operators below are forwarders kept until the operator layer is deleted.
// @unsafe - forwards to `env.save(ar)` which drives a Marshal
// operator<< chain.
template<typename TypeList>
inline void serialize(const SerializableEnvelope<TypeList>& env,
                      BinaryWriteArchive& ar) {
  env.save(ar);
}

template<typename TypeList>
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar,
                                      const SerializableEnvelope<TypeList>& env) {
  serialize(env, ar);
  return ar;
}

// @unsafe - forwards to `env.load(ar)` which drives a Marshal
// operator>> chain.
template<typename TypeList>
inline void deserialize(SerializableEnvelope<TypeList>& env,
                        BinaryReadArchive& ar) {
  env.load(ar);
}

template<typename TypeList>
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar,
                                     SerializableEnvelope<TypeList>& env) {
  deserialize(env, ar);
  return ar;
}

// Marshal-deprecation slice C: the legacy `Marshal&` envelope operators
// are deleted — the archive save/load path above is the only surface.


}  // export namespace rrr
