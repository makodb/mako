// Workstream N L7 — `AnyMessage` runtime registry + wire hookup.
//
// Header: any_message.hpp.
//
// `AnyMessageRegistry`: name ↔ type_index ↔ factory map behind a
// SpinMutex. Mirrors the SerializableRegistry idiom — registrations
// at static-init, lookups during RPC dispatch.
//
// Workstream N L10f-2 step 5 (2026-05-05): retired the
// `MarshallDeputy::reg_initializer<AnyMessage>(ANY_MESSAGE)` static-
// init line and the legacy `to_marshal`/`from_marshal` Marshallable
// methods.  AnyMessage now embeds directly in RPC struct fields via
// the Serializable-style `save`/`load` (and free archive operators
// in the header).

#include "any_message.hpp"

#include <utility>

#include <rusty/hashmap.hpp>

#include "../base/threading.hpp"

namespace rrr {

// ---- AnyMessage value-type ctor -------------------------------------

AnyMessage::AnyMessage(std::string type_name,
                       std::shared_ptr<Marshallable> payload)
    : type_name_(std::move(type_name)),
      payload_(std::move(payload)) {}

// ---- Wire ops -------------------------------------------------------

void AnyMessage::save(BinaryWriteArchive& ar) const {
  // Wire format inside save: [v64-len-prefixed string: type_name]
  // [payload bytes from payload_->to_marshal].  Identical to the
  // bytes inside `to_marshal`; the difference vs the deputy path is
  // only the absence of the surrounding kind tag (the C++ field type
  // discriminates statically here).
  ar << type_name_;
  if (payload_) {
    // Route the payload through a temporary `Marshal` so the inner
    // Marshallable's `to_marshal` (its only Serialize-shaped method
    // until L10f drops `Marshallable`) can do its work.  Drain the
    // temp into the archive byte-by-byte chunk.  One extra copy per
    // payload byte vs the eventual all-Serializable design — accepted
    // as a transitional cost during L10.
    Marshal tmp;
    payload_->to_marshal(tmp);
    char buf[4096];
    while (true) {
      size_t got = tmp.read(buf, sizeof(buf));
      if (got == 0) break;
      ar.write_bytes(buf, got);
    }
  }
}

void AnyMessage::load(BinaryReadArchive& ar) {
  ar >> type_name_;
  payload_ = AnyMessageRegistry::create(type_name_);
  verify(payload_ != nullptr &&
         "AnyMessage::load: unknown type name on wire.  "
         "Did the sender register a type the receiver does not know?");
  // The inner payload's `from_marshal` needs a `Marshal*` to read
  // from.  Recover the underlying Marshal from the archive's source
  // (which must be a `MarshalSource` — same restriction as the
  // `operator>>(BinaryReadArchive&, MarshallDeputy&)` bridge).  Then
  // delegate to the legacy from_marshal so the inner type's existing
  // logic stays unchanged.
  auto* mark_adapter =
      proxy_cast<MarshalSourceAdapter>(&*ar.source());
  verify(mark_adapter != nullptr &&
         "AnyMessage::load requires the archive's source to be a "
         "MarshalSource.  Wrap the wire bytes in a Marshal first; "
         "the streaming-from-arbitrary-source case needs a "
         "length-prefixed wire format which AnyMessage does not have "
         "at the payload-bytes layer.");
  Marshal* m = mark_adapter->source()->marshal();
  verify(m != nullptr);
  payload_->from_marshal(*m);
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

std::shared_ptr<Marshallable> AnyMessageRegistry::create(
    const std::string& name) {
  auto guard = registry().lock().unwrap();
  auto entry = guard->by_name.get(name);
  if (entry.is_none()) return nullptr;
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
