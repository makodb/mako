#![no_std]

//! Raw declarations for Mako's hardened Masstree C ABI.
//!
//! This crate contains no ownership or threading policy. Applications should
//! use the safe `masstree` crate instead of calling these functions directly.
//!
//! The declarations intentionally carry no `#[link]` attribute. The native
//! shim must be built by Mako's CMake build with the same generated Masstree
//! configuration and linked by the final application or native integration
//! test. Ordinary Rust-only checks therefore do not require a native library.

use core::ffi::c_void;

pub const ABI_VERSION: u32 = 1;
pub type RecordId = u64;
pub const RECORD_ID_NONE: RecordId = 0;
pub const CONFIGURED_MAX_KEY_LENGTH: usize = 1024;

pub type Status = i32;
pub const OK: Status = 0;
pub const ERR_INVALID: Status = 1;
pub const ERR_KEY_TOO_LARGE: Status = 2;
pub const ERR_BUFFER_TOO_SMALL: Status = 3;
pub const ERR_NOT_ATTACHED: Status = 4;
pub const ERR_WRONG_THREAD: Status = 5;
pub const ERR_WRONG_RUNTIME: Status = 6;
pub const ERR_THREAD_LIMIT: Status = 7;
pub const ERR_OUT_OF_MEMORY: Status = 8;
pub const ERR_BUSY: Status = 9;
pub const ERR_ACTIVE_GUARDS: Status = 10;
pub const ERR_ABI_MISMATCH: Status = 11;
pub const ERR_CPP_EXCEPTION: Status = 12;
pub const ERR_INTERNAL: Status = 13;
pub const ERR_UNSUPPORTED: Status = 14;
pub const ERR_INCOMPATIBLE_RUNTIME: Status = 15;
pub const ERR_POISONED: Status = 16;
pub const ERR_CLOSED: Status = 17;

pub type FeatureSet = u64;
pub const FEATURE_POINT_GET: FeatureSet = 1 << 0;
pub const FEATURE_ATOMIC_GET_OR_INSERT: FeatureSet = 1 << 1;
pub const FEATURE_EXPLICIT_HANDLES: FeatureSet = 1 << 2;
pub const FEATURE_BINARY_KEYS: FeatureSet = 1 << 3;
pub const FEATURE_INTEGRAL_RECORD_IDS: FeatureSet = 1 << 4;
pub const FEATURE_RUNTIME_HEALTH: FeatureSet = 1 << 5;
pub const FEATURE_SINGLETON_RUNTIME: FeatureSet = 1 << 6;
pub const FEATURE_GRACEFUL_SHUTDOWN: FeatureSet = 1 << 7;
pub const FEATURE_COPIED_RANGE_SCANS: FeatureSet = 1 << 8;
pub const FEATURE_SCOPED_POINT_READS: FeatureSet = 1 << 9;
pub const FEATURE_SCOPED_STRIDED_POINT_READS: FeatureSet = 1 << 10;
pub const FEATURE_STRIDED_POINT_READS: FeatureSet = 1 << 11;
pub const FEATURE_SCOPED_RCU: FeatureSet = 1 << 12;

pub type ByteOrder = u32;
pub const BYTE_ORDER_UNKNOWN: ByteOrder = 0;
pub const BYTE_ORDER_LITTLE_ENDIAN: ByteOrder = 1;
pub const BYTE_ORDER_BIG_ENDIAN: ByteOrder = 2;

pub type RuntimeHealthState = u32;
pub const RUNTIME_HEALTHY: RuntimeHealthState = 1;
pub const RUNTIME_POISONED: RuntimeHealthState = 2;

pub type PublicationDisposition = u32;
pub const PUBLICATION_FAILURE_BEFORE_PUBLICATION: PublicationDisposition = 1;
pub const PUBLICATION_CANDIDATE_INSERTED: PublicationDisposition = 2;
pub const PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED: PublicationDisposition = 3;
pub const PUBLICATION_UNKNOWN: PublicationDisposition = 4;

#[repr(C)]
pub struct Runtime {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Thread {
    _private: [u8; 0],
}

#[repr(C)]
pub struct Tree {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RuntimeConfig {
    pub struct_size: u32,
    pub abi_version: u32,
    pub required_features: FeatureSet,
    pub max_threads: u32,
    pub max_key_length: u32,
    pub reserved: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct BuildId {
    pub low: u64,
    pub high: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReadScope {
    pub owner: usize,
    pub generation: u64,
}

pub type RcuScope = ReadScope;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GetOrInsertResult {
    pub winner: RecordId,
    pub publication: PublicationDisposition,
    pub inserted: u8,
    pub reserved: [u8; 3],
}

pub type ScanBoundKind = u32;
pub const SCAN_BOUND_ABSENT: ScanBoundKind = 0;
pub const SCAN_BOUND_INCLUSIVE: ScanBoundKind = 1;
pub const SCAN_BOUND_EXCLUSIVE: ScanBoundKind = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ScanBound {
    pub key: *const c_void,
    pub key_length: usize,
    pub kind: ScanBoundKind,
    pub reserved: u32,
}

pub type ScanDirection = u32;
pub const SCAN_FORWARD: ScanDirection = 1;
pub const SCAN_REVERSE: ScanDirection = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanEntry {
    pub key_offset: usize,
    pub key_length: usize,
    pub record_id: RecordId,
}

pub type ScanStopReason = u32;
pub const SCAN_STOP_END: ScanStopReason = 1;
pub const SCAN_STOP_ENTRY_CAPACITY: ScanStopReason = 2;
pub const SCAN_STOP_KEY_ARENA_CAPACITY: ScanStopReason = 3;

pub type ScanResumeKind = u32;
pub const SCAN_RESUME_NONE: ScanResumeKind = 0;
pub const SCAN_RESUME_UNCHANGED_INPUT: ScanResumeKind = 1;
pub const SCAN_RESUME_EXCLUSIVE_LAST: ScanResumeKind = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanResult {
    pub entries_written: usize,
    pub arena_bytes_used: usize,
    pub next_key_bytes_required: usize,
    pub stop_reason: ScanStopReason,
    pub resume: ScanResumeKind,
    pub resume_key_offset: usize,
    pub resume_key_length: usize,
    pub reserved: [u64; 2],
}

pub const REQUIRED_V1_FEATURES: FeatureSet = FEATURE_POINT_GET
    | FEATURE_ATOMIC_GET_OR_INSERT
    | FEATURE_EXPLICIT_HANDLES
    | FEATURE_BINARY_KEYS
    | FEATURE_INTEGRAL_RECORD_IDS
    | FEATURE_RUNTIME_HEALTH
    | FEATURE_SINGLETON_RUNTIME
    | FEATURE_COPIED_RANGE_SCANS
    | FEATURE_SCOPED_POINT_READS
    | FEATURE_SCOPED_STRIDED_POINT_READS
    | FEATURE_STRIDED_POINT_READS
    | FEATURE_SCOPED_RCU;

const EXPORTED_SYMBOLS: &str = concat!(
    "mt_abi_version;mt_feature_bits;mt_endianness;mt_pointer_width;",
    "mt_max_key_length;mt_max_threads;mt_record_id_limit;",
    "mt_runtime_config_size;mt_runtime_config_alignment;mt_build_id_size;",
    "mt_build_id_alignment;mt_read_scope_size;mt_read_scope_alignment;",
    "mt_get_or_insert_result_size;",
    "mt_get_or_insert_result_alignment;mt_scan_bound_size;",
    "mt_scan_bound_alignment;mt_scan_entry_size;mt_scan_entry_alignment;",
    "mt_scan_result_size;mt_scan_result_alignment;",
    "mt_exported_symbols_fingerprint;",
    "mt_get_build_fingerprint;mt_runtime_config_init;mt_runtime_acquire;",
    "mt_runtime_health;mt_runtime_max_key_length;mt_runtime_max_threads;",
    "mt_runtime_shutdown;mt_thread_attach;mt_thread_quiesce;mt_tree_create;",
    "mt_tree_release;mt_get;mt_get_strided;mt_read_scope_begin;mt_read_scope_get;",
    "mt_read_scope_get_strided;mt_read_scope_end;mt_rcu_scope_begin;",
    "mt_rcu_scope_end;mt_get_or_insert;mt_scan"
);

const fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash = 14_695_981_039_346_656_037_u64;
    let mut index = 0;
    while index < bytes.len() {
        hash ^= bytes[index] as u64;
        hash = hash.wrapping_mul(1_099_511_628_211);
        index += 1;
    }
    hash
}

pub const EXPORTED_SYMBOLS_FINGERPRINT: u64 = fnv1a(EXPORTED_SYMBOLS.as_bytes());
pub const EXPORTED_SYMBOL_COUNT: usize = 43;

unsafe extern "C" {
    pub fn mt_abi_version() -> u32;
    pub fn mt_feature_bits() -> FeatureSet;
    pub fn mt_endianness() -> ByteOrder;
    pub fn mt_pointer_width() -> u32;
    pub fn mt_max_key_length() -> usize;
    pub fn mt_max_threads() -> u32;
    pub fn mt_record_id_limit() -> RecordId;
    pub fn mt_runtime_config_size() -> usize;
    pub fn mt_runtime_config_alignment() -> usize;
    pub fn mt_build_id_size() -> usize;
    pub fn mt_build_id_alignment() -> usize;
    pub fn mt_read_scope_size() -> usize;
    pub fn mt_read_scope_alignment() -> usize;
    pub fn mt_get_or_insert_result_size() -> usize;
    pub fn mt_get_or_insert_result_alignment() -> usize;
    pub fn mt_scan_bound_size() -> usize;
    pub fn mt_scan_bound_alignment() -> usize;
    pub fn mt_scan_entry_size() -> usize;
    pub fn mt_scan_entry_alignment() -> usize;
    pub fn mt_scan_result_size() -> usize;
    pub fn mt_scan_result_alignment() -> usize;
    pub fn mt_exported_symbols_fingerprint() -> u64;
    pub fn mt_get_build_fingerprint(out: *mut BuildId) -> Status;
    pub fn mt_runtime_config_init(out: *mut RuntimeConfig) -> Status;
    pub fn mt_runtime_acquire(config: *const RuntimeConfig, out: *mut *mut Runtime) -> Status;
    pub fn mt_runtime_health(runtime: *const Runtime, out: *mut RuntimeHealthState) -> Status;
    pub fn mt_runtime_max_key_length(runtime: *const Runtime, out: *mut usize) -> Status;
    pub fn mt_runtime_max_threads(runtime: *const Runtime, out: *mut u32) -> Status;
    pub fn mt_runtime_shutdown(runtime: *mut Runtime, thread: *mut Thread) -> Status;
    pub fn mt_thread_attach(runtime: *mut Runtime, out: *mut *mut Thread) -> Status;
    pub fn mt_thread_quiesce(thread: *mut Thread) -> Status;
    pub fn mt_tree_create(
        runtime: *mut Runtime,
        thread: *mut Thread,
        out: *mut *mut Tree,
    ) -> Status;
    pub fn mt_tree_release(tree: *mut Tree) -> Status;
    pub fn mt_get(
        tree: *mut Tree,
        thread: *mut Thread,
        key: *const c_void,
        key_length: usize,
        out: *mut RecordId,
    ) -> Status;
    pub fn mt_get_strided(
        tree: *mut Tree,
        thread: *mut Thread,
        keys: *const c_void,
        key_count: usize,
        key_length: usize,
        key_stride: usize,
        out: *mut RecordId,
    ) -> Status;
    pub fn mt_read_scope_begin(
        tree: *mut Tree,
        thread: *mut Thread,
        token: *mut ReadScope,
    ) -> Status;
    pub fn mt_read_scope_get(
        token: *const ReadScope,
        key: *const c_void,
        key_length: usize,
        out: *mut RecordId,
    ) -> Status;
    pub fn mt_read_scope_get_strided(
        token: *const ReadScope,
        keys: *const c_void,
        key_count: usize,
        key_length: usize,
        key_stride: usize,
        out: *mut RecordId,
    ) -> Status;
    pub fn mt_read_scope_end(token: *mut ReadScope) -> Status;
    pub fn mt_rcu_scope_begin(thread: *mut Thread, token: *mut RcuScope) -> Status;
    pub fn mt_rcu_scope_end(token: *mut RcuScope) -> Status;
    pub fn mt_get_or_insert(
        tree: *mut Tree,
        thread: *mut Thread,
        key: *const c_void,
        key_length: usize,
        candidate: RecordId,
        out: *mut GetOrInsertResult,
    ) -> Status;
    pub fn mt_scan(
        tree: *mut Tree,
        thread: *mut Thread,
        direction: ScanDirection,
        lower: *const ScanBound,
        upper: *const ScanBound,
        entries: *mut ScanEntry,
        entry_capacity: usize,
        key_arena: *mut c_void,
        key_arena_capacity: usize,
        out: *mut ScanResult,
    ) -> Status;
}

/// Private static-link entry points used only by the safe `masstree` facade.
///
/// These symbols deliberately sit outside the versioned `mt_*` C ABI. They
/// immediately dereference handles and rely on the caller to retain and
/// validate every handle, key, output, thread-affinity, and runtime invariant.
#[doc(hidden)]
pub mod trusted {
    use super::{
        c_void, GetOrInsertResult, RecordId, ScanBound, ScanDirection, ScanEntry, ScanResult,
        Status, Thread, Tree,
    };

    pub const SCAN_RESUME_INCLUSIVE_NEXT: super::ScanResumeKind = 3;

    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
    pub struct RecordIdScanResult {
        pub records_written: usize,
        pub continuation_bytes_used: usize,
        pub next_key_bytes_required: usize,
        pub stop_reason: super::ScanStopReason,
        pub resume: super::ScanResumeKind,
        pub reserved: [u64; 2],
    }

    #[cfg(target_pointer_width = "64")]
    const _: [(); 48] = [(); core::mem::size_of::<RecordIdScanResult>()];

    unsafe extern "C" {
        pub fn mako_mtree_get_trusted(
            tree: *mut Tree,
            thread: *mut Thread,
            key: *const c_void,
            key_length: usize,
            out: *mut RecordId,
        ) -> Status;
        pub fn mako_mtree_get_strided_trusted(
            tree: *mut Tree,
            thread: *mut Thread,
            keys: *const c_void,
            key_count: usize,
            key_length: usize,
            out: *mut RecordId,
        ) -> Status;
        pub fn mako_mtree_get_or_insert_trusted(
            tree: *mut Tree,
            thread: *mut Thread,
            key: *const c_void,
            key_length: usize,
            candidate: RecordId,
            out: *mut GetOrInsertResult,
        ) -> Status;
        pub fn mako_mtree_get_or_insert_strided_trusted(
            tree: *mut Tree,
            thread: *mut Thread,
            keys: *const c_void,
            key_count: usize,
            key_length: usize,
            key_stride: usize,
            candidates: *const RecordId,
            out: *mut GetOrInsertResult,
        ) -> Status;
        pub fn mako_mtree_scan_trusted(
            tree: *mut Tree,
            thread: *mut Thread,
            direction: ScanDirection,
            lower: *const ScanBound,
            upper: *const ScanBound,
            entries: *mut ScanEntry,
            entry_capacity: usize,
            key_arena: *mut c_void,
            key_arena_capacity: usize,
            out: *mut ScanResult,
        ) -> Status;
        pub fn mako_mtree_scan_record_ids_bounded_trusted(
            tree: *mut Tree,
            thread: *mut Thread,
            lower: *const c_void,
            lower_length: usize,
            upper: *const c_void,
            upper_length: usize,
            records: *mut RecordId,
            record_capacity: usize,
            continuation: *mut c_void,
            continuation_capacity: usize,
            out: *mut RecordIdScanResult,
        ) -> Status;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn public_pod_layout_matches_the_c11_contract() {
        assert_eq!(core::mem::size_of::<RuntimeConfig>(), 40);
        assert_eq!(core::mem::align_of::<RuntimeConfig>(), 8);
        assert_eq!(core::mem::size_of::<BuildId>(), 16);
        assert_eq!(core::mem::align_of::<BuildId>(), 8);
        assert_eq!(core::mem::size_of::<ReadScope>(), 16);
        assert_eq!(core::mem::align_of::<ReadScope>(), 8);
        assert_eq!(core::mem::size_of::<RcuScope>(), 16);
        assert_eq!(core::mem::align_of::<RcuScope>(), 8);
        assert_eq!(core::mem::size_of::<GetOrInsertResult>(), 16);
        assert_eq!(core::mem::align_of::<GetOrInsertResult>(), 8);
        assert_eq!(core::mem::size_of::<ScanBound>(), 24);
        assert_eq!(core::mem::align_of::<ScanBound>(), 8);
        assert_eq!(core::mem::size_of::<ScanEntry>(), 24);
        assert_eq!(core::mem::align_of::<ScanEntry>(), 8);
        assert_eq!(core::mem::size_of::<ScanResult>(), 64);
        assert_eq!(core::mem::align_of::<ScanResult>(), 8);
    }

    #[test]
    fn required_features_exclude_unimplemented_shutdown() {
        assert_eq!(FEATURE_POINT_GET, 1 << 0);
        assert_eq!(FEATURE_ATOMIC_GET_OR_INSERT, 1 << 1);
        assert_eq!(FEATURE_EXPLICIT_HANDLES, 1 << 2);
        assert_eq!(FEATURE_BINARY_KEYS, 1 << 3);
        assert_eq!(FEATURE_INTEGRAL_RECORD_IDS, 1 << 4);
        assert_eq!(FEATURE_RUNTIME_HEALTH, 1 << 5);
        assert_eq!(FEATURE_SINGLETON_RUNTIME, 1 << 6);
        assert_eq!(FEATURE_GRACEFUL_SHUTDOWN, 1 << 7);
        assert_eq!(FEATURE_COPIED_RANGE_SCANS, 1 << 8);
        assert_eq!(FEATURE_SCOPED_POINT_READS, 1 << 9);
        assert_eq!(FEATURE_SCOPED_STRIDED_POINT_READS, 1 << 10);
        assert_eq!(FEATURE_STRIDED_POINT_READS, 1 << 11);
        assert_eq!(FEATURE_SCOPED_RCU, 1 << 12);
        assert_eq!(REQUIRED_V1_FEATURES, 0x1f7f);
        assert_eq!(REQUIRED_V1_FEATURES & FEATURE_GRACEFUL_SHUTDOWN, 0);
    }

    #[test]
    fn fixed_width_enum_values_match_the_c11_contract() {
        assert_eq!(
            [
                OK,
                ERR_INVALID,
                ERR_KEY_TOO_LARGE,
                ERR_BUFFER_TOO_SMALL,
                ERR_NOT_ATTACHED,
                ERR_WRONG_THREAD,
                ERR_WRONG_RUNTIME,
                ERR_THREAD_LIMIT,
                ERR_OUT_OF_MEMORY,
                ERR_BUSY,
                ERR_ACTIVE_GUARDS,
                ERR_ABI_MISMATCH,
                ERR_CPP_EXCEPTION,
                ERR_INTERNAL,
                ERR_UNSUPPORTED,
                ERR_INCOMPATIBLE_RUNTIME,
                ERR_POISONED,
                ERR_CLOSED,
            ],
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17]
        );
        assert_eq!(
            [
                PUBLICATION_FAILURE_BEFORE_PUBLICATION,
                PUBLICATION_CANDIDATE_INSERTED,
                PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED,
                PUBLICATION_UNKNOWN,
            ],
            [1, 2, 3, 4]
        );
        assert_eq!(
            [
                SCAN_BOUND_ABSENT,
                SCAN_BOUND_INCLUSIVE,
                SCAN_BOUND_EXCLUSIVE
            ],
            [0, 1, 2]
        );
        assert_eq!([SCAN_FORWARD, SCAN_REVERSE], [1, 2]);
        assert_eq!(
            [
                SCAN_STOP_END,
                SCAN_STOP_ENTRY_CAPACITY,
                SCAN_STOP_KEY_ARENA_CAPACITY,
            ],
            [1, 2, 3]
        );
        assert_eq!(
            [
                SCAN_RESUME_NONE,
                SCAN_RESUME_UNCHANGED_INPUT,
                SCAN_RESUME_EXCLUSIVE_LAST,
            ],
            [0, 1, 2]
        );
        assert_eq!(
            [
                BYTE_ORDER_UNKNOWN,
                BYTE_ORDER_LITTLE_ENDIAN,
                BYTE_ORDER_BIG_ENDIAN,
            ],
            [0, 1, 2]
        );
        assert_eq!([RUNTIME_HEALTHY, RUNTIME_POISONED], [1, 2]);
    }

    #[test]
    fn complete_exported_symbol_manifest_matches_the_finalized_header() {
        assert_eq!(EXPORTED_SYMBOL_COUNT, 43);
        assert_eq!(EXPORTED_SYMBOLS.split(';').count(), EXPORTED_SYMBOL_COUNT);
        assert_eq!(EXPORTED_SYMBOLS_FINGERPRINT, 0x0b2e_c215_8e69_d9c7);
    }

    #[test]
    fn zero_record_id_remains_reserved() {
        assert_eq!(RECORD_ID_NONE, 0);
    }
}
