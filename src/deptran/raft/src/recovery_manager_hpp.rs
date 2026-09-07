#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum RecoveryMode {
    FRESH_START = 0,
    NORMAL_RECOVERY = 1,
    FORCED_FRESH = 2,
}

pub const fn recovery_mode_is_fresh(mode: i32) -> bool {
    mode == RecoveryMode::FRESH_START as i32 ||
        mode == RecoveryMode::FORCED_FRESH as i32
}

pub const fn recovery_mode_needs_recovery(mode: i32) -> bool {
    mode == RecoveryMode::NORMAL_RECOVERY as i32
}

pub const fn recovery_should_clear_forced_fresh(mode: i32,
                                                clear_on_forced_fresh: bool) -> bool {
    mode == RecoveryMode::FORCED_FRESH as i32 && clear_on_forced_fresh
}

pub const fn recovery_storage_open_failed(has_storage: bool,
                                          storage_is_open: bool) -> bool {
    !has_storage || !storage_is_open
}

pub const fn recovery_storage_missing(has_storage: bool) -> bool {
    !has_storage
}

pub const fn recovery_replay_failed(recover_ok: bool) -> bool {
    !recover_ok
}
