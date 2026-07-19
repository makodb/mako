module;

#include <cstdint>

export module rrr.serializable_envelope;

import std;
import rrr.basetypes;
import rrr.debugging;
import rrr.marshal;
import rrr.serializable;

// @safe - SerializableEnvelope: shared_ptr-backed sum type carried
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
    inner_ = std::make_shared<details::SerializableSharedPtrHolder<T>>(
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
    inner_ = std::make_shared<details::SerializableSharedPtrHolder<T>>(
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
    env.inner_ = std::make_shared<details::SerializableSharedPtrHolder<T>>(
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
    env.inner_ = std::make_shared<details::SerializableSharedPtrHolder<T>>(
        std::move(sp));
    env.refresh_kind();
    return env;
  }

  // -- Typed accessors ---------------------------------------------------
  // Recover the carried payload as a `T*`, or nullptr if the carried
  // type is not T (or the envelope is empty). Aliases the envelope-
  // owned T.
  // @unsafe - dynamic_cast through `inner_.get()` returning raw `T*`.
  template<typename T>
  T* unpack() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (!inner_) return nullptr;
    if (auto* h = dynamic_cast<details::SerializableSharedPtrHolder<T>*>(inner_.get())) {
      return h->ptr.get();
    }
    if constexpr (std::is_base_of_v<SerializableBase, T>) {
      if (auto* p = dynamic_cast<T*>(inner_.get())) return p;
    }
    return nullptr;
  }

  // @unsafe - dynamic_cast through `inner_.get()` returning raw `const T*`.
  template<typename T>
  const T* unpack() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack<T>: T is not in TypeList.");
    if (!inner_) return nullptr;
    if (auto* h = dynamic_cast<const details::SerializableSharedPtrHolder<T>*>(inner_.get())) {
      return h->ptr.get();
    }
    if constexpr (std::is_base_of_v<SerializableBase, T>) {
      if (auto* p = dynamic_cast<const T*>(inner_.get())) return p;
    }
    return nullptr;
  }

  // Recover the carried payload as `shared_ptr<T>`.
  //   * For `pack_aliased`-constructed envelopes: returns the original
  //     shared_ptr<T> (refcount-shared with the caller's pointer).
  //   * For `pack`-constructed envelopes: returns a `shared_ptr<T>`
  //     with a no-op deleter — the pointer aliases the envelope-owned
  //     T and the caller is responsible for keeping the envelope alive.
  // @unsafe - dynamic_cast + raw `T*` lambda-deleter shared_ptr build.
  template<typename T>
  std::shared_ptr<T> unpack_shared() {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    if (!inner_) return nullptr;
    if (auto* h = dynamic_cast<details::SerializableSharedPtrHolder<T>*>(inner_.get())) {
      return h->ptr;
    }
    if constexpr (std::is_base_of_v<SerializableBase, T>) {
      if (auto* p = dynamic_cast<T*>(inner_.get())) {
        // No-op deleter: caller responsibility for envelope lifetime.
        return std::shared_ptr<T>(p, [](T*){});
      }
    }
    return nullptr;
  }

  // @unsafe - dynamic_cast + raw `const T*` lambda-deleter shared_ptr build.
  template<typename T>
  std::shared_ptr<const T> unpack_shared() const {
    static_assert(TypeList::template contains<T>(),
                  "SerializableEnvelope::unpack_shared<T>: T is not in TypeList.");
    if (!inner_) return nullptr;
    if (auto* h = dynamic_cast<const details::SerializableSharedPtrHolder<T>*>(inner_.get())) {
      return std::const_pointer_cast<const T>(h->ptr);
    }
    if constexpr (std::is_base_of_v<SerializableBase, T>) {
      if (auto* p = dynamic_cast<const T*>(inner_.get())) {
        return std::shared_ptr<const T>(p, [](const T*){});
      }
    }
    return nullptr;
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
    return inner_ ? inner_->kind() : 0;
  }

  bool has_value() const noexcept { return static_cast<bool>(inner_); }
  explicit operator bool() const noexcept { return has_value(); }

  // Identity comparison.  For empty envelopes: equal iff both empty.
  // For non-empty: equal iff they wrap the same SerializableBase
  // instance — i.e., copies sharing the same shared_ptr refcount.
  // Two `pack_aliased(sp)` envelopes copied from the same source
  // share a holder and thus compare equal; `pack(v)` copies own
  // their own holder and compare unequal.
  bool operator==(const SerializableEnvelope& other) const noexcept {
    if (!inner_ && !other.inner_) return true;
    if (!inner_ || !other.inner_) return false;
    return inner_.get() == other.inner_.get();
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
    rrr::Serialize_::serialize(v32(inner_->kind()), ar);
    inner_->save(ar);
  }

  // @unsafe - Marshal `operator>>` chain on the binary archive.
  void load(BinaryReadArchive& ar) {
    v32 kind_v;
    rrr::Deserialize_::deserialize(kind_v, ar);
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
    kind_ = inner_ ? inner_->kind() : 0;
  }

  SerializableProxy inner_;
};

// Migration compat: `marshallable_cast<T>` overload for envelopes.
template<typename T, typename TypeList>
inline std::shared_ptr<T> marshallable_cast(
    SerializableEnvelope<TypeList>& env) {
  return env.template unpack_shared<T>();
}

// @unsafe - `const_cast<SerializableEnvelope&>(env)` to call the
// non-const `unpack_shared`.
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
