use sto_core::legacy_tid::{
    check_version, is_locked, is_locked_elsewhere, is_locked_here, next_nonopaque_version,
    next_unflagged_nonopaque_version, try_check_opacity, unlocked, INCREMENT_VALUE, LOCK_BIT,
    NONOPAQUE_BIT, THREAD_ID_MASK, USER_BIT,
};

#[test]
fn layout_matches_the_existing_sto_version_word() {
    assert_eq!(THREAD_ID_MASK, 0x01ff);
    assert_eq!(LOCK_BIT, 0x0200);
    assert_eq!(NONOPAQUE_BIT, 0x0400);
    assert_eq!(USER_BIT, 0x0800);
    assert_eq!(INCREMENT_VALUE, 0x4000);
    assert_eq!(THREAD_ID_MASK & LOCK_BIT, 0);
    assert_eq!((INCREMENT_VALUE - 1) & INCREMENT_VALUE, 0);
}

#[test]
fn classifies_unlocked_owned_and_foreign_versions() {
    let base = (7 * INCREMENT_VALUE) | USER_BIT;
    for thread_id in [0, 1, 17, THREAD_ID_MASK as i32] {
        let owned = base | LOCK_BIT | thread_id as u64;
        assert!(is_locked(owned));
        assert!(is_locked_here(owned, thread_id));
        assert!(!is_locked_elsewhere(owned, thread_id));

        let other = if thread_id == 1 { 2 } else { 1 };
        assert!(!is_locked_here(owned, other));
        assert!(is_locked_elsewhere(owned, other));
        assert_eq!(unlocked(owned), base);
    }

    assert!(!is_locked(base));
    assert!(!is_locked_elsewhere(base, 9));

    // The low bits also act as a reader count for the legacy RW-lock path.
    assert!(is_locked_elsewhere(base | 3, 9));
}

#[test]
fn advances_opaque_and_unflagged_versions_with_cpp_wrapping_semantics() {
    assert_eq!(next_nonopaque_version(0), INCREMENT_VALUE | NONOPAQUE_BIT);
    assert_eq!(
        next_nonopaque_version(USER_BIT | 3),
        INCREMENT_VALUE | NONOPAQUE_BIT | USER_BIT | 3
    );
    assert_eq!(
        next_unflagged_nonopaque_version(USER_BIT | 3),
        INCREMENT_VALUE | NONOPAQUE_BIT
    );

    assert_eq!(
        next_nonopaque_version(u64::MAX),
        (INCREMENT_VALUE - 1) | NONOPAQUE_BIT
    );
    assert_eq!(next_unflagged_nonopaque_version(u64::MAX), NONOPAQUE_BIT);
}

#[test]
fn accepts_the_observed_version_or_the_same_version_locked_here() {
    let observed = (9 * INCREMENT_VALUE) | USER_BIT;
    assert!(check_version(observed, observed, 23));
    assert!(check_version(observed | LOCK_BIT | 23, observed, 23));
    assert!(!check_version(observed | LOCK_BIT | 24, observed, 23));
    assert!(!check_version(observed + INCREMENT_VALUE, observed, 23));
}

#[test]
fn opacity_check_uses_the_signed_wrapping_delta_and_rejects_flags() {
    assert!(try_check_opacity(
        11 * INCREMENT_VALUE,
        10 * INCREMENT_VALUE
    ));
    assert!(!try_check_opacity(
        10 * INCREMENT_VALUE,
        10 * INCREMENT_VALUE
    ));
    assert!(!try_check_opacity(
        9 * INCREMENT_VALUE,
        10 * INCREMENT_VALUE
    ));
    assert!(!try_check_opacity(
        11 * INCREMENT_VALUE,
        (10 * INCREMENT_VALUE) | LOCK_BIT
    ));
    assert!(!try_check_opacity(
        11 * INCREMENT_VALUE,
        (10 * INCREMENT_VALUE) | NONOPAQUE_BIT
    ));

    // C++ unsigned subtraction followed by the existing signed interpretation.
    let wrapping_version = !(LOCK_BIT | NONOPAQUE_BIT);
    assert!(try_check_opacity(0, wrapping_version));
}
