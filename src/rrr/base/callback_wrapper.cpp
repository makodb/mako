module;

#include <rusty/arc.hpp>
#include <rusty/function.hpp>
#include <rusty/move.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>
#include <rusty/traits.hpp>

export module rrr.callback_wrapper;

import std;

// @safe - optional shared wrapper around a callable type `F`.
// Every method just forwards into rusty types whose `// @safe`
// annotations are already in the rusty-cpp library. No raw pointers,
// syscalls, or Marshal chains.
export namespace rrr {
// @safe - see file header.
namespace detail {

#if RUSTYCPP_RUST
pub(crate) struct CallbackWrapper<F> {
    inner: rusty::Option<rusty::Arc<F>>,
}

impl<F> CallbackWrapper<F> {
    pub(crate) fn from_callable(callable: F) -> CallbackWrapper<F> {
        CallbackWrapper {
            inner: Some(rusty::Arc::<F>::make(callable)),
        }
    }

    pub(crate) fn has_value(&self) -> bool {
        self.inner.is_some()
    }

    pub(crate) fn callable(&self) -> &F {
        &**self.inner.as_ref().unwrap()
    }
}

impl<F> Clone for CallbackWrapper<F> {
    fn clone(&self) -> CallbackWrapper<F> {
        CallbackWrapper { inner: self.inner.clone() }
    }
}

impl<F> Default for CallbackWrapper<F> {
    fn default() -> CallbackWrapper<F> {
        CallbackWrapper { inner: None }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=callback_wrapper.wrapper version=1 rust_sha256=0268fd9c4207ece117c1e449d7b0d3df917199053f5f675846ba988bc9b853de*/
template<typename F>
struct CallbackWrapper;

template<typename F>
struct CallbackWrapper {
    rusty::Option<rusty::Arc<F>> inner;

    static CallbackWrapper<F> from_callable(F callable) {
        return CallbackWrapper<F>{.inner = rusty::Option<rusty::Arc<F>>(rusty::Arc<F>::make(std::move(callable)))};
    }
    bool has_value() const {
        return this->inner.is_some();
    }
    const F& callable() const {
        return rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer_like(this->inner.as_ref().unwrap()));
    }
    CallbackWrapper<F> clone() const {
        return CallbackWrapper<F>{.inner = rusty::clone(this->inner)};
    }
    static CallbackWrapper<F> default_() {
        return CallbackWrapper<F>{.inner = rusty::Option<rusty::Arc<F>>{rusty::None}};
    }
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = rusty::is_send<F>::value && rusty::is_sync<F>::value;
    static constexpr bool is_sync = rusty::is_send<F>::value && rusty::is_sync<F>::value;
};
/*RUSTYCPP:GEN-END id=callback_wrapper.wrapper*/

}  // namespace detail
}  // export namespace rrr
