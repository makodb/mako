//! Cache-order namespace modes.

/// Immutable dense-order authority selected for one claimed cache namespace.
///
/// The discriminants are part of the engine contract.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum CacheOrderMode {
    /// The engine allocates every concurrent dense cache sequence.
    Concurrent = 1,
    /// The exclusive Rust producer owns dense allocation for the claim.
    SingleProducer = 2,
    /// Independent Rust worker lanes own their local sequences while the
    /// engine coordinates HLC allocation with general-commit certification.
    PerWorker = 3,
}
