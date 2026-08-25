//! Raw declarations for the `mako_local_*` C ABI.
//!
//! The implementation is compiled into CMake's `libmako.a`. This crate owns
//! no policy and performs no linking; [`mako_local`](https://docs.rs/mako-local)
//! supplies the safe types and locates the native archive.

#![allow(non_camel_case_types)]

include!(concat!(env!("OUT_DIR"), "/mako_local_bindings.rs"));
include!(concat!(env!("OUT_DIR"), "/mako_local_statuses.rs"));

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::c_int;

    #[test]
    fn generated_surface_preserves_revision_zero_rust_types_and_layouts() {
        let _: c_int = MAKO_LOCAL_OK;
        let _: u32 = MAKO_LOCAL_ABI_VERSION;
        let _: u32 = MAKO_LOCAL_BUILD_FINGERPRINT_SIZE;
        let _: u32 = MAKO_LOCAL_MAX_MAKO_TIMESTAMP;
        let _: u64 = MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS;
        let _: u32 = MAKO_LOCAL_MAX_TABLE_NAME_BYTES;
        let _: u32 = MAKO_LOCAL_MAX_KEY_BYTES;
        let _: u32 = MAKO_LOCAL_MAX_VALUE_BYTES;
        let _: u32 = MAKO_LOCAL_TXN_ITEM_BUDGET;

        assert_eq!(core::mem::size_of::<mako_local_scan_entry>(), 16);
        assert_eq!(core::mem::align_of::<mako_local_scan_entry>(), 4);
        assert_eq!(
            MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE as usize,
            core::mem::offset_of!(mako_local_scan_options, resume_len)
                + core::mem::size_of::<usize>()
        );
        assert_eq!(MAKO_LOCAL_EXPORT_NAMES.len(), 32);
    }

    #[test]
    fn generated_statuses_are_dense_and_match_raw_constants() {
        let raw_values = [
            MAKO_LOCAL_OK,
            MAKO_LOCAL_CONFLICT,
            MAKO_LOCAL_NOT_ATTACHED,
            MAKO_LOCAL_WRONG_THREAD,
            MAKO_LOCAL_TXN_ALREADY_ACTIVE,
            MAKO_LOCAL_TXN_FINISHED,
            MAKO_LOCAL_WRONG_DB_OR_TABLE,
            MAKO_LOCAL_INVALID_ARGUMENT,
            MAKO_LOCAL_THREAD_LIMIT,
            MAKO_LOCAL_BUSY,
            MAKO_LOCAL_OUT_OF_MEMORY,
            MAKO_LOCAL_INTERNAL,
            MAKO_LOCAL_DUPLICATE_WRITE,
            MAKO_LOCAL_TXN_TOO_LARGE,
            MAKO_LOCAL_VALUE_TOO_LARGE,
            MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED,
            MAKO_LOCAL_BUFFER_TOO_SMALL,
            MAKO_LOCAL_FEATURE_UNAVAILABLE,
            MAKO_LOCAL_WORKER_POISONED,
        ];

        assert_eq!(ALL_KNOWN_STATUSES.len(), 20);
        for (expected_code, status) in ALL_KNOWN_STATUSES.iter().copied().enumerate() {
            let expected_code = expected_code as c_int;
            assert_eq!(status.code(), expected_code);
            assert_eq!(raw_values[expected_code as usize], expected_code);
            assert_eq!(KnownStatus::from_code(expected_code), Some(status));
            assert_eq!(KnownStatus::try_from(expected_code), Ok(status));
            assert_eq!(c_int::from(status), expected_code);
            assert!(!status.name().is_empty());
            assert!(!status.c_symbol().is_empty());
            assert!(!status.message().is_empty());
        }

        assert_eq!(KnownStatus::from_code(-1), None);
        assert_eq!(KnownStatus::from_code(20), None);
        assert_eq!(KnownStatus::try_from(-1), Err(-1));
        assert_eq!(KnownStatus::try_from(20), Err(20));
    }
}
