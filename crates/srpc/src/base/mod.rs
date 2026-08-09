//! Foundation layer — the `src/rrr/base/` port (conversion slice S1).
//!
//! What the C++ base/ holds and where it went:
//!   * `basetypes.cpp` — SparseInt varints are already the [`crate::wire`]
//!     layer; `Counter`/`Time`/`Timer` are here.
//!   * `threading.cpp` — [`legacy_threading`] preserves the generated C++
//!     compatibility surface; [`sync`] provides the separate Rust-native
//!     synchronization helpers.
//!   * `logging.cpp` — [`log`].
//!   * `debugging.cpp` — `verify()` is Rust's `assert!`; backtraces are
//!     `std::backtrace`. Nothing to port.
//!   * `strop.cpp` — `str::starts_with`/`ends_with`/`split_whitespace`.
//!     Only the thousands-separator formatter had real logic, and it has
//!     no consumer in the RPC path.
//!   * `callback_wrapper.cpp` — [`callback_wrapper`] retains the legacy
//!     generated-C++ compatibility surface while Rust callers can use ordinary
//!     `Arc<dyn Fn>` directly.

pub mod callback_wrapper;
pub mod legacy_basetypes;
pub mod legacy_cpuinfo;
pub mod legacy_logging;
pub mod legacy_rand;
pub mod legacy_threading;
pub mod log;
pub mod misc;
pub mod monotonic;
pub mod rand;
pub mod stat;
pub mod sync;
pub mod time;
