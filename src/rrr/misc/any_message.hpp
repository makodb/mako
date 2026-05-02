#pragma once

// Workstream N L7 — `AnyMessage`: open-set polymorphic envelope.
//
// Counterpart to the closed-set `TypeList` discriminant pattern in
// `marshal_serializable_bridge.hpp`. The two cover different polymorphism
// shapes:
//
//   Closed-set (TypeList + bincode-style int discriminant):
//     - User declares every payload type up-front in one TypeList.
//     - Wire tag is the type's 1-indexed position in the list (1 byte
//       in the eventual v32-encoded form).
//     - Adding a new type = append to the list. Reorder/remove = wire
//       break.
//     - For things like Command types where the receiver always knows
//       the universe of possible messages.
//
//   Open-set (AnyMessage + string type tag, this file):
//     - User registers each payload type under a string name.
//     - Wire tag is the variable-length string name.
//     - Adding a new type = independent — no central list to touch.
//     - For things like graph payloads (`RccGraph` / `EmptyGraph`)
//       where a service that doesn't know about a new graph type can
//       still deserialize the envelope and inspect / dispatch.
//
// Mirrors Rust's `typetag` crate / Protobuf's `google.protobuf.Any`:
// every value carries its own type identity on the wire, the receiver
// looks up a factory by the carried name, and downcasts to the concrete
// type for typed access.
//
// Wire format (when wrapped inside `MarshallDeputy`):
//   MarshallDeputy: [int32: ANY_MESSAGE] [AnyMessage::to_marshal bytes]
//   AnyMessage:     [v64-prefixed string: type_name] [payload bytes]
//
// The payload bytes come from the inner `Marshallable::to_marshal`
// (for Marshallable T) or the bridged `Save` of a `SerializableProxy`
// view (for Serializable T) — both produce the byte stream the
// receiver-side type's `from_marshal` / `Load` knows how to consume.
//
// Usage:
//
//   // 1) Register at static-init time (once per type, anywhere):
//   namespace janus {
//   static int _reg_rcc_graph =
//       rrr::reg_any_message_as<RccGraph>("janus.RccGraph");
//   }
//
//   // 2) Pack on the sender:
//   auto graph = std::make_shared<RccGraph>();
//   MarshallDeputy out(rrr::AnyMessage::pack(graph));
//
//   // 3) Unpack on the receiver:
//   if (auto am = rrr::AnyMessage::try_cast(md)) {
//     if (auto g = am->unpack<RccGraph>()) {
//       // ... use g ...
//     } else if (am->is_a<EmptyGraph>()) {
//       // ... empty case ...
//     }
//   }

#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include <rusty/fn.hpp>

#include "marshal.hpp"
#include "marshal_archive.hpp"
#include "marshal_serializable_bridge.hpp"

namespace rrr {

// `AnyMessage` is a Marshallable envelope. It registers itself once
// with MarshallDeputy under the fixed kind `MarshallDeputy::ANY_MESSAGE`
// — every AnyMessage payload, regardless of carried type, hits this
// kind on the wire. The actual type discriminator is the carried
// `type_name_` string inside the AnyMessage's bytes.
class AnyMessage : public Marshallable {
 public:
  AnyMessage();
  AnyMessage(std::string type_name, std::shared_ptr<Marshallable> payload);

  // Marshallable contract — the bytes flow through here when the
  // surrounding MarshallDeputy serializes / deserializes.
  Marshal& to_marshal(Marshal& m) const override;
  Marshal& from_marshal(Marshal& m) override;

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

  // Convenience: recover a shared_ptr<AnyMessage> from a deputy
  // that carries an AnyMessage envelope. Returns nullptr if the
  // deputy carries something else (kind != ANY_MESSAGE) or is empty.
  static std::shared_ptr<AnyMessage> try_cast(const MarshallDeputy& md);

  // Inner payload accessor — exposes the underlying Marshallable so
  // callers that need the raw bytes-source (rare) can reach it.
  const std::shared_ptr<Marshallable>& payload() const noexcept {
    return payload_;
  }

 private:
  std::string type_name_;
  std::shared_ptr<Marshallable> payload_;
};

// Runtime registry: maps registered type-name string → factory and
// std::type_index → registered name. Stored behind a SpinMutex
// (registrations run at static init time, lookups are concurrent
// across reactor threads during RPC dispatch).
class AnyMessageRegistry {
 public:
  // rusty::Function is move-only; the registry stores each factory by
  // move and invokes it under the registry's SpinMutex inside `create()`.
  using Factory = rusty::Function<std::shared_ptr<Marshallable>()>;

  // Register `T` under `name`. Returns 0 so it can sit at namespace
  // scope as a static-initializer return value:
  //   static int _reg = AnyMessageRegistry::register_type(...);
  // Aborts if `name` is already registered to a different type, or
  // if `T` is already registered under a different name.
  static int register_type(std::string name,
                           std::type_index ti,
                           Factory factory);

  // Create a fresh payload-Marshallable for the given name. Returns
  // nullptr if the name is not registered.
  static std::shared_ptr<Marshallable> create(const std::string& name);

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
//   * `AnyMessage::from_marshal` can construct a fresh T-shaped
//     payload when the wire bytes carry `name`.
//
// For Marshallable subclasses, the factory default-constructs T.
// For Serializable types (anything not deriving Marshallable that has
// the save/load/kind triplet), the factory default-constructs T inside
// a SerializableProxy, then wraps via `as_marshallable` so the AnyMessage
// payload is byte-compatible with the surrounding MarshallDeputy plumbing.
//
// Returns 0 — suitable for `static int _reg = reg_any_message_as<T>("...");`.
template <typename T>
inline int reg_any_message_as(std::string name) {
  auto factory = []() -> std::shared_ptr<Marshallable> {
    if constexpr (std::is_base_of_v<Marshallable, T>) {
      return std::make_shared<T>();
    } else {
      return as_marshallable(make_serializable_proxy<T>());
    }
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
  if (payload_ == nullptr) return nullptr;
  // marshallable_cast handles both Marshallable subclasses and
  // Serializable-via-bridge cases (the bridge overload routes through
  // SerializableMarshallableAdapter).
  return marshallable_cast<T>(payload_);
}

template <typename T>
inline std::shared_ptr<AnyMessage> AnyMessage::pack_as(
    std::string name, std::shared_ptr<T> val) {
  verify(val != nullptr);
  std::shared_ptr<Marshallable> payload;
  if constexpr (std::is_base_of_v<Marshallable, T>) {
    payload = std::static_pointer_cast<Marshallable>(std::move(val));
  } else {
    // Serializable — wrap aliased so mutations on the caller's
    // shared_ptr<T> remain visible to the encoded payload (matches
    // wrap_serializable_aliased semantics used elsewhere in the bridge).
    payload = wrap_serializable_aliased(std::move(val));
  }
  return std::make_shared<AnyMessage>(std::move(name), std::move(payload));
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

}  // namespace rrr
