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

#if RUSTYCPP_RUST
pub mod detail {
    #[repr(C)]
    pub struct CallbackWrapper<F> {
        pub inner: Option<std::sync::Arc<F>>,
    }

    impl<F> CallbackWrapper<F> {
        pub fn from_callable(callable: F) -> CallbackWrapper<F> {
            CallbackWrapper {
                inner: Some(std::sync::Arc::<F>::new(callable)),
            }
        }

        pub fn has_value(&self) -> bool {
            self.inner.is_some()
        }

        #[allow(clippy::explicit_auto_deref)]
        pub fn callable(&self) -> &F {
            &**self.inner.as_ref().unwrap()
        }
    }

    impl<F> Clone for CallbackWrapper<F> {
        fn clone(&self) -> CallbackWrapper<F> {
            CallbackWrapper {
                inner: self.inner.clone(),
            }
        }
    }

    impl<F> Default for CallbackWrapper<F> {
        fn default() -> CallbackWrapper<F> {
            CallbackWrapper { inner: None }
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=callback_wrapper.wrapper version=1 rust_sha256=0eb7067cfafc0124b1e840f1c90c8a028e2d1df50f0e12acf6494d1b3fa8bd7f*/
namespace detail {}

namespace detail {
    template<typename F>
    struct CallbackWrapper;
}

// mod detail
namespace detail {

    template<typename F>
    struct CallbackWrapper;

    template<typename F>
    struct CallbackWrapper {
        rusty::Option<rusty::Arc<F>> inner;

        static CallbackWrapper<F> from_callable(F callable) {
            return CallbackWrapper<F>{.inner = rusty::Option<rusty::Arc<F>>(rusty::Arc<F>::new_(std::move(callable)))};
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

}
/*RUSTYCPP:GEN-END id=callback_wrapper.wrapper*/

}  // export namespace rrr
