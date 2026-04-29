#pragma once

// Workstream N Phase 3b — Marshallable ↔ Serializable adapters.
//
// During Phase 4 we migrate per-command-type implementations from the
// old `Marshallable` (virtual to_marshal/from_marshal) interface to
// the new `Serializable` (save/load/kind via SerializableProxy)
// interface. While the migration is in flight, `MarshallDeputy`
// callers must keep working — some hold pointers to Marshallable,
// others want to hand bytes to a SerializableProxy-consuming API.
//
// This header provides two adapters and a couple of free helpers:
//
//   SerializableMarshallableAdapter  Wraps a SerializableProxy and
//                                    presents it as a Marshallable
//                                    (save→to_marshal, load→from_marshal).
//                                    Uses the Phase 3a Marshal↔Archive
//                                    bridges; both directions work.
//
//   MarshallableSerializableAdapter  Wraps a shared_ptr<Marshallable>
//                                    and presents it as a Serializable.
//                                    SAVE works (drains via a temp
//                                    Marshal); LOAD aborts (the
//                                    Marshal-streaming model can't be
//                                    inverted on demand). Phase 4
//                                    work flips command types to be
//                                    Serializable directly, so the
//                                    load direction here doesn't
//                                    need to be supported.
//
// Free helpers:
//   as_serializable(shared_ptr<Marshallable>) -> SerializableProxy
//       (save-only; aborts on load)
//   as_marshallable(SerializableProxy) -> shared_ptr<Marshallable>
//       (full bidirectional via 3a bridges)
//   as_serializable(const MarshallDeputy&) -> SerializableProxy
//       (save-only view of the deputy's contents)

#include <memory>
#include <utility>

#include "../base/all.hpp"
#include "marshal.hpp"
#include "marshal_archive.hpp"

namespace rrr {

// ---- Serializable → Marshallable adapter -----------------------------
//
// Inherits Marshallable. Holds a SerializableProxy. Forwards
// to_marshal/from_marshal through the Phase 3a Marshal bridges.
class SerializableMarshallableAdapter : public Marshallable {
 public:
  explicit SerializableMarshallableAdapter(SerializableProxy proxy)
      : Marshallable(proxy->kind()), serializable_(std::move(proxy)) {}

  // @safe - delegates through MarshalSink to the underlying
  // SerializableProxy's save method. Bytes flow:
  //   Marshallable::to_marshal(out) → SerializableMarshallableAdapter
  //   → MarshalSink(out) → BinaryWriteArchive → SerializableProxy::save
  Marshal& to_marshal(Marshal& out) const override {
    MarshalSink sink(&out);
    BinaryWriteArchive writer(&sink);
    serializable_->save(writer);
    return out;
  }

  // @safe - delegates through MarshalSource to the SerializableProxy's
  // load method. Bytes flow in the opposite direction.
  Marshal& from_marshal(Marshal& in) override {
    MarshalSource source(&in);
    BinaryReadArchive reader(&source);
    serializable_->load(reader);
    return in;
  }

  // Expose the inner SerializableProxy so callers can downcast to the
  // underlying T via `pro::proxy_cast<T*>(&adapter.proxy_mut())`. Used
  // by `serializable_cast<T>` below.
  SerializableProxy& proxy_mut() { return serializable_; }
  const SerializableProxy& proxy_view() const { return serializable_; }

 private:
  // mutable so the const-only `to_marshal` can call `serializable_->save`
  // through pro::proxy's non-const operator-> (the proxy is mutable
  // even when the underlying convention is const-qualified).
  mutable SerializableProxy serializable_;
};

// ---- Marshallable → Serializable adapter (save-only) ---------------
//
// Wraps a shared_ptr<Marshallable> and exposes it as something that
// satisfies SerializableFacade. SAVE works by encoding through a
// temporary Marshal and draining bytes into the Archive. LOAD aborts
// — the streaming Marshal model can't be inverted on demand without
// a length prefix, and Phase 4 will flip Marshallable types to
// Serializable directly rather than relying on this load path.
class MarshallableSerializableAdapter {
 public:
  explicit MarshallableSerializableAdapter(
      std::shared_ptr<Marshallable> m)
      : m_(std::move(m)) {}

  int32_t kind() const { return m_->kind(); }

  // @safe - drains a temporary Marshal produced by m_->to_marshal
  // into the archive. The temp Marshal lives only on the stack of
  // this call.
  void save(BinaryWriteArchive& ar) const {
    Marshal tmp;
    m_->to_marshal(tmp);
    char buf[4096];
    while (true) {
      size_t got = tmp.read(buf, sizeof(buf));
      if (got == 0) break;
      ar.write_bytes(buf, got);
    }
  }

  // Load is intentionally unsupported. Phase 4 migrations replace
  // Marshallable types with Serializable types directly; the
  // bridge's read direction goes through `as_marshallable` instead.
  void load(BinaryReadArchive& /*ar*/) {
    verify(false &&
           "MarshallableSerializableAdapter::load is unsupported. "
           "Migrate the underlying type to Serializable (Phase 4) and use "
           "SerializableRegistry::create + proxy->load directly.");
  }

 private:
  std::shared_ptr<Marshallable> m_;
};

// ---- Free helpers ----------------------------------------------------

// Wrap a Marshallable as a save-only SerializableProxy.
// Aborts if the Marshallable is null.
inline SerializableProxy as_serializable(
    std::shared_ptr<Marshallable> m) {
  verify(m != nullptr);
  return make_serializable_proxy<MarshallableSerializableAdapter>(
      std::move(m));
}

// Wrap a SerializableProxy as a shared_ptr<Marshallable>.
// Both save and load directions work via the Phase 3a bridges.
inline std::shared_ptr<Marshallable> as_marshallable(
    SerializableProxy proxy) {
  return std::make_shared<SerializableMarshallableAdapter>(
      std::move(proxy));
}

// Save-only SerializableProxy view of a MarshallDeputy's contents.
// Aborts if the deputy is empty.
inline SerializableProxy as_serializable(const MarshallDeputy& md) {
  auto inner = md.inner();
  verify(inner != nullptr);
  return as_serializable(std::move(inner));
}

// ---- MarshallDeputy ↔ Serializable registration --------------------
//
// Phase 4 prep: register a Serializable type T (one with `save`,
// `load`, `kind` methods but no Marshallable inheritance and no
// TypedMarshallableAdapter trait) under `kind` so that
// `MarshallDeputy::operator>>` can decode an instance of T from the
// wire. The factory creates a fresh T-backed SerializableProxy and
// wraps it as a Marshallable via `SerializableMarshallableAdapter`,
// which routes its `to_marshal` / `from_marshal` calls through the
// Phase 3a `MarshalSink` / `MarshalSource` bridges to T's `save` /
// `load`. T is therefore byte-for-byte indistinguishable on the wire
// from a Marshallable subclass implementing the same fields.
//
// Usage (mirrors `MarshallDeputy::reg_initializer<T>(kind)` for
// Marshallable types):
//
//   static int reg_my_cmd =
//       rrr::reg_serializable_in_deputy<MyCommand>(MyCommand::kKind);
//
// `MarshallDeputy(as_marshallable(make_serializable_proxy<T>()))` is
// the matching write-side call until a `MarshallDeputy(SerializableProxy)`
// constructor lands in Phase 3f.
template<typename T>
inline int reg_serializable_in_deputy(int32_t kind) {
  return MarshallDeputy::reg_initializer(kind, [kind]() {
    auto proxy = make_serializable_proxy<T>();
    verify(proxy->kind() == kind);
    auto marsh = as_marshallable(std::move(proxy));
    MarshallDeputy::MarInitializerState state;
    state.kind = kind;
    state.marshallable = marsh;
    state.proxy = std::make_shared<MarshallableProxy>(
        make_marshallable_proxy(marsh));
    return state;
  });
}

// Cast a `shared_ptr<Marshallable>` (typically from
// `MarshallDeputy::inner()`) back to the underlying Serializable type
// `T`. Returns nullptr if the value is null, or if it does not wrap a
// `T` via `SerializableMarshallableAdapter`.
//
// Phase 4 migrations replace `marshallable_cast<T>(md.inner())` with
// `serializable_cast<T>(md.inner())` for types moved from
// `Marshallable` to `Serializable`. The two coexist during the
// migration window: the old call site stays working for unmigrated
// types, the new one for migrated ones.
//
// Returns a raw `T*` rather than a `shared_ptr<T>` because the
// underlying T is owned by the proxy (heap-allocated inplace by
// `pro::make_proxy`), not by an external shared_ptr. The pointer is
// valid for the lifetime of the SerializableMarshallableAdapter
// instance — typically as long as the MarshallDeputy holding it.
template <typename T>
inline T* serializable_cast(const std::shared_ptr<Marshallable>& value) {
  if (value == nullptr) return nullptr;
  auto adapter = std::dynamic_pointer_cast<SerializableMarshallableAdapter>(
      value);
  if (adapter == nullptr) return nullptr;
  // `proxy_cast` is a friend function injected by
  // `pro::skills::indirect_rtti`. Found via ADL on the
  // proxy_indirect_accessor returned by `*proxy`. Returns nullptr if
  // the wrapped type is not T (no exception).
  return proxy_cast<T>(&*adapter->proxy_mut());
}

template <typename T>
inline T* serializable_cast(Marshallable* value) {
  if (value == nullptr) return nullptr;
  auto* adapter = dynamic_cast<SerializableMarshallableAdapter*>(value);
  if (adapter == nullptr) return nullptr;
  return proxy_cast<T>(&*adapter->proxy_mut());
}

template <typename T>
inline T* serializable_cast(const MarshallDeputy& md) {
  return serializable_cast<T>(md.inner());
}

}  // namespace rrr
