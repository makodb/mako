// Workstream N Phase 2 — SerializableRegistry implementation.
//
// The registry maps int32_t kind tags to factories that construct
// fresh `SerializableProxy` instances. Stored behind a SpinMutex
// because:
//   - Static-init-time registrations (`SerializableRegistry::reg<T>`)
//     can run from multiple translation units in unspecified order.
//   - Lookups during RPC dispatch are concurrent across reactor
//     threads.
//
// Lookups are read-mostly — registrations happen at static init time,
// before any RPC traffic. SpinMutex is fine for the low contention.

#include "marshal_archive.hpp"

#include <rusty/hashmap.hpp>

#include "../base/threading.hpp"

namespace rrr {

namespace {

struct SerializableRegistryMap {
  rusty::HashMap<int32_t, SerializableRegistry::Factory> map;
};

// @safe - file-local SpinMutex around the registry map. Constructed
// on first use; outlives all callers (function-local static).
SpinMutex<SerializableRegistryMap>& registry() {
  static SpinMutex<SerializableRegistryMap> r;
  return r;
}

}  // namespace

void SerializableRegistry::register_factory(int32_t kind, Factory factory) {
  auto guard = registry().lock().unwrap();
  guard->map.insert(kind, std::move(factory));
}

SerializableProxy SerializableRegistry::create(int32_t kind) {
  Factory f;
  {
    auto guard = registry().lock().unwrap();
    auto entry = guard->map.get(kind);
    verify(entry.is_some());
    f = *entry.unwrap();
  }
  return f();
}

bool SerializableRegistry::is_registered(int32_t kind) {
  auto guard = registry().lock().unwrap();
  return guard->map.get(kind).is_some();
}

void SerializableRegistry::clear_for_testing() {
  auto guard = registry().lock().unwrap();
  guard->map.clear();
}

}  // namespace rrr
