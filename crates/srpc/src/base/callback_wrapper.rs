//! Shared nullable callbacks for the generated C++ consumer.
//!
//! The C++ API stores an `Arc<F>` behind an optional wrapper so copied
//! callbacks share callable state while a default value remains empty.  This
//! owner keeps that representation explicit instead of relying on an
//! impossible-to-construct empty Rust trait object.

/// Compatibility types intentionally live in the legacy `rrr::detail`
/// namespace when transpiled.
pub mod detail {
    use std::sync::Arc;

    /// Optional, shared ownership of a callable value.
    pub struct CallbackWrapper<F> {
        inner: Option<Arc<F>>,
    }

    impl<F> CallbackWrapper<F> {
        pub fn from_callable(callable: F) -> Self {
            Self {
                inner: Some(Arc::new(callable)),
            }
        }

        pub fn has_value(&self) -> bool {
            self.inner.is_some()
        }

        pub fn callable(&self) -> &F {
            &**self.inner.as_ref().unwrap()
        }
    }

    impl<F> Clone for CallbackWrapper<F> {
        fn clone(&self) -> Self {
            Self {
                inner: self.inner.clone(),
            }
        }
    }

    impl<F> Default for CallbackWrapper<F> {
        fn default() -> Self {
            Self { inner: None }
        }
    }
}
