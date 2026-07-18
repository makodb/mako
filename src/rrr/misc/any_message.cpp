module;

#include <cstdint>
#include <cstdlib>

#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/result.hpp>
#include <rusty/rusty.hpp>  // rusty::Mutex (was transitively via SpinMutex/rrr.threading)

export module rrr.any_message;

import std;
import rusty;
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


struct AnyMessage;
using AnyMessageSp = std::shared_ptr<AnyMessage>;

// Hand-written backing free fns for the DSL save/load below (Marshal
// operator chains + shared_ptr deref). Defined in the impl namespace.
void anymessage_save(const AnyMessage& self, BinaryWriteArchive& ar);
void anymessage_load(AnyMessage& self, BinaryReadArchive& ar);

// The generic backing free fns must be DECLARED before the generated
// template methods below: `pack`/`pack_as` take only std::shared_ptr<T>
// arguments, so for a non-rrr T argument-dependent lookup never
// considers namespace rrr — ordinary lookup at the template definition
// point has to find these. (is_a/unpack pass `(*this)` and would be
// found by ADL, declared here anyway for uniformity.) Definitions
// (inline) follow the registry declarations below.
template <typename T> bool anymessage_is_a(const AnyMessage& self);
template <typename T> std::shared_ptr<T> anymessage_unpack(const AnyMessage& self);
template <typename T> AnyMessageSp anymessage_pack_as(std::string name, std::shared_ptr<T> val);
template <typename T> AnyMessageSp anymessage_pack(std::shared_ptr<T> val);

// `AnyMessage` — typed wire payload `[v64 type_name] [payload bytes]`.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * The `is_a(std::string_view)` overload and the `type_name()`
//     accessor are DROPPED — zero callers repo-wide, and a Rust impl
//     block cannot hold two fns named `is_a` anyway.
//   * The private 2-arg ctor is gone; the struct is a plain public
//     aggregate (`AnyMessage m;` default-construct sites in rcc_rpc.h
//     keep working — both members default-construct). pack_as builds
//     the aggregate directly.
//   * The generic methods delegate to anymessage_* template free fns
//     (found by ADL at instantiation, the clientconn_request_*
//     precedent); pack/pack_as spell the return as the AnyMessageSp
//     alias (same type as before).
#if RUSTYCPP_RUST
struct AnyMessage {
    type_name_: std::string,
    payload_: SerializableProxy,
}

impl AnyMessage {
    // Wire ops. The payload's bytes come from the inner T's
    // `save`/`load` via the proxy facade.
    fn save(&self, ar: &mut BinaryWriteArchive) {
        anymessage_save(self, ar)
    }

    fn load(&mut self, ar: &mut BinaryReadArchive) {
        anymessage_load(self, ar)
    }

    // True iff this AnyMessage carries a value of type T (i.e., the
    // wire-carried type_name matches T's registered name).
    fn is_a<T>(&self) -> bool {
        anymessage_is_a::<T>(self)
    }

    // Recover the typed payload. Returns nullptr if T is not the
    // carried type, or if T was never registered.
    fn unpack<T>(&self) -> std::shared_ptr<T> {
        anymessage_unpack::<T>(self)
    }

    // Build an AnyMessage holding `val` under an explicit `name`. The
    // name does NOT need to have been pre-registered — pack_as is the
    // escape hatch for ad-hoc names. The receiver still needs a
    // factory registered under the same name to deserialize.
    fn pack_as<T>(name: std::string, val: std::shared_ptr<T>) -> AnyMessageSp {
        anymessage_pack_as(name, val)
    }

    // Build an AnyMessage using T's registered name. Aborts via
    // verify() if T was not registered with `reg_any_message_as<T>(...)`.
    fn pack<T>(val: std::shared_ptr<T>) -> AnyMessageSp {
        anymessage_pack(val)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.message version=1 rust_sha256=0c7fa9020ff38127267d6cd2f38983829a15a401deeb803c274c7374497667c6*/
struct AnyMessage;

struct AnyMessage {
    std::string type_name_;
    SerializableProxy payload_;

    void save(BinaryWriteArchive& ar) const;
    void load(BinaryReadArchive& ar);
    template<typename T>
    bool is_a() const;
    template<typename T>
    std::shared_ptr<T> unpack() const;
    template<typename T>
    static AnyMessageSp pack_as(std::string name, std::shared_ptr<T> val);
    template<typename T>
    static AnyMessageSp pack(std::shared_ptr<T> val);
};


void AnyMessage::save(BinaryWriteArchive& ar) const {
    anymessage_save((*this), ar);
}

void AnyMessage::load(BinaryReadArchive& ar) {
    anymessage_load((*this), ar);
}

template<typename T>
bool AnyMessage::is_a() const {
    return anymessage_is_a<T>((*this));
}

template<typename T>
std::shared_ptr<T> AnyMessage::unpack() const {
    return anymessage_unpack<T>((*this));
}

template<typename T>
AnyMessageSp AnyMessage::pack_as(std::string name, std::shared_ptr<T> val) {
    return anymessage_pack_as(std::move(name), std::move(val));
}

template<typename T>
AnyMessageSp AnyMessage::pack(std::shared_ptr<T> val) {
    return anymessage_pack(std::move(val));
}
/*RUSTYCPP:GEN-END id=any_message.message*/

// Runtime registry: maps registered type-name string → factory and
// std::type_index → registered name. Stored behind a rusty::Mutex
// (registrations run at static init time, lookups are concurrent
// across reactor threads during RPC dispatch).
// @safe - see file header. `name_for_type` returns a raw
// `const std::string*` into the rusty::Mutex-owned HashMap; that
// method and its caller `AnyMessage::is_a<T>` / `AnyMessage::pack<T>`
// carry per-method `// @unsafe`.
// `any_message_registry` was a class with only static methods + a
// public Factory typedef and no fields. Converted to a namespace so
// the inventory reflects what it actually is — a namespace-scoped
// API over the file-static rusty::Mutex<AnyMessageRegistryMap> below.
namespace any_message_registry {

// rusty::Function is move-only; the registry stores each factory by
// move and invokes it under the registry's rusty::Mutex inside `create()`.
using Factory = rusty::Function<SerializableProxy()>;

// Register `T` under `name`. Returns 0 so it can sit at namespace
// scope as a static-initializer return value:
//   static int _reg = any_message_registry::register_type(...);
// Aborts if `name` is already registered to a different type, or
// if `T` is already registered under a different name.
int register_type(std::string name,
                  std::type_index ti,
                  Factory factory);

// Create a fresh payload proxy for the given name. Returns an
// empty proxy if the name is not registered.
SerializableProxy create(const std::string& name);

// Look up the registered name for type `ti`. Returns nullptr if
// the type was not registered.
const std::string* name_for_type(std::type_index ti);

bool is_registered_name(const std::string& name);
bool is_registered_type(std::type_index ti);

// Test helper: clear the registry. Not thread-safe; use only
// between tests in single-threaded fixtures.
void clear_for_testing();

}  // namespace any_message_registry

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
  return any_message_registry::register_type(std::move(name),
                                           std::type_index(typeid(T)),
                                           std::move(factory));
}

// ---- Inlines that rely on the registry ------------------------------

// @unsafe - dereferences raw `const std::string*` returned by
// any_message_registry::name_for_type.
template <typename T>
inline bool anymessage_is_a(const AnyMessage& self) {
  const std::string* name = any_message_registry::name_for_type(
      std::type_index(typeid(T)));
  if (name == nullptr) return false;
  return self.type_name_ == *name;
}

// @unsafe - dynamic_cast through `payload_.get()` returning raw `T*`.
template <typename T>
inline std::shared_ptr<T> anymessage_unpack(const AnyMessage& self) {
  if (!anymessage_is_a<T>(self)) return nullptr;
  if (!self.payload_) return nullptr;
  if (auto* h = dynamic_cast<details::SerializableSharedPtrHolder<T>*>(
          self.payload_.get())) {
    return h->ptr;
  }
  return nullptr;
}

// @unsafe - aggregate-constructs the AnyMessage into a shared_ptr.
template <typename T>
inline AnyMessageSp anymessage_pack_as(std::string name,
                                       std::shared_ptr<T> val) {
  verify(val != nullptr);
  auto payload = std::make_shared<details::SerializableSharedPtrHolder<T>>(
      std::move(val));
  return std::make_shared<AnyMessage>(
      AnyMessage{std::move(name), std::move(payload)});
}

// @unsafe - dereferences raw `const std::string*` from name_for_type
// and forwards to the @unsafe anymessage_pack_as.
template <typename T>
inline AnyMessageSp anymessage_pack(std::shared_ptr<T> val) {
  const std::string* name = any_message_registry::name_for_type(
      std::type_index(typeid(T)));
  verify(name != nullptr &&
         "AnyMessage::pack<T>: T not registered. "
         "Call reg_any_message_as<T>(\"name\") at static init.");
  return anymessage_pack_as<T>(*name, std::move(val));
}

// ---- Free archive operators -----------------------------------------

// Phase 8 batch 4 (endgame straggler): serde free functions own the
// AnyMessage wire format; the operators are forwarders kept until the
// operator layer is deleted.
// @unsafe - forwards to `am.save(ar)` which drives a Marshal
// operator<< chain.
inline void serialize(const AnyMessage& am, BinaryWriteArchive& ar) {
  am.save(ar);
}

inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar,
                                      const AnyMessage& am) {
  serialize(am, ar);
  return ar;
}

// @unsafe - forwards to `am.load(ar)` which drives a Marshal
// operator>> chain.
inline void deserialize(AnyMessage& am, BinaryReadArchive& ar) {
  am.load(ar);
}

inline BinaryReadArchive& operator>>(BinaryReadArchive& ar,
                                     AnyMessage& am) {
  deserialize(am, ar);
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
void anymessage_save(const AnyMessage& self, BinaryWriteArchive& ar) {
  rrr::Serialize_::serialize(self.type_name_, ar);
  if (self.payload_) {
    self.payload_->save(ar);
  }
}

// @unsafe - `ar >> type_name_` Marshal operator>> chain + raw
// shared_ptr deref to call payload_->load.
void anymessage_load(AnyMessage& self, BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(self.type_name_, ar);
  self.payload_ = any_message_registry::create(self.type_name_);
  verify(self.payload_ &&
         "AnyMessage::load: unknown type name on wire.  "
         "Did the sender register a type the receiver does not know?");
  self.payload_->load(ar);
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
    by_name: rusty::HashMap<std::string, any_message_registry::Factory>,
    name_by_type_hash: rusty::HashMap<usize, std::string>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.1 version=1 rust_sha256=f0e25eabe80e818d24f9dadec699373e8c69ffb0471bf45f49fd33840ee8923c*/
struct AnyMessageRegistryMap;

struct AnyMessageRegistryMap {
    rusty::HashMap<std::string, any_message_registry::Factory> by_name;
    rusty::HashMap<size_t, std::string> name_by_type_hash;
};
/*RUSTYCPP:GEN-END id=any_message.1*/

rusty::Mutex<AnyMessageRegistryMap>& registry() {
  // rusty::Mutex has no default ctor (unlike the retired SpinMutex), so seed
  // it with an empty registry map explicitly.
  static rusty::Mutex<AnyMessageRegistryMap> r{AnyMessageRegistryMap{}};
  return r;
}

}  // namespace

// @unsafe - rusty::Mutex::lock().unwrap() + HashMap::get / contains_key /
// insert pattern not yet recognized as @safe here (annotation
// discovery limitation across the AnyMessageRegistryMap struct).
int any_message_registry::register_type(std::string name,
                                      std::type_index ti,
                                      Factory factory) {
  auto guard = registry().lock().unwrap();
  size_t hash = ti.hash_code();
  verify((*guard).by_name.get(name).is_none() &&
         "AnyMessageRegistry: name already registered.");
  if ((*guard).name_by_type_hash.get(hash).is_none()) {
    (*guard).name_by_type_hash.insert(hash, name);
  }
  (*guard).by_name.insert(std::move(name), std::move(factory));
  return 0;
}

// @unsafe - rusty::Mutex::lock().unwrap() + HashMap::get + invocation
// through `*entry.unwrap()` (Option-of-pointer deref).
SerializableProxy any_message_registry::create(const std::string& name) {
  auto guard = registry().lock().unwrap();
  auto entry = (*guard).by_name.get(name);
  if (entry.is_none()) return SerializableProxy{};
  return entry.unwrap()();
}

// @unsafe - returns a raw `const std::string*` into the rusty::Mutex-
// owned HashMap. Callers must not outlive the guard's borrow window;
// in practice each caller dereferences immediately and discards.
const std::string* any_message_registry::name_for_type(std::type_index ti) {
  auto guard = registry().lock().unwrap();
  size_t hash = ti.hash_code();
  auto entry = (*guard).name_by_type_hash.get(hash);
  if (entry.is_none()) return nullptr;
  return &entry.unwrap();
}

// @unsafe - rusty::Mutex::lock().unwrap() + HashMap::get + Option::is_some.
bool any_message_registry::is_registered_name(const std::string& name) {
  auto guard = registry().lock().unwrap();
  return (*guard).by_name.get(name).is_some();
}

// @unsafe - same pattern as is_registered_name.
bool any_message_registry::is_registered_type(std::type_index ti) {
  auto guard = registry().lock().unwrap();
  return (*guard).name_by_type_hash.get(ti.hash_code()).is_some();
}

// @unsafe - rusty::Mutex::lock().unwrap() + HashMap::clear().
void any_message_registry::clear_for_testing() {
  auto guard = registry().lock().unwrap();
  (*guard).by_name.clear();
  (*guard).name_by_type_hash.clear();
}

}  // namespace rrr
