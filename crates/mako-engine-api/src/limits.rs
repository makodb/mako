//! Limits every local engine honors identically.
//!
//! The cache sizes its buffers, lanes, and worker slots from these constants,
//! so an engine that cannot honor one of them must reject at open time rather
//! than silently accept a different bound.

/// Maximum table-name length.
pub const MAX_TABLE_NAME_BYTES: usize = 1024;
/// Maximum key length.
pub const MAX_KEY_BYTES: usize = 1024;
/// Maximum value length.
pub const MAX_VALUE_BYTES: usize = 1_048_576;
/// Weighted item budget for one transaction.
pub const TRANSACTION_ITEM_BUDGET: usize = 512;
/// Maximum number of OS workers that may attach to an engine in one process.
///
/// Worker identifiers are process-lifetime resources and are not recycled.
pub const MAX_WORKERS: usize = 460;
