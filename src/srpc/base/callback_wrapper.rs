// Canonical Rust source for the srpc.callback_wrapper module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
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

        // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it makes `callable()` return `this->inner.as_ref().unwrap()` -- the Box handle -- where `const F&` is declared; both deref_if_pointer_like unwraps are dropped (2 emitted lines in srpc.callback_wrapper.cppm).
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
