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

// @safe - AnyMessage: rusty::Arc-backed typed wire payload; the
// runtime AnyMessageRegistry maps registered names to factory
// closures. Methods that drive a Marshal operator<</>> chain
// (`save`, `load`, the four free operator helpers), do a
// dynamic_cast to a raw `T*` (`unpack`), or escape a raw
// `const std::string*` (`name_for_type` and its callers) carry
// per-method `// @unsafe` below.
/*RUSTYCPP:GEN-DISPATCH-BEGIN*/
namespace rusty { namespace detail {
RUSTY_METHOD_DISPATCH(unwrap)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

export namespace rrr {


struct AnyMessage;

// The registry namespace is defined further down (it needs SerializableProxy
// and the Mutex-guarded map), but AnyMessage::load — DSL, above it — calls
// create(). Forward-declare just that one entry point.
using SerializableProxy = rusty::Arc<SerializableBase>;
namespace any_message_registry {
rusty::Option<SerializableProxy> create(const std::string& name);
}  // namespace any_message_registry

// The generic backing free fns must be DECLARED before the generated
// template methods below: `pack`/`pack_as` take only std::shared_ptr<T>
// arguments, so for a non-rrr T argument-dependent lookup never
// considers namespace rrr — ordinary lookup at the template definition
// point has to find these. (is_a/unpack pass `(*this)` and would be
// found by ADL, declared here anyway for uniformity.) Definitions
// (inline) follow the registry declarations below.
template <typename T> bool anymessage_is_a(const AnyMessage& self);
template <typename T> rusty::Option<rusty::Arc<T>> anymessage_unpack(const AnyMessage& self);
template <typename T> AnyMessage anymessage_pack_as(std::string name, rusty::Arc<T> val);
template <typename T> AnyMessage anymessage_pack(rusty::Arc<T> val);

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
//     precedent); pack/pack_as return AnyMessage BY VALUE — rcc_rpc.h
//     stores AnyMessage by value and every caller stored the result
//     into such a field, so the former shared_ptr return was one heap
//     allocation + immediate deref-copy per pack.
#if RUSTYCPP_RUST
struct AnyMessage {
    type_name_: std::string,
    payload_: Option<SerializableProxy>,
}

impl AnyMessage {
    // Wire ops. The payload's bytes come from the inner T's
    // `save`/`load` via the proxy facade.
    fn save(&self, ar: &mut BinaryWriteArchive) {
        rrr::Serialize_::serialize(self.type_name_, ar);
        if self.payload_.is_some() {
            // Named Arc type so the call lowers to `->save` (playbook §7.13):
            // the element type is lost through Option::as_ref().unwrap().
            let payload: &rusty::Arc<SerializableBase> = self.payload_.as_ref().unwrap();
            payload.save(ar);
        }
    }

    fn load(&mut self, ar: &mut BinaryReadArchive) {
        rrr::Deserialize_::deserialize(self.type_name_, ar);
        let proxy_opt = any_message_registry::create(self.type_name_);
        // The C++ wrote `verify(cond && "unknown type name on wire...")`.
        // That string is never printed — rrr's verify() reports only file
        // and line under NDEBUG, and asserts on `ok` otherwise — so it was
        // documentation for the reader, and stays that way here: an
        // unknown type name means the sender registered a type the
        // receiver does not know.
        verify(proxy_opt.is_some());
        let mut proxy: rusty::Arc<SerializableBase> = proxy_opt.unwrap();
        // Unique-owner mutation window: the proxy is factory-fresh, so
        // get_mut() hands back the &mut that Arc otherwise withholds.
        proxy.get_mut().unwrap().load(ar);
        self.payload_ = rusty::Some(proxy);
    }

    // True iff this AnyMessage carries a value of type T (i.e., the
    // wire-carried type_name matches T's registered name).
    fn is_a<T>(&self) -> bool {
        anymessage_is_a::<T>(self)
    }

    // Recover the typed payload. Returns nullptr if T is not the
    // carried type, or if T was never registered.
    fn unpack<T>(&self) -> Option<Arc<T>> {
        anymessage_unpack::<T>(self)
    }

    // Build an AnyMessage holding `val` under an explicit `name`. The
    // name does NOT need to have been pre-registered — pack_as is the
    // escape hatch for ad-hoc names. The receiver still needs a
    // factory registered under the same name to deserialize.
    fn pack_as<T>(name: std::string, val: Arc<T>) -> AnyMessage {
        anymessage_pack_as(name, val)
    }

    // Build an AnyMessage using T's registered name. Aborts via
    // verify() if T was not registered with `reg_any_message_as<T>(...)`.
    fn pack<T>(val: Arc<T>) -> AnyMessage {
        anymessage_pack(val)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.message version=1 rust_sha256=d50e06d531351fe40caa5cc28f7507e64ac6bf67c5fc332203e77a121cc8931c*/
struct AnyMessage;

struct AnyMessage {
    std::string type_name_;
    rusty::Option<SerializableProxy> payload_;

    void save(BinaryWriteArchive& ar) const;
    void load(BinaryReadArchive& ar);
    template<typename T>
    bool is_a() const;
    template<typename T>
    rusty::Option<rusty::Arc<T>> unpack() const;
    template<typename T>
    static AnyMessage pack_as(std::string name, rusty::Arc<T> val);
    template<typename T>
    static AnyMessage pack(rusty::Arc<T> val);
};


void AnyMessage::save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(this->type_name_, ar);
    if (this->payload_.is_some()) {
        const rusty::Arc<SerializableBase>& payload = this->payload_.as_ref().unwrap();
        payload->save(ar);
    }
}

void AnyMessage::load(BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(this->type_name_, ar);
    auto proxy_opt = any_message_registry::create(this->type_name_);
    verify(proxy_opt.is_some());
    rusty::Arc<SerializableBase> proxy = proxy_opt.unwrap();
    proxy.get_mut().unwrap().load(ar);
    this->payload_ = rusty::Option<SerializableProxy>(std::move(proxy));
}

template<typename T>
bool AnyMessage::is_a() const {
    return anymessage_is_a<T>((*this));
}

template<typename T>
rusty::Option<rusty::Arc<T>> AnyMessage::unpack() const {
    return anymessage_unpack<T>((*this));
}

template<typename T>
AnyMessage AnyMessage::pack_as(std::string name, rusty::Arc<T> val) {
    return anymessage_pack_as(std::move(name), std::move(val));
}

template<typename T>
AnyMessage AnyMessage::pack(rusty::Arc<T> val) {
    return anymessage_pack(std::move(val));
}
/*RUSTYCPP:GEN-END id=any_message.message*/

// Runtime registry: maps registered type-name string → factory and
// std::type_index → registered name. Stored behind a rusty::Mutex
// (registrations run at static init time, lookups are concurrent
// across reactor threads during RPC dispatch).
// @safe - see file header.
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

// Create a fresh payload proxy for the given name. None if the
// name is not registered.
rusty::Option<SerializableProxy> create(const std::string& name);

// Look up the registered name for type `ti`. Returns "" if the
// type was not registered (owned copy — no borrow escapes the
// registry mutex; Marshal-era pointer-return reshaped away).
std::string name_for_type_owned(std::type_index ti);

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
// The factory wraps a fresh Arc<T> in a holder-shaped proxy —
// same shape `SerializableEnvelope` uses, so unpack semantics match.
//
// Returns 0 — suitable for `static int _reg = reg_any_message_as<T>("...");`.
template <typename T>
// @unsafe - builds a factory LAMBDA and uses typeid / std::type_index
// (RTTI). Neither is DSL-expressible.
inline int reg_any_message_as(std::string name) {
  auto factory = []() -> SerializableProxy {
    auto sp = rusty::Arc<T>::make();
    return rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(
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
  const std::string name = any_message_registry::name_for_type_owned(
      std::type_index(typeid(T)));
  if (name.empty()) return false;
  return self.type_name_ == name;
}

// @unsafe - dynamic_cast through the held base pointer.
template <typename T>
inline rusty::Option<rusty::Arc<T>> anymessage_unpack(const AnyMessage& self) {
  using Out = rusty::Option<rusty::Arc<T>>;
  if (!anymessage_is_a<T>(self)) return Out(rusty::None);
  if (self.payload_.is_none()) return Out(rusty::None);
  if (auto* h = dynamic_cast<const details::SerializableSharedPtrHolder<T>*>(
          self.payload_.unwrap().get())) {
    return Out(h->ptr.clone());
  }
  return Out(rusty::None);
}

// @unsafe - aggregate-constructs the AnyMessage (returned by value;
// callers store it directly in rcc_rpc.h fields).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. This is the only anymessage_* template
// with no RTTI, hence the only one convertible.
//
// TRAP, probe-verified at pin da6e9bf4: the `let` annotation MUST be the
// `SerializableProxy` ALIAS, never the spelled-out
// `rusty::Arc<SerializableBase>`. Spelling the same template head as the
// turbofish makes the transpiler substitute the ANNOTATION's type argument
// into the call, silently emitting `rusty::Arc<SerializableBase>::make(val)`
// — a different, wrong factory. Through the alias it emits the intended
// `rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(...)`, and the
// Arc<Derived> -> Arc<Base> upcast happens in Arc's implicit converting ctor
// (one refcount bump, exactly as the hand-written version did).
#if RUSTYCPP_RUST
fn anymessage_pack_as<T>(name: std::string, val: rusty::Arc<T>) -> AnyMessage {
    let payload: SerializableProxy =
        rusty::Arc::<details::SerializableSharedPtrHolder<T>>::make(val);
    AnyMessage { type_name_: name, payload_: rusty::Some(payload) }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.pack_as version=1 rust_sha256=a0555f77932caac4c454e9726bbeeb745f4100203ec855b99248c981e31ba75b*/
template<typename T>
AnyMessage anymessage_pack_as(std::string name, rusty::Arc<T> val) {
    SerializableProxy payload = rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(val));
    return AnyMessage{.type_name_ = std::move(name), .payload_ = rusty::Option<SerializableProxy>(std::move(payload))};
}
/*RUSTYCPP:GEN-END id=any_message.pack_as*/

// @unsafe - dereferences raw `const std::string*` from name_for_type
// and forwards to the @unsafe anymessage_pack_as.
template <typename T>
inline AnyMessage anymessage_pack(rusty::Arc<T> val) {
  const std::string name = any_message_registry::name_for_type_owned(
      std::type_index(typeid(T)));
  verify(!name.empty() &&
         "AnyMessage::pack<T>: T not registered. "
         "Call reg_any_message_as<T>(\"name\") at static init.");
  return anymessage_pack_as<T>(name, std::move(val));
}

// ---- Free archive serde fns + operator forwarders --------------------

// Phase 8 batch 4 (endgame straggler): serde free functions own the
// AnyMessage wire format; the operators below are forwarders kept until the
// operator layer is deleted. The two serde fns are LIVE — rcc_rpc.h's
// generated structs reach them via `rrr::Serialize_::serialize(o.md_graph,
// ar)` — so they outlive the operators and must not be deleted with them.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. Both bodies are one-line delegations,
// which the DSL emits UNQUALIFIED in the enclosing namespace — exactly what
// the Serialize_ / ADL bridge needs. `&AnyMessage` lowers to
// `const AnyMessage&` and `&mut AnyMessage` to `AnyMessage&`, so the two
// exported signatures are unchanged; the only delta is the dropped `inline`,
// harmless for a definition attached to this module interface unit.
//
// @unsafe - both forward into save/load, which drive a Marshal
// operator<< / operator>> chain.
#if RUSTYCPP_RUST
fn serialize(am: &AnyMessage, ar: &mut BinaryWriteArchive) {
    am.save(ar);
}

fn deserialize(am: &mut AnyMessage, ar: &mut BinaryReadArchive) {
    am.load(ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.serde version=1 rust_sha256=f6c50f6c35be66f123993360f3f50cea9c4ab21cb634d3414ed11025f009a7e0*/
void serialize(const AnyMessage& am, BinaryWriteArchive& ar) {
    am.save(ar);
}

void deserialize(AnyMessage& am, BinaryReadArchive& ar) {
    am.load(ar);
}
/*RUSTYCPP:GEN-END id=any_message.serde*/



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

// @unsafe - `ar >> type_name_` Marshal operator>> chain + raw
// shared_ptr deref to call payload_->load.

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

// @unsafe - Returns a reference into a process-wide static singleton; the
// caller treats the returned reference as `'static`-lifetime, which
// rusty-cpp doesn't model.
//
// Authored as inline Rust DSL: the Meyers-singleton shape IS expressible —
// `static NAME: T = init;` lowers to a block-scope C++ static, and the
// `&mut NAME` TAIL expression lowers to a plain `return NAME;` (spelling
// `return NAME;` in the DSL instead emits `return std::move(NAME)`, which
// would gut the process-lifetime object on the first call). rusty::Mutex
// has no default ctor (unlike the retired SpinMutex), so it is seeded with
// an empty registry map explicitly.
#if RUSTYCPP_RUST
fn registry() -> &mut rusty::Mutex<AnyMessageRegistryMap> {
    static R: rusty::Mutex<AnyMessageRegistryMap> = rusty::Mutex::new(AnyMessageRegistryMap {});
    &mut R
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.5 version=1 rust_sha256=8411f2bc9c0188bd0f70ab448aedabdae86bf878ac0331205d1503f849e88dc9*/
rusty::Mutex<AnyMessageRegistryMap>& registry() {
    static rusty::Mutex<AnyMessageRegistryMap> R = rusty::Mutex<AnyMessageRegistryMap>::new_(AnyMessageRegistryMap{});
    return R;
}
/*RUSTYCPP:GEN-END id=any_message.5*/

}  // namespace


// Registry API, authored as inline Rust DSL (register_type's old
// "name used twice" kernel reason dissolves with name.clone(); the
// duplicate-name verify loses its string-literal message — diagnostic
// only).
// Reopened namespace: the DSL emits unqualified definitions, which
// must land inside any_message_registry to define the declared API.
namespace any_message_registry {
#if RUSTYCPP_RUST
fn register_type(name: std::string, ti: std::type_index, factory: Factory) -> i32 {
    let mut guard = registry().lock().unwrap();
    let hash: usize = ti.hash_code();
    verify((*guard).by_name.get(&name).is_none());
    if (*guard).name_by_type_hash.get(&hash).is_none() {
        (*guard).name_by_type_hash.insert(hash, name.clone());
    }
    (*guard).by_name.insert(name, factory);
    0
}

fn create(name: &std::string) -> Option<SerializableProxy> {
    let mut guard = registry().lock().unwrap();
    let entry = (*guard).by_name.get(name);
    if entry.is_none() {
        return None;
    }
    Some(entry.unwrap()())
}

fn name_for_type_owned(ti: std::type_index) -> std::string {
    let guard = registry().lock().unwrap();
    let entry = (*guard).name_by_type_hash.get(ti.hash_code());
    if entry.is_none() {
        return Default::default();
    }
    entry.unwrap()
}

fn is_registered_name(name: &std::string) -> bool {
    let guard = registry().lock().unwrap();
    (*guard).by_name.get(name).is_some()
}

fn is_registered_type(ti: std::type_index) -> bool {
    let guard = registry().lock().unwrap();
    (*guard).name_by_type_hash.get(ti.hash_code()).is_some()
}

fn clear_for_testing() {
    let mut guard = registry().lock().unwrap();
    (*guard).by_name.clear();
    (*guard).name_by_type_hash.clear();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=any_message.registry_queries version=1 rust_sha256=74c3c7e34a747d74f8042b369ed48f4fff809431a70c6cefb3ed90d95fc3ca77*/
std::string name_for_type_owned(std::type_index ti);
bool is_registered_name(const std::string& name);
bool is_registered_type(std::type_index ti);
void clear_for_testing();

int32_t register_type(std::string name, std::type_index ti, Factory factory) {
    auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    const size_t hash = ti.hash_code();
    verify((rusty::detail::deref_if_pointer_like(guard)).by_name.get(name).is_none());
    if ((rusty::detail::deref_if_pointer_like(guard)).name_by_type_hash.get(hash).is_none()) {
        (rusty::detail::deref_if_pointer_like(guard)).name_by_type_hash.insert(std::move(hash), rusty::clone(name));
    }
    (rusty::detail::deref_if_pointer_like(guard)).by_name.insert(std::move(name), std::move(factory));
    return static_cast<int32_t>(0);
}

rusty::Option<SerializableProxy> create(const std::string& name) {
    auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    auto entry = (rusty::detail::deref_if_pointer_like(guard)).by_name.get(name);
    if (entry.is_none()) {
        return rusty::Option<SerializableProxy>{rusty::None};
    }
    return rusty::Option<SerializableProxy>(entry.unwrap()());
}

std::string name_for_type_owned(std::type_index ti) {
    const auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    auto entry = (rusty::detail::deref_if_pointer_like(guard)).name_by_type_hash.get(ti.hash_code());
    if (entry.is_none()) {
        return rusty::default_like<std::string>();
    }
    return entry.unwrap();
}

bool is_registered_name(const std::string& name) {
    const auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    return (rusty::detail::deref_if_pointer_like(guard)).by_name.get(name).is_some();
}

bool is_registered_type(std::type_index ti) {
    const auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    return (rusty::detail::deref_if_pointer_like(guard)).name_by_type_hash.get(ti.hash_code()).is_some();
}

void clear_for_testing() {
    auto&& guard = rusty::deref_call(registry().lock(), rusty::detail::__mdisp_unwrap{});
    (rusty::detail::deref_if_pointer_like(guard)).by_name.clear();
    (rusty::detail::deref_if_pointer_like(guard)).name_by_type_hash.clear();
}
/*RUSTYCPP:GEN-END id=any_message.registry_queries*/
}  // namespace any_message_registry

}  // namespace rrr
