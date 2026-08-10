module;

#include <array>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/array.hpp>
#include <rusty/dispatch.hpp>
#include <rusty/marker.hpp>
#include <rusty/option.hpp>
// Reachability: this file's GEN names rusty::ptr::null/null_mut,
// rusty::detail::{deref_if_pointer_like,rust_not,zero_length_array},
// rusty::PhantomData, std::array, and rusty::clone.
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
// reads. The checked raw-holder unpack family, `marshallable_cast`'s
// `const_cast` overload, and the four `operator<<` / `operator>>`
// archive entry points carry per-method `// @unsafe` (Marshal
// operator chains + raw `T*` returns).
export namespace rrr {

// @unsafe - mutable escape from the const-view Arc. Backs the non-const
// `T* unpack_mut()` contract; new code should prefer
// the const overload or unpack_shared. The current pointer-cast lowering
// emits the required C++ const_cast.
#if RUSTYCPP_RUST
fn envelope_holder_ptr_mut<T>(h: *const details::SerializableSharedPtrHolder<T>) -> *mut T {
    let p: *const T = unsafe { (*h).ptr.get() };
    p as *mut T
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.5 version=1 rust_sha256=41ef3ab554f24af2903e22db350794dff41c32496158ba992a187b9f1eb9487c*/
template<typename T>
std::add_pointer_t<T> envelope_holder_ptr_mut(std::add_pointer_t<std::add_const_t<details::SerializableSharedPtrHolder<T>>> h) {
    const std::add_pointer_t<std::add_const_t<T>> p = (*h).ptr.get();
    return const_cast<std::add_pointer_t<T>>(reinterpret_cast<std::add_pointer_t<std::add_const_t<T>>>(p));
}
/*RUSTYCPP:GEN-END id=serializable_envelope.5*/

// @safe - see file header. Authored as inline Rust DSL; the
// `#if RUSTYCPP_RUST` block is the source of truth and the transpiler
// regenerates the GEN block below it.
//
// Closed-set membership is nominal: each accepted payload explicitly
// implements PayloadMember for its set and supplies its fixed wire kind.
// The trait and every bounded API deliberately share this ONE inline block:
// inline blocks are independent codegen units, so splitting them would make
// the C++ emitter silently lose the marker-bound `requires` clauses.
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_marker_trait)]
pub trait PayloadMember<Set> {
    const KIND: i32;
}

pub struct SerializableEnvelope<PayloadSet> {
    // Public `kind_` field — cached snapshot of `inner_->kind()`,
    // refreshed by every state-changing entry point, so the
    // `cmd.kind_ == X` direct-field access pattern keeps compiling.
    pub kind_: i32,
    inner_: rusty::Option<SerializableProxy>,
    _payload_set: [core::marker::PhantomData<PayloadSet>; 0],
}

impl<PayloadSet> SerializableEnvelope<PayloadSet> {
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

    pub fn has_value(&self) -> bool {
        self.inner_.is_some()
    }

    pub fn kind(&self) -> i32 {
        if self.inner_.is_none() {
            return 0i32;
        }
        let b = self.base_ptr();
        unsafe { (*b).kind() }
    }

    // VALUE-SEMANTIC: stores a fresh Arc<T> holding a copy of `value`.
    // Internally identical to pack_aliased(Arc<T>::make(value.clone())) — gives
    // unpack_shared<T> proper refcounted ownership at the cost of one
    // extra heap allocation per pack.
    pub fn pack<T: PayloadMember<PayloadSet> + Clone + 'static>(value: &T) -> SerializableEnvelope<PayloadSet> {
        SerializableEnvelope::<PayloadSet>::pack_aliased::<T>(rusty::Arc::<T>::make(value.clone()))
    }

    // ALIASED: the proxy retains the caller's Arc<T>. Mutations through
    // the caller's handle stay visible to the encoded payload;
    // unpack<T>() aliases the same instance and unpack_shared<T>()
    // returns a refcount-shared Arc<T>.
    pub fn pack_aliased<T: PayloadMember<PayloadSet> + 'static>(sp: rusty::Arc<T>) -> SerializableEnvelope<PayloadSet> {
        let mut env: SerializableEnvelope<PayloadSet> = Default::default();
        env.inner_ = Some(
            rusty::Arc::<details::SerializableSharedPtrHolder<T>>::make(sp));
        env.refresh_kind();
        env
    }

    // INVARIANT: `inner_` is always holder-shaped — every construction
    // path and every SerializableRegistry factory wraps the payload in a
    // SerializableSharedPtrHolder<T>, so one checked TypeId cast suffices with no
    // direct-SerializableBase fallback.
    pub fn unpack<T: PayloadMember<PayloadSet> + 'static>(&self) -> *const T {
        let h = serializable_holder_of::<T>(self.base_ptr());
        if h.is_null() {
            return core::ptr::null();
        }
        unsafe { (*h).ptr.get() }
    }

    // Recover the payload as a refcount-shared Arc<T> (for pack_aliased
    // envelopes this shares the caller's original Arc; for pack
    // envelopes the envelope-owned copy). None on empty or mismatch.
    pub fn unpack_shared<T: PayloadMember<PayloadSet> + 'static>(&self) -> rusty::Option<rusty::Arc<T>> {
        let h = serializable_holder_of::<T>(self.base_ptr());
        if h.is_null() {
            return None;
        }
        Some(unsafe { (*h).ptr.clone() })
    }

    pub fn is_a<T: PayloadMember<PayloadSet> + 'static>(&self) -> bool {
        let p: *const T = self.unpack::<T>();
        !p.is_null()
    }

    // Wire format: [v32 kind] [payload bytes] — same as MarshallDeputy post-L9.
    pub fn save(&self, ar: &mut BinaryWriteArchive) {
        verify(self.has_value());
        let b = self.base_ptr();
        Serialize_::serialize(v32::new(unsafe { (*b).kind() }), ar);
        unsafe { (*b).save(ar); }
    }

    pub fn load(&mut self, ar: &mut BinaryReadArchive) {
        let mut kind_v = v32::new(0i32);
        Deserialize_::deserialize(&mut kind_v, ar);
        let kind: i32 = kind_v.get();
        let mut proxy: SerializableProxy = SerializableRegistry::create(kind);
        proxy.get_mut().unwrap().load(ar);
        self.inner_ = Some(proxy);
        self.refresh_kind();
    }
}

// Explicitly named mutable escape. Keeping a second same-named `unpack`
// overload was legal C++ but invalid Rust source.
impl<PayloadSet> SerializableEnvelope<PayloadSet> {
    pub fn unpack_mut<T: PayloadMember<PayloadSet> + 'static>(&mut self) -> *mut T {
        let h = serializable_holder_of::<T>(self.base_ptr());
        if h.is_null() {
            return core::ptr::null_mut();
        }
        envelope_holder_ptr_mut::<T>(h)
    }
}

// One source of truth for Rust Default and the C++ default constructor.
// `#[cpp_ctor]` lowers this pure literal into the ctor needed by generated
// RPC structs, while Rust sees an ordinary, valid Default implementation.
impl<PayloadSet> Default for SerializableEnvelope<PayloadSet> {
    #[cfg_attr(any(), cpp_ctor)]
    fn default() -> SerializableEnvelope<PayloadSet> {
        SerializableEnvelope {
            kind_: 0i32,
            inner_: None,
            _payload_set: [],
        }
    }
}

// Rust parity for the C++ envelope's existing Arc-sharing copy semantics.
// Spell this manually rather than deriving Clone: derive would impose the
// irrelevant `PayloadSet: Clone` bound through the zero-sized marker field.
impl<PayloadSet> Clone for SerializableEnvelope<PayloadSet> {
    fn clone(&self) -> SerializableEnvelope<PayloadSet> {
        let mut env: SerializableEnvelope<PayloadSet> = Default::default();
        env.kind_ = self.kind_;
        env.inner_ = self.inner_.clone();
        env
    }
}

// Identity comparison. Empty envelopes are equal iff both are empty;
// non-empty ones iff they wrap the same SerializableBase instance — so
// two pack_aliased(sp) envelopes from one source compare equal, while
// pack(v) copies own their own holder and compare unequal.
// (operator!= is gone: C++20 synthesizes it from operator==.)
impl<PayloadSet> PartialEq for SerializableEnvelope<PayloadSet> {
    fn eq(&self, other: &SerializableEnvelope<PayloadSet>) -> bool {
        if self.inner_.is_none() && other.inner_.is_none() {
            return true;
        }
        if self.inner_.is_none() || other.inner_.is_none() {
            return false;
        }
        self.base_ptr() == other.base_ptr()
    }
}

// Migration compatibility: T is declared first so callers keep spelling
// `marshallable_cast<T>(env)` while PayloadSet is deduced from the argument.
pub fn marshallable_cast<T: PayloadMember<PayloadSet> + 'static, PayloadSet>(env: &SerializableEnvelope<PayloadSet>) -> rusty::Option<rusty::Arc<T>> {
    env.unpack_shared::<T>()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.1 version=1 rust_sha256=18c1704fbfd5434f9a4a9fab3b3fd04740edcac62edb01962f46e71d247fcb8b*/
template<typename Set, typename Implementor>
struct PayloadMember;
template<typename PayloadSet>
struct SerializableEnvelope;
template<typename T, typename PayloadSet>
    requires (PayloadMember<PayloadSet, T>::value)
rusty::Option<rusty::Arc<T>> marshallable_cast(const SerializableEnvelope<PayloadSet>& env);

template<typename Set, typename Implementor>
struct PayloadMember { static constexpr bool value = false; };

template<typename PayloadSet>
struct SerializableEnvelope {
    int32_t kind_;
    rusty::Option<SerializableProxy> inner_;
    [[no_unique_address]] rusty::detail::zero_length_array<rusty::PhantomData<PayloadSet>> _payload_set;

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
        requires (PayloadMember<PayloadSet, T>::value && rusty::clone_like<T>)
    static SerializableEnvelope<PayloadSet> pack(const T& value) {
        return SerializableEnvelope<PayloadSet>::pack_aliased(rusty::Arc<T>::make(rusty::clone(value)));
    }
    template<typename T>
        requires (PayloadMember<PayloadSet, T>::value)
    static SerializableEnvelope<PayloadSet> pack_aliased(rusty::Arc<T> sp) {
        SerializableEnvelope<PayloadSet> env = rusty::default_like<SerializableEnvelope<PayloadSet>>();
        env.inner_ = rusty::Option<SerializableProxy>(rusty::Arc<details::SerializableSharedPtrHolder<T>>::make(std::move(sp)));
        env.refresh_kind();
        return std::move(env);
    }
    template<typename T>
        requires (PayloadMember<PayloadSet, T>::value)
    std::add_pointer_t<std::add_const_t<T>> unpack() const {
        const auto h = serializable_holder_of<T>(this->base_ptr());
        if ((h == nullptr)) {
            return rusty::ptr::null();
        }
        // @unsafe
        {
            return (rusty::detail::deref_if_pointer_like(h)).ptr.get();
        }
    }
    template<typename T>
        requires (PayloadMember<PayloadSet, T>::value)
    rusty::Option<rusty::Arc<T>> unpack_shared() const {
        const auto h = serializable_holder_of<T>(this->base_ptr());
        if ((h == nullptr)) {
            return rusty::Option<rusty::Arc<T>>{rusty::None};
        }
        return rusty::Option<rusty::Arc<T>>(rusty::clone((rusty::detail::deref_if_pointer_like(h)).ptr));
    }
    template<typename T>
        requires (PayloadMember<PayloadSet, T>::value)
    bool is_a() const {
        const std::add_pointer_t<std::add_const_t<T>> p = this->template unpack<T>();
        return rusty::detail::rust_not((p == nullptr));
    }
    void save(BinaryWriteArchive& ar) const {
        verify(this->has_value());
        const auto b = this->base_ptr();
        Serialize_::serialize(v32::new_(((*b)).kind()), ar);
        // @unsafe
        {
            ((*b)).save(ar);
        }
    }
    void load(BinaryReadArchive& ar) {
        auto kind_v = v32::new_(static_cast<int32_t>(0));
        Deserialize_::deserialize(kind_v, ar);
        int32_t kind = kind_v.get();
        SerializableProxy proxy = SerializableRegistry::create(std::move(kind));
        proxy.get_mut().unwrap().load(ar);
        this->inner_ = rusty::Option<SerializableProxy>(std::move(proxy));
        this->refresh_kind();
    }
    template<typename T>
        requires (PayloadMember<PayloadSet, T>::value)
    std::add_pointer_t<T> unpack_mut() {
        const auto h = serializable_holder_of<T>(this->base_ptr());
        if ((h == nullptr)) {
            return rusty::ptr::null_mut();
        }
        return envelope_holder_ptr_mut<T>(std::move(h));
    }
    SerializableEnvelope()
        : kind_(static_cast<int32_t>(0))
        , inner_(rusty::Option<SerializableProxy>{rusty::None})
        , _payload_set(std::array<rusty::PhantomData<PayloadSet>, 0>{})
    {}
    SerializableEnvelope<PayloadSet> clone() const {
        SerializableEnvelope<PayloadSet> env = rusty::default_like<SerializableEnvelope<PayloadSet>>();
        env.kind_ = this->kind_;
        env.inner_ = rusty::clone(this->inner_);
        return std::move(env);
    }
    bool operator==(const SerializableEnvelope<PayloadSet>& other) const {
        if (this->inner_.is_none() && other.inner_.is_none()) {
            return true;
        }
        if (this->inner_.is_none() || other.inner_.is_none()) {
            return false;
        }
        return this->base_ptr() == other.base_ptr();
    }
};

template<typename T, typename PayloadSet>
    requires (PayloadMember<PayloadSet, T>::value)
rusty::Option<rusty::Arc<T>> marshallable_cast(const SerializableEnvelope<PayloadSet>& env) {
    return env.template unpack_shared<T>();
}
/*RUSTYCPP:GEN-END id=serializable_envelope.1*/

// Free archive serde entry points — let SerializableEnvelope ride
// directly in rpcgen-emitted RPC struct fields the same way any other
// Serializable type does.
// Phase 8 batch 4: serde free functions own the envelope wire format.
// @unsafe - forwards to `env.save(ar)` / `env.load(ar)`, which drive
// the Marshal operator chains.
#if RUSTYCPP_RUST
pub fn serialize<PayloadSet>(env: &SerializableEnvelope<PayloadSet>, ar: &mut BinaryWriteArchive) {
    env.save(ar);
}

pub fn deserialize<PayloadSet>(env: &mut SerializableEnvelope<PayloadSet>, ar: &mut BinaryReadArchive) {
    env.load(ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=serializable_envelope.4 version=1 rust_sha256=8df0f2d016046beb2462b8ff6b87de513d426722b5ad900a2bdf0a67632194c5*/
template<typename PayloadSet>
void serialize(const SerializableEnvelope<PayloadSet>& env, BinaryWriteArchive& ar) {
    env.save(ar);
}

template<typename PayloadSet>
void deserialize(SerializableEnvelope<PayloadSet>& env, BinaryReadArchive& ar) {
    env.load(ar);
}
/*RUSTYCPP:GEN-END id=serializable_envelope.4*/

// Marshal-deprecation slice C: the legacy `Marshal&` envelope operators
// are deleted — the archive save/load path above is the only surface.


}  // export namespace rrr
