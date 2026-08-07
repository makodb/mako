module;

#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
// Reachability: this file's GEN names rusty::ptr::null/null_mut,
// rusty::detail::deref_if_pointer_like + rust_not, and rusty::clone.
// A GMF must include what its own GEN names.
#include <rusty/move.hpp>
#include <rusty/ptr.hpp>
#include <rusty/slice.hpp>

export module rrr.serializable_envelope;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.serializable;

// @safe - SerializableEnvelope: rusty::Arc-backed sum type carried
// over the Marshal wire. The kind/has_value/operator bool/operator==
// accessors and `refresh_kind` are pure pointer-equality and integer
// reads. The dynamic_cast-heavy unpack family, `marshallable_cast`'s
// `const_cast` overload, and the four `operator<<` / `operator>>`
// archive entry points carry per-method `// @unsafe` (Marshal
// operator chains + raw `T*` returns).
export namespace rrr {


// @unsafe kernels for the DSL class below. Each is a construct the DSL
// genuinely cannot express, kept minimal so the envelope's SHAPE stays
// in Rust: C++ template metaprogramming (the TypeList membership
// static_assert), the RTTI downcast, the const-escape that backs the
// historical `T* unpack()` contract, and the four archive/registry
// steps that touch v32 + the serde free functions. Signature rule: a
// DSL `&mut` PARAMETER passes through as a C++ reference (the archives),
// while `&mut local` lowers to a pointer (the proxy).

// @unsafe - THE kernel: the C++ dependent-name `template` disambiguator.
// `TypeList::contains::<T>()` IS spellable in the DSL and DOES lower to a
// static_assert; the one thing the emitter cannot add is the `template`
// keyword C++ requires before a dependent member template. One
// non-dependent line buys it back, and the assertion itself moves to DSL.
template<typename TypeList, typename T>
inline constexpr bool type_list_contains() {
  return TypeList::template contains<T>();
}

// @safe - Authored as inline Rust DSL; the `#if RUSTYCPP_RUST` block is the
// source of truth and the transpiler regenerates the GEN block below it.
// A fn-body `const _: () = assert!(cond, "prose")` lowers to a real in-body
// `static_assert`, so this stays a COMPILE-TIME check with its prose intact.
// Diff from the hand-written C++: the fn is no longer `constexpr` — every
// call site is a statement, never a constant evaluation.
#if RUSTYCPP_RUST
fn envelope_assert_in_type_list<TypeList, T>() {
    const _: () = assert!(type_list_contains::<TypeList, T>(),
        "SerializableEnvelope: T is not in TypeList. Add T to the TypeList declaration.");
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.2 version=1 rust_sha256=dea492a53b809d1a5c01a4aad52896e67e7761db04d490987852d572f3a3eae3*/
template<typename TypeList, typename T>
void envelope_assert_in_type_list();

template<typename TypeList, typename T>
void envelope_assert_in_type_list() {
    static_assert(type_list_contains<TypeList, T>(), "assert ! (type_list_contains ::< TypeList , T > () , \"SerializableEnvelope: T is not in TypeList. Add T to the TypeList declaration.\")");
}
/*RUSTYCPP:GEN-END id=serializable_envelope.2*/

// @unsafe - RTTI downcast to the holder for T (nullptr on miss).
template<typename T>
inline const details::SerializableSharedPtrHolder<T>* envelope_holder_of(
    const SerializableBase* base) {
  return dynamic_cast<const details::SerializableSharedPtrHolder<T>*>(base);
}

// @unsafe - mutable escape from the const-view Arc. Backs the non-const
// `T* unpack()` overload's historical contract; new code should prefer
// the const overload or unpack_shared.
template<typename T>
inline T* envelope_holder_ptr_mut(
    const details::SerializableSharedPtrHolder<T>* h) {
  return const_cast<details::SerializableSharedPtrHolder<T>*>(h)->ptr.as_ptr();
}

// @unsafe - v32 construction + the Serialize_/Deserialize_ free functions.
inline void envelope_write_kind(int32_t kind, BinaryWriteArchive& ar) {
  rrr::Serialize_::serialize(v32(kind), ar);
}
inline int32_t envelope_read_kind(BinaryReadArchive& ar) {
  v32 kind_v;
  rrr::Deserialize_::deserialize(kind_v, ar);
  return kind_v.get();
}

// @unsafe - virtual dispatch through the held base pointer.
inline void envelope_base_save(const SerializableBase* base,
                               BinaryWriteArchive& ar) {
  base->save(ar);
}
// @unsafe - unique-owner mutation window: the proxy is factory-fresh
// (strong_count 1); a shared proxy here would panic loudly.
inline void envelope_proxy_load(SerializableProxy* proxy,
                                BinaryReadArchive& ar) {
  proxy->get_mut().unwrap().load(ar);
}

// @safe - see file header. Authored as inline Rust DSL; the
// `#if RUSTYCPP_RUST` block is the source of truth and the transpiler
// regenerates the GEN block below it.
//
// TWO impl blocks by design: Rust forbids duplicate method names, but
// inline-rust merges every impl in a block into one C++ struct, which
// is how `unpack<T>()` keeps its const/non-const overload PAIR (and so
// its exact historical call-site contract: `const T*` from a const
// envelope, `T*` from a mutable one).
#if RUSTYCPP_RUST
pub struct SerializableEnvelope<TypeList> {
    // Public `kind_` field — cached snapshot of `inner_->kind()`,
    // refreshed by every state-changing entry point, so the
    // `cmd.kind_ == X` direct-field access pattern keeps compiling.
    kind_: i32,
    inner_: rusty::Option<SerializableProxy>,
}

impl<TypeList> SerializableEnvelope<TypeList> {
    // #[cpp_ctor] (not the usual `new_` factory) because the DSL has no
    // default field initializers: the emitted struct would otherwise
    // leave `kind_` indeterminate under `Command cmd;`, and 24 such
    // default-constructions live in GENERATED rcc_rpc.h.
    #[cpp_ctor]
    fn new() -> SerializableEnvelope<TypeList> {
        SerializableEnvelope { kind_: 0i32, inner_: None }
    }

    // Borrow the held base pointer (null when empty). Const-only — the
    // Arc is const-view; mutation goes through the load() window.
    fn base_ptr(&self) -> *const SerializableBase {
        if self.inner_.is_none() {
            return core::ptr::null();
        }
        self.inner_.as_ref().unwrap().get()
    }

    fn refresh_kind(&mut self) {
        if self.inner_.is_none() {
            self.kind_ = 0i32;
            return;
        }
        let b = self.base_ptr();
        self.kind_ = unsafe { (*b).kind() };
    }

    fn has_value(&self) -> bool {
        self.inner_.is_some()
    }

    fn kind(&self) -> i32 {
        if self.inner_.is_none() {
            return 0i32;
        }
        let b = self.base_ptr();
        unsafe { (*b).kind() }
    }

    // VALUE-SEMANTIC: stores a fresh Arc<T> holding a copy of `value`.
    // Internally identical to pack_aliased(Arc<T>::make(value)) — gives
    // unpack_shared<T> proper refcounted ownership at the cost of one
    // extra heap allocation per pack.
    fn pack<T>(value: &T) -> SerializableEnvelope<TypeList> {
        envelope_assert_in_type_list::<TypeList, T>();
        SerializableEnvelope::<TypeList>::pack_aliased::<T>(rusty::Arc::<T>::make(*value))
    }

    // ALIASED: the proxy retains the caller's Arc<T>. Mutations through
    // the caller's handle stay visible to the encoded payload;
    // unpack<T>() aliases the same instance and unpack_shared<T>()
    // returns a refcount-shared Arc<T>.
    fn pack_aliased<T>(sp: rusty::Arc<T>) -> SerializableEnvelope<TypeList> {
        envelope_assert_in_type_list::<TypeList, T>();
        let mut env: SerializableEnvelope<TypeList> = Default::default();
        env.inner_ = Some(
            rusty::Arc::<details::SerializableSharedPtrHolder<T>>::make(sp));
        env.refresh_kind();
        env
    }

    // INVARIANT: `inner_` is always holder-shaped — every construction
    // path and every SerializableRegistry factory wraps the payload in a
    // SerializableSharedPtrHolder<T>, so one downcast suffices with no
    // direct-SerializableBase fallback.
    fn unpack<T>(&self) -> *const T {
        envelope_assert_in_type_list::<TypeList, T>();
        let h = envelope_holder_of::<T>(self.base_ptr());
        if h.is_null() {
            return core::ptr::null();
        }
        unsafe { (*h).ptr.get() }
    }

    // Recover the payload as a refcount-shared Arc<T> (for pack_aliased
    // envelopes this shares the caller's original Arc; for pack
    // envelopes the envelope-owned copy). None on empty or mismatch.
    fn unpack_shared<T>(&self) -> rusty::Option<rusty::Arc<T>> {
        envelope_assert_in_type_list::<TypeList, T>();
        let h = envelope_holder_of::<T>(self.base_ptr());
        if h.is_null() {
            return None;
        }
        Some(unsafe { (*h).ptr.clone() })
    }

    fn is_a<T>(&self) -> bool {
        let p: *const T = self.unpack::<T>();
        !p.is_null()
    }

    // Wire format: [v32 kind] [payload bytes] — same as MarshallDeputy post-L9.
    fn save(&self, ar: &mut BinaryWriteArchive) {
        verify(self.has_value());
        let b = self.base_ptr();
        envelope_write_kind(unsafe { (*b).kind() }, ar);
        envelope_base_save(b, ar);
    }

    fn load(&mut self, ar: &mut BinaryReadArchive) {
        let kind: i32 = envelope_read_kind(ar);
        let mut proxy: SerializableProxy = SerializableRegistry::create(kind);
        envelope_proxy_load(&mut proxy, ar);
        self.inner_ = Some(proxy);
        self.refresh_kind();
    }
}

// The non-const half of the unpack overload pair (see the note above).
impl<TypeList> SerializableEnvelope<TypeList> {
    fn unpack<T>(&mut self) -> *mut T {
        envelope_assert_in_type_list::<TypeList, T>();
        let h = envelope_holder_of::<T>(self.base_ptr());
        if h.is_null() {
            return core::ptr::null_mut();
        }
        envelope_holder_ptr_mut::<T>(h)
    }
}

// Identity comparison. Empty envelopes are equal iff both are empty;
// non-empty ones iff they wrap the same SerializableBase instance — so
// two pack_aliased(sp) envelopes from one source compare equal, while
// pack(v) copies own their own holder and compare unequal.
// (operator!= is gone: C++20 synthesizes it from operator==.)
impl<TypeList> PartialEq for SerializableEnvelope<TypeList> {
    fn eq(&self, other: &SerializableEnvelope<TypeList>) -> bool {
        if self.inner_.is_none() && other.inner_.is_none() {
            return true;
        }
        if self.inner_.is_none() || other.inner_.is_none() {
            return false;
        }
        self.base_ptr() == other.base_ptr()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.1 version=1 rust_sha256=c5351a4d5f60a2219b4671c4385703fc0943fa8057485d50d8da322ca5c81f7a*/
template<typename TypeList>
struct SerializableEnvelope;

template<typename TypeList>
struct SerializableEnvelope {
    int32_t kind_;
    rusty::Option<SerializableProxy> inner_;

    SerializableEnvelope()
        : kind_(static_cast<int32_t>(0))
        , inner_(rusty::Option<SerializableProxy>{rusty::None})
    {}
    const SerializableBase* base_ptr() const {
        if (this->inner_.is_none()) {
            return rusty::ptr::null();
        }
        return this->inner_.as_ref().unwrap().get();
    }
    void refresh_kind() {
        if (this->inner_.is_none()) {
            this->kind_ = static_cast<int32_t>(0);
            return;
        }
        const auto b = this->base_ptr();
        this->kind_ = ((*b)).kind();
    }
    bool has_value() const {
        return this->inner_.is_some();
    }
    int32_t kind() const {
        if (this->inner_.is_none()) {
            return static_cast<int32_t>(0);
        }
        const auto b = this->base_ptr();
        // @unsafe
        {
            return ((*b)).kind();
        }
    }
    template<typename T>
    static SerializableEnvelope<TypeList> pack(const T& value) {
        envelope_assert_in_type_list<TypeList, T>();
        return SerializableEnvelope<TypeList>::pack_aliased(rusty::Arc<T>::make(value));
    }
    template<typename T>
    static SerializableEnvelope<TypeList> pack_aliased(rusty::Arc<T> sp) {
        envelope_assert_in_type_list<TypeList, T>();
        SerializableEnvelope<TypeList> env = rusty::default_like<SerializableEnvelope<TypeList>>();
        env.inner_ = rusty::Option<SerializableProxy>(rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(sp)));
        env.refresh_kind();
        return std::move(env);
    }
    template<typename T>
    std::add_pointer_t<std::add_const_t<T>> unpack() const {
        envelope_assert_in_type_list<TypeList, T>();
        const auto h = envelope_holder_of<T>(this->base_ptr());
        if ((h == nullptr)) {
            return rusty::ptr::null();
        }
        // @unsafe
        {
            return (rusty::detail::deref_if_pointer_like(h)).ptr.get();
        }
    }
    template<typename T>
    rusty::Option<rusty::Arc<T>> unpack_shared() const {
        envelope_assert_in_type_list<TypeList, T>();
        const auto h = envelope_holder_of<T>(this->base_ptr());
        if ((h == nullptr)) {
            return rusty::Option<rusty::Arc<T>>{rusty::None};
        }
        return rusty::Option<rusty::Arc<T>>(rusty::clone((rusty::detail::deref_if_pointer_like(h)).ptr));
    }
    template<typename T>
    bool is_a() const {
        const std::add_pointer_t<std::add_const_t<T>> p = this->template unpack<T>();
        return rusty::detail::rust_not((p == nullptr));
    }
    void save(BinaryWriteArchive& ar) const {
        verify(this->has_value());
        const auto b = this->base_ptr();
        envelope_write_kind(((*b)).kind(), ar);
        envelope_base_save(b, ar);
    }
    void load(BinaryReadArchive& ar) {
        int32_t kind = envelope_read_kind(ar);
        SerializableProxy proxy = SerializableRegistry::create(std::move(kind));
        envelope_proxy_load(&proxy, ar);
        this->inner_ = rusty::Option<SerializableProxy>(std::move(proxy));
        this->refresh_kind();
    }
    template<typename T>
    std::add_pointer_t<T> unpack() {
        envelope_assert_in_type_list<TypeList, T>();
        const auto h = envelope_holder_of<T>(this->base_ptr());
        if ((h == nullptr)) {
            return rusty::ptr::null_mut();
        }
        return envelope_holder_ptr_mut<T>(std::move(h));
    }
    bool operator==(const SerializableEnvelope<TypeList>& other) const {
        if (this->inner_.is_none() && other.inner_.is_none()) {
            return true;
        }
        if (this->inner_.is_none() || other.inner_.is_none()) {
            return false;
        }
        return this->base_ptr() == other.base_ptr();
    }
};
/*RUSTYCPP:GEN-END id=serializable_envelope.1*/

// Migration compat: `marshallable_cast<T>` overload for envelopes.
// Returns Option<Arc<T>> — None on empty envelope / type mismatch.
// (unpack_shared is const now; the old const_cast overload collapsed.)
// @unsafe - authored as inline Rust DSL; the body is a pure delegation
// to the dynamic_cast-backed `unpack_shared`. T is declared FIRST so the
// call sites keep spelling `marshallable_cast<T>(env)` with TypeList
// deduced from the argument.
#if RUSTYCPP_RUST
fn marshallable_cast<T, TypeList>(env: &SerializableEnvelope<TypeList>) -> rusty::Option<rusty::Arc<T>> {
    env.unpack_shared::<T>()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.3 version=1 rust_sha256=3d1b6f75a3a014413617da69d485569cbbaa6909085948a11a4e9826ee664729*/
template<typename T, typename TypeList>
rusty::Option<rusty::Arc<T>> marshallable_cast(const SerializableEnvelope<TypeList>& env) {
    return env.template unpack_shared<T>();
}
/*RUSTYCPP:GEN-END id=serializable_envelope.3*/

template<typename T, typename TypeList>
inline rusty::Option<rusty::Arc<T>> marshallable_cast(
    SerializableEnvelope<TypeList>* env) {
  if (env == nullptr) return rusty::Option<rusty::Arc<T>>(rusty::None);
  return env->template unpack_shared<T>();
}

// Free archive serde entry points — let SerializableEnvelope ride
// directly in rpcgen-emitted RPC struct fields the same way any other
// Serializable type does.
// Phase 8 batch 4: serde free functions own the envelope wire format.
// @unsafe - forwards to `env.save(ar)` / `env.load(ar)`, which drive
// the Marshal operator chains.
#if RUSTYCPP_RUST
fn serialize<TypeList>(env: &SerializableEnvelope<TypeList>, ar: &mut BinaryWriteArchive) {
    env.save(ar);
}

fn deserialize<TypeList>(env: &mut SerializableEnvelope<TypeList>, ar: &mut BinaryReadArchive) {
    env.load(ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.4 version=1 rust_sha256=adac1c607824f4725229a1a6f3cd6a4b8da7870b71e9f169e3d2f0590bc05f46*/
template<typename TypeList>
void serialize(const SerializableEnvelope<TypeList>& env, BinaryWriteArchive& ar) {
    env.save(ar);
}

template<typename TypeList>
void deserialize(SerializableEnvelope<TypeList>& env, BinaryReadArchive& ar) {
    env.load(ar);
}
/*RUSTYCPP:GEN-END id=serializable_envelope.4*/

// Marshal-deprecation slice C: the legacy `Marshal&` envelope operators
// are deleted — the archive save/load path above is the only surface.


}  // export namespace rrr
