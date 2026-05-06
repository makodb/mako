// `AnyMessage` runtime registry + wire hookup.
//
// Header: any_message.hpp.
//
// `AnyMessageRegistry`: name ↔ type_index ↔ factory map behind a
// SpinMutex. Mirrors the SerializableRegistry idiom — registrations
// at static-init, lookups during RPC dispatch.
//
// payload_ is now a value-
// typed `pro::proxy<SerializableFacade>` (no more `shared_ptr<
// Marshallable>`).  The factory returns a holder-shaped proxy
// matching `SerializableEnvelope`'s storage shape, so unpack
// semantics match.

#include "any_message.hpp"

#include <utility>

#include <rusty/hashmap.hpp>

#include "../base/threading.hpp"

namespace rrr {

// ---- Wire ops -------------------------------------------------------

void AnyMessage::save(BinaryWriteArchive& ar) const {
  // Wire format: [v64-len-prefixed string: type_name] [payload bytes].
  ar << type_name_;
  if (payload_.has_value()) {
    payload_->save(ar);
  }
}

void AnyMessage::load(BinaryReadArchive& ar) {
  ar >> type_name_;
  payload_ = AnyMessageRegistry::create(type_name_);
  verify(payload_.has_value() &&
         "AnyMessage::load: unknown type name on wire.  "
         "Did the sender register a type the receiver does not know?");
  payload_->load(ar);
}

// ---- AnyMessageRegistry ---------------------------------------------

namespace {

struct AnyMessageRegistryMap {
  rusty::HashMap<std::string, AnyMessageRegistry::Factory> by_name;
  // typeid::hash_code → name. We use uint64_t (the hash) rather than
  // std::type_index directly because rusty::HashMap requires the key
  // to satisfy our internal Hash trait; type_index → uint64_t keeps
  // the registry plumbing uniform with the rest of the codebase.
  // Collision risk is acceptable: registrations abort if a hash
  // collides with a different already-registered type.
  rusty::HashMap<size_t, std::string> name_by_type_hash;
};

// @safe - file-local SpinMutex around the registry. Constructed on
// first use; outlives all callers (function-local static).
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
  // Disallow overwriting a name that's already bound — same-name-
  // different-T is always a bug, and same-name-same-T is a
  // duplicate static-init line that can be silently shadowed in a
  // future refactor. Either way, abort loud.
  verify(guard->by_name.get(name).is_none() &&
         "AnyMessageRegistry: name already registered. "
         "Pick a unique name per polymorphic payload type.");
  // Multiple names per T is allowed (versioning / aliasing). The
  // name_by_type_hash map records only the FIRST-registered name —
  // that's the one `pack<T>` uses by default. Additional names can
  // be emitted via `pack_as(name, val)`.
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
  // The pointer is into the registry map; safe to return because the
  // registry is process-lifetime — we never erase entries except via
  // `clear_for_testing`, which is single-threaded.
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
