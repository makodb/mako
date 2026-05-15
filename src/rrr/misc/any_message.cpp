module;

#include <cstdint>
#include <cstdlib>

#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/hashmap.hpp>

export module rrr.any_message;

import std;
import rrr.debugging;
import rrr.serializable;
import rrr.threading;

export namespace rrr {


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

template <typename T>
inline bool AnyMessage::is_a() const {
  const std::string* name = AnyMessageRegistry::name_for_type(
      std::type_index(typeid(T)));
  if (name == nullptr) return false;
  return type_name_ == *name;
}

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

template <typename T>
inline std::shared_ptr<AnyMessage> AnyMessage::pack_as(
    std::string name, std::shared_ptr<T> val) {
  verify(val != nullptr);
  auto payload = std::make_shared<details::SerializableSharedPtrHolder<T>>(
      std::move(val));
  return std::shared_ptr<AnyMessage>(
      new AnyMessage(std::move(name), std::move(payload)));
}

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

inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar,
                                      const AnyMessage& am) {
  am.save(ar);
  return ar;
}

inline BinaryReadArchive& operator>>(BinaryReadArchive& ar,
                                     AnyMessage& am) {
  am.load(ar);
  return ar;
}


}  // export namespace rrr

// ============================================================================
// Implementation (formerly any_message.cpp's body)
// ============================================================================
namespace rrr {

void AnyMessage::save(BinaryWriteArchive& ar) const {
  ar << type_name_;
  if (payload_) {
    payload_->save(ar);
  }
}

void AnyMessage::load(BinaryReadArchive& ar) {
  ar >> type_name_;
  payload_ = AnyMessageRegistry::create(type_name_);
  verify(payload_ &&
         "AnyMessage::load: unknown type name on wire.  "
         "Did the sender register a type the receiver does not know?");
  payload_->load(ar);
}

namespace {

struct AnyMessageRegistryMap {
  rusty::HashMap<std::string, AnyMessageRegistry::Factory> by_name;
  rusty::HashMap<size_t, std::string> name_by_type_hash;
};

SpinMutex<AnyMessageRegistryMap>& registry() {
  static SpinMutex<AnyMessageRegistryMap> r;
  return r;
}

}  // namespace

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

SerializableProxy AnyMessageRegistry::create(const std::string& name) {
  auto guard = registry().lock().unwrap();
  auto entry = guard->by_name.get(name);
  if (entry.is_none()) return SerializableProxy{};
  return (*entry.unwrap())();
}

const std::string* AnyMessageRegistry::name_for_type(std::type_index ti) {
  auto guard = registry().lock().unwrap();
  size_t hash = ti.hash_code();
  auto entry = guard->name_by_type_hash.get(hash);
  if (entry.is_none()) return nullptr;
  return entry.unwrap();
}

bool AnyMessageRegistry::is_registered_name(const std::string& name) {
  auto guard = registry().lock().unwrap();
  return guard->by_name.get(name).is_some();
}

bool AnyMessageRegistry::is_registered_type(std::type_index ti) {
  auto guard = registry().lock().unwrap();
  return guard->name_by_type_hash.get(ti.hash_code()).is_some();
}

void AnyMessageRegistry::clear_for_testing() {
  auto guard = registry().lock().unwrap();
  guard->by_name.clear();
  guard->name_by_type_hash.clear();
}

}  // namespace rrr
