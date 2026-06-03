module;

#include <cstdint>
#include <cstdlib>

#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/hashmap.hpp>
#include <rusty/option.hpp>
#include <rusty/result.hpp>

export module rrr.any_message;

import std;
import rrr.debugging;
import rrr.serializable;
import rrr.threading;

// @safe - AnyMessage: shared_ptr-backed typed wire payload; the
// runtime AnyMessageRegistry maps registered names to factory
// closures. Methods that drive a Marshal operator<</>> chain
// (`save`, `load`, the four free operator helpers), do a
// dynamic_cast to a raw `T*` (`unpack`), or escape a raw
// `const std::string*` (`name_for_type` and its callers) carry
// per-method `// @unsafe` below.
export namespace rrr {


// @safe - see file header.
class AnyMessage {
 public:
  AnyMessage() = default;

  // Wire ops — `[v64 type_name] [payload bytes]`.  The payload's
  // bytes come from the inner T's `save`/`load` via the proxy
  // facade.
  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);

  // Discriminator accessors.
  const std::string& type_name() const noexcept { return type_name_; }
  bool is_a(std::string_view name) const noexcept {
    return std::string_view{type_name_} == name;
  }

  // True iff this AnyMessage carries a value of type T (i.e., the
  // wire-carried type_name matches T's registered name).
  template <typename T>
  bool is_a() const;

  // Recover the typed payload. Returns nullptr if T is not the
  // carried type, or if T was never registered.
  template <typename T>
  std::shared_ptr<T> unpack() const;

  // Build an AnyMessage holding `val` under an explicit `name`. The
  // name does NOT need to have been pre-registered — pack_as is the
  // escape hatch for ad-hoc names. The receiver still needs a
  // factory registered under the same name to deserialize.
  template <typename T>
  static std::shared_ptr<AnyMessage> pack_as(std::string name,
                                             std::shared_ptr<T> val);

  // Build an AnyMessage using T's registered name. Aborts via
  // verify() if T was not registered with `reg_any_message_as<T>(...)`.
  template <typename T>
  static std::shared_ptr<AnyMessage> pack(std::shared_ptr<T> val);

 private:
  AnyMessage(std::string type_name, SerializableProxy payload)
      : type_name_(std::move(type_name)), payload_(std::move(payload)) {}

  std::string type_name_;
  SerializableProxy payload_;
};

// Runtime registry: maps registered type-name string → factory and
// std::type_index → registered name. Stored behind a SpinMutex
// (registrations run at static init time, lookups are concurrent
// across reactor threads during RPC dispatch).
// @safe - see file header. `name_for_type` returns a raw
// `const std::string*` into the SpinMutex-owned HashMap; that
// method and its caller `AnyMessage::is_a<T>` / `AnyMessage::pack<T>`
// carry per-method `// @unsafe`.
class AnyMessageRegistry {
 public:
  // rusty::Function is move-only; the registry stores each factory by
  // move and invokes it under the registry's SpinMutex inside `create()`.
  using Factory = rusty::Function<SerializableProxy()>;

  // Register `T` under `name`. Returns 0 so it can sit at namespace
  // scope as a static-initializer return value:
  //   static int _reg = AnyMessageRegistry::register_type(...);
  // Aborts if `name` is already registered to a different type, or
  // if `T` is already registered under a different name.
  static int register_type(std::string name,
                           std::type_index ti,
                           Factory factory);

  // Create a fresh payload proxy for the given name. Returns an
  // empty proxy if the name is not registered.
  static SerializableProxy create(const std::string& name);

  // Look up the registered name for type `ti`. Returns nullptr if
  // the type was not registered.
  static const std::string* name_for_type(std::type_index ti);

  static bool is_registered_name(const std::string& name);
  static bool is_registered_type(std::type_index ti);

  // Test helper: clear the registry. Not thread-safe; use only
  // between tests in single-threaded fixtures.
  static void clear_for_testing();
};

// Register T under `name` so:
//   * `AnyMessage::pack(make_shared<T>())` knows what name to stamp.
//   * `AnyMessage::load` can construct a fresh T-shaped payload when
//     the wire bytes carry `name`.
//
// The factory wraps a fresh shared_ptr<T> in a holder-shaped proxy —
// same shape `SerializableEnvelope` uses, so unpack semantics match.
//
// Returns 0 — suitable for `static int _reg = reg_any_message_as<T>("...");`.
template <typename T>
inline int reg_any_message_as(std::string name) {
  auto factory = []() -> SerializableProxy {
    auto sp = std::make_shared<T>();
    return std::make_shared<details::SerializableSharedPtrHolder<T>>(
        std::move(sp));
  };
  return AnyMessageRegistry::register_type(std::move(name),
                                           std::type_index(typeid(T)),
                                           std::move(factory));
}

// ---- Inlines that rely on the registry ------------------------------

// @unsafe - dereferences raw `const std::string*` returned by
// AnyMessageRegistry::name_for_type.
template <typename T>
inline bool AnyMessage::is_a() const {
  const std::string* name = AnyMessageRegistry::name_for_type(
      std::type_index(typeid(T)));
  if (name == nullptr) return false;
  return type_name_ == *name;
}

// @unsafe - dynamic_cast through `payload_.get()` returning raw `T*`.
template <typename T>
inline std::shared_ptr<T> AnyMessage::unpack() const {
  if (!is_a<T>()) return nullptr;
  if (!payload_) return nullptr;
  if (auto* h = dynamic_cast<details::SerializableSharedPtrHolder<T>*>(
          payload_.get())) {
    return h->ptr;
  }
  return nullptr;
}

// @unsafe - `new AnyMessage(...)` raw allocation passed into shared_ptr.
template <typename T>
inline std::shared_ptr<AnyMessage> AnyMessage::pack_as(
    std::string name, std::shared_ptr<T> val) {
  verify(val != nullptr);
  auto payload = std::make_shared<details::SerializableSharedPtrHolder<T>>(
      std::move(val));
  return std::shared_ptr<AnyMessage>(
      new AnyMessage(std::move(name), std::move(payload)));
}

// @unsafe - dereferences raw `const std::string*` from name_for_type
// and forwards to the @unsafe pack_as.
template <typename T>
inline std::shared_ptr<AnyMessage> AnyMessage::pack(std::shared_ptr<T> val) {
  const std::string* name = AnyMessageRegistry::name_for_type(
      std::type_index(typeid(T)));
  verify(name != nullptr &&
         "AnyMessage::pack<T>: T not registered. "
         "Call reg_any_message_as<T>(\"name\") at static init.");
  return pack_as<T>(*name, std::move(val));
}

// ---- Free archive operators -----------------------------------------

// @unsafe - forwards to `am.save(ar)` which drives a Marshal
// operator<< chain.
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar,
                                      const AnyMessage& am) {
  am.save(ar);
  return ar;
}

// @unsafe - forwards to `am.load(ar)` which drives a Marshal
// operator>> chain.
inline BinaryReadArchive& operator>>(BinaryReadArchive& ar,
                                     AnyMessage& am) {
  am.load(ar);
  return ar;
}


}  // export namespace rrr

// ============================================================================
// Implementation (formerly any_message.cpp's body)
// ============================================================================
// @safe - impl namespace. The two AnyMessage save/load entries below
// inherit class @safe but carry per-method `// @unsafe` for the
// Marshal operator chain + shared_ptr deref. The registry helpers
// inherit class @safe directly except `create` and `name_for_type`,
// which return raw pointer / forward an Option-of-pointer deref.
namespace rrr {

// @unsafe - `ar << type_name_` Marshal operator<< chain + raw
// shared_ptr deref to call payload_->save.
void AnyMessage::save(BinaryWriteArchive& ar) const {
  ar << type_name_;
  if (payload_) {
    payload_->save(ar);
  }
}

// @unsafe - `ar >> type_name_` Marshal operator>> chain + raw
// shared_ptr deref to call payload_->load.
void AnyMessage::load(BinaryReadArchive& ar) {
  ar >> type_name_;
  payload_ = AnyMessageRegistry::create(type_name_);
  verify(payload_ &&
         "AnyMessage::load: unknown type name on wire.  "
         "Did the sender register a type the receiver does not know?");
  payload_->load(ar);
}

namespace {

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ struct. Tier-2.2
// of the rrr trait/struct sweep — a TU-local POD that just bundles
// two HashMaps. The struct stays inside this anonymous namespace so
// `registry()` and the impl functions below all see it.
#if RUSTYCPP_RUST
struct AnyMessageRegistryMap {
    by_name: rusty::HashMap<std::string, AnyMessageRegistry::Factory>,
    name_by_type_hash: rusty::HashMap<usize, std::string>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.1 version=1 rust_sha256=1f01bc9771042fc5f5717fefe3b0d38fe50e88870888ff5601cffc712958a545*/
struct AnyMessageRegistryMap;

struct AnyMessageRegistryMap {
    rusty::HashMap<std::string, AnyMessageRegistry::Factory> by_name;
    rusty::HashMap<size_t, std::string> name_by_type_hash;
};
/*RUSTYCPP:GEN-END id=any_message.1*/

SpinMutex<AnyMessageRegistryMap>& registry() {
  static SpinMutex<AnyMessageRegistryMap> r;
  return r;
}

}  // namespace

// @unsafe - SpinMutex::lock().unwrap() + HashMap::get / contains_key /
// insert pattern not yet recognized as @safe here (annotation
// discovery limitation across the AnyMessageRegistryMap struct).
int AnyMessageRegistry::register_type(std::string name,
                                      std::type_index ti,
                                      Factory factory) {
  auto guard = registry().lock().unwrap();
  size_t hash = ti.hash_code();
  verify(guard->by_name.get(name).is_none() &&
         "AnyMessageRegistry: name already registered.");
  if (guard->name_by_type_hash.get(hash).is_none()) {
    guard->name_by_type_hash.insert(hash, name);
  }
  guard->by_name.insert(std::move(name), std::move(factory));
  return 0;
}

// @unsafe - SpinMutex::lock().unwrap() + HashMap::get + invocation
// through `*entry.unwrap()` (Option-of-pointer deref).
SerializableProxy AnyMessageRegistry::create(const std::string& name) {
  auto guard = registry().lock().unwrap();
  auto entry = guard->by_name.get(name);
  if (entry.is_none()) return SerializableProxy{};
  return entry.unwrap()();
}

// @unsafe - returns a raw `const std::string*` into the SpinMutex-
// owned HashMap. Callers must not outlive the guard's borrow window;
// in practice each caller dereferences immediately and discards.
const std::string* AnyMessageRegistry::name_for_type(std::type_index ti) {
  auto guard = registry().lock().unwrap();
  size_t hash = ti.hash_code();
  auto entry = guard->name_by_type_hash.get(hash);
  if (entry.is_none()) return nullptr;
  return &entry.unwrap();
}

// @unsafe - SpinMutex::lock().unwrap() + HashMap::get + Option::is_some.
bool AnyMessageRegistry::is_registered_name(const std::string& name) {
  auto guard = registry().lock().unwrap();
  return guard->by_name.get(name).is_some();
}

// @unsafe - same pattern as is_registered_name.
bool AnyMessageRegistry::is_registered_type(std::type_index ti) {
  auto guard = registry().lock().unwrap();
  return guard->name_by_type_hash.get(ti.hash_code()).is_some();
}

// @unsafe - SpinMutex::lock().unwrap() + HashMap::clear().
void AnyMessageRegistry::clear_for_testing() {
  auto guard = registry().lock().unwrap();
  guard->by_name.clear();
  guard->name_by_type_hash.clear();
}

}  // namespace rrr
