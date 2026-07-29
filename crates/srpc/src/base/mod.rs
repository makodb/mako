//! Foundation layer — the `src/rrr/base/` port (conversion slice S1).
//!
//! What the C++ base/ holds and where it went:
//!   * `basetypes.cpp` — SparseInt varints are already the [`crate::wire`]
//!     layer; `Counter`/`Time`/`Timer` are here.
//!   * `threading.cpp` — `SpinLock` is here; the pthread wrapper family
//!     is subsumed by `std::sync`.
//!   * `logging.cpp` — [`log`].
//!   * `debugging.cpp` — `verify()` is Rust's `assert!`; backtraces are
//!     `std::backtrace`. Nothing to port.
//!   * `strop.cpp` — `str::starts_with`/`ends_with`/`split_whitespace`.
//!     Only the thousands-separator formatter had real logic, and it has
//!     no consumer in the RPC path.
//!   * `callback_wrapper.cpp` — an `Arc<Function<...>>` shim that
//!     `Arc<dyn Fn>` makes unnecessary.

pub mod log;
pub mod sync;
pub mod time;
