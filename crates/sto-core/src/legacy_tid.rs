//! Legacy C++ transaction-version layout and pure parity operations.
//!
//! These definitions are a migration oracle for the existing C++ STO. They
//! are not the representation contract for native Rust versions.

pub const THREAD_ID_MASK: u64 = 0x01ff;
pub const LOCK_BIT: u64 = 0x0200;
pub const NONOPAQUE_BIT: u64 = 0x0400;
pub const USER_BIT: u64 = 0x0800;
pub const INCREMENT_VALUE: u64 = 0x4000;

pub const fn is_locked(version: u64) -> bool {
    version & LOCK_BIT != 0
}

pub const fn is_locked_here(version: u64, thread_id: i32) -> bool {
    let lock_owner = LOCK_BIT | thread_id as u64;
    version & (LOCK_BIT | THREAD_ID_MASK) == lock_owner
}

pub const fn is_locked_elsewhere(version: u64, thread_id: i32) -> bool {
    let lock_owner = version & (LOCK_BIT | THREAD_ID_MASK);
    lock_owner != 0 && lock_owner != (LOCK_BIT | thread_id as u64)
}

pub const fn unlocked(version: u64) -> u64 {
    version & !(LOCK_BIT | THREAD_ID_MASK)
}

pub const fn next_nonopaque_version(version: u64) -> u64 {
    version.wrapping_add(INCREMENT_VALUE) | NONOPAQUE_BIT
}

pub const fn next_unflagged_nonopaque_version(version: u64) -> u64 {
    (version.wrapping_add(INCREMENT_VALUE) & !(INCREMENT_VALUE - 1)) | NONOPAQUE_BIT
}

pub const fn check_version(current: u64, observed: u64, thread_id: i32) -> bool {
    current == observed || current == (observed | LOCK_BIT | thread_id as u64)
}

pub const fn try_check_opacity(start_tid: u64, version: u64) -> bool {
    let delta = start_tid.wrapping_sub(version) as i64;
    delta > 0 && version & (LOCK_BIT | NONOPAQUE_BIT) == 0
}
