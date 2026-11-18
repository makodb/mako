//! Compact hook for the preselected one-put holder terminal.
//!
//! Equivalent to the general Driver's PreselectedHolder sink. Both profiles
//! allocate only the timestamp after validation; the lane owns its sequence.
//! The general profile holds the HLC gate from WritesetLocked until acceptance
//! or until the core finishes abort cleanup. The restricted profile never
//! holds the gate and checks health before and after timestamp allocation.

use super::*;

struct HolderHook<'a, const RESTRICTED: bool> {
    clock: &'a Clock,
    unhealthy: Option<&'a AtomicU8>,
    minted_stamp: u64,
    accepted_stamp: u64,
    failure: Option<Error>,
    gate_held: bool,
    observer: Option<TestCommitObserver>,
}

impl<'a, const RESTRICTED: bool> HolderHook<'a, RESTRICTED> {
    fn new(clock: &'a Clock, unhealthy: Option<&'a AtomicU8>) -> Self {
        Self {
            clock,
            unhealthy,
            minted_stamp: 0,
            accepted_stamp: 0,
            failure: None,
            gate_held: false,
            observer: worker::observer(),
        }
    }

    #[inline(always)]
    fn notify(&self, phase: TestCommitPhase, stamp: u64) {
        if let Some(observer) = self.observer {
            observer(phase, mako_clock::expand(stamp));
        }
    }

    #[inline(always)]
    fn unhealthy(&self) -> bool {
        self.unhealthy
            .is_some_and(|health| health.load(Ordering::Acquire) != 0)
    }

    #[cold]
    fn fail(&mut self, error: Error) -> std::result::Result<(), CommitHookError> {
        self.failure = Some(error);
        Err(CommitHookError::Rejected)
    }

    #[inline]
    fn leave_gate(&mut self) {
        if self.gate_held {
            self.gate_held = false;
            self.clock.leave_general();
        }
    }
}

impl<const RESTRICTED: bool> CommitHook for HolderHook<'_, RESTRICTED> {
    fn reserve_upper_metadata(&mut self) -> std::result::Result<(), CommitHookError> {
        self.notify(TestCommitPhase::WritesetLocked, 0);
        if RESTRICTED {
            crate::prefetch::restricted_clock(self.clock);
        } else {
            self.clock.enter_general();
            self.gate_held = true;
        }
        Ok(())
    }

    fn pre_install(&mut self) -> std::result::Result<(), CommitHookError> {
        self.notify(TestCommitPhase::LocalValidationComplete, 0);
        if RESTRICTED {
            if self.unhealthy() {
                return self.fail(Error::CommitHookRejected);
            }
            loop {
                match self.clock.try_allocate_restricted_timestamp() {
                    StampAllocation::Accepted(stamp) => {
                        self.minted_stamp = stamp;
                        break;
                    }
                    StampAllocation::GeneralLocked => spin_loop(),
                    StampAllocation::TimestampExhausted => {
                        return self.fail(Error::TimestampExhausted);
                    }
                }
            }
        } else {
            let Some(stamp) = self.clock.try_allocate_timestamp() else {
                return self.fail(Error::TimestampExhausted);
            };
            self.minted_stamp = stamp;
        }
        self.notify(TestCommitPhase::MakoTimestampAllocated, self.minted_stamp);
        if self.unhealthy() {
            return self.fail(Error::CommitHookRejected);
        }
        self.accepted_stamp = self.minted_stamp;
        self.leave_gate();
        // A one-operation holder never has a FirstWriteInstalled observer.
        #[cfg(feature = "test-hooks")]
        worker::prepare_install_event(None, None);
        self.notify(TestCommitPhase::PreinstallAccepted, self.minted_stamp);
        Ok(())
    }

    fn post_install(&mut self) {
        self.notify(TestCommitPhase::AllWritesInstalled, self.minted_stamp);
    }
}

impl<const RESTRICTED: bool> Drop for HolderHook<'_, RESTRICTED> {
    fn drop(&mut self) {
        #[cfg(feature = "test-hooks")]
        worker::prepare_install_event(None, None);
        // run_profile retains this hook until the consuming core terminal
        // returns, including panic containment and write-lock cleanup.
        self.leave_gate();
    }
}

pub(super) struct HolderRun {
    pub(super) report: CommitReport,
    pub(super) timestamp_stamp: u64,
}

#[inline(always)]
pub(super) fn run(
    inner: StoTransaction,
    unhealthy: Option<*const u8>,
    restricted: bool,
) -> HolderRun {
    // SAFETY: the enclosing holder terminal already requires the optional
    // atomic health byte to remain live through this synchronous commit.
    // Bind that contract to a borrow for the entire hook, with Acquire loads.
    let unhealthy = unhealthy.map(|address| unsafe { AtomicU8::from_ptr(address.cast_mut()) });
    if restricted {
        run_profile::<true>(inner, unhealthy)
    } else {
        run_profile::<false>(inner, unhealthy)
    }
}

#[inline(always)]
fn run_profile<const RESTRICTED: bool>(
    inner: StoTransaction,
    unhealthy: Option<&AtomicU8>,
) -> HolderRun {
    let mut hook = HolderHook::<RESTRICTED>::new(mako_clock::global(), unhealthy);
    #[cfg(feature = "test-hooks")]
    let outcome = sto_core::test_install::with_observer(worker::observe_install, || {
        inner.commit_with_hook(&mut hook)
    });
    #[cfg(not(feature = "test-hooks"))]
    let outcome = inner.commit_with_hook(&mut hook);
    let run = HolderRun {
        report: map_commit(outcome, hook.failure),
        timestamp_stamp: hook.accepted_stamp,
    };
    drop(hook);
    run
}

#[cfg(all(test, any(not(miri), feature = "test-hooks")))]
mod tests {
    use super::*;

    fn clock() -> Clock {
        let clock = Clock::new();
        #[cfg(feature = "test-hooks")]
        assert!(clock.physical().set_test_unix_ms(1_000));
        clock
    }

    #[test]
    fn general_profile_releases_gate_after_acceptance() {
        let clock = clock();
        let mut hook = HolderHook::<false>::new(&clock, None);
        hook.reserve_upper_metadata().unwrap();
        assert!(clock.general_gate_held());
        hook.pre_install().unwrap();
        assert!(!clock.general_gate_held());
        assert_eq!(hook.accepted_stamp, hook.minted_stamp);
        assert!(mako_clock::valid_stamp(hook.accepted_stamp));
    }

    #[test]
    fn restricted_profile_does_not_acquire_general_gate() {
        let clock = clock();
        let mut hook = HolderHook::<true>::new(&clock, None);
        hook.reserve_upper_metadata().unwrap();
        assert!(!clock.general_gate_held());
        hook.pre_install().unwrap();
        assert!(!clock.general_gate_held());
        assert!(mako_clock::valid_stamp(hook.accepted_stamp));
    }

    #[test]
    fn restricted_health_rejection_does_not_mint_a_stamp() {
        let clock = clock();
        let unhealthy = AtomicU8::new(1);
        let mut hook = HolderHook::<true>::new(&clock, Some(&unhealthy));
        hook.reserve_upper_metadata().unwrap();
        assert_eq!(hook.pre_install(), Err(CommitHookError::Rejected));
        assert_eq!(hook.failure, Some(Error::CommitHookRejected));
        assert_eq!(hook.accepted_stamp, 0);
        assert_eq!(clock.current_stamp(), 0);
        assert!(!clock.general_gate_held());
    }

    #[test]
    fn general_health_rejection_retains_gate_until_terminal_cleanup() {
        let clock = clock();
        let unhealthy = AtomicU8::new(1);
        {
            let mut hook = HolderHook::<false>::new(&clock, Some(&unhealthy));
            hook.reserve_upper_metadata().unwrap();
            assert_eq!(hook.pre_install(), Err(CommitHookError::Rejected));
            assert_eq!(hook.failure, Some(Error::CommitHookRejected));
            assert!(hook.minted_stamp != 0);
            assert_eq!(hook.accepted_stamp, 0);
            assert!(clock.general_gate_held());
        }
        assert!(!clock.general_gate_held());
    }

    #[test]
    fn validation_abort_retains_gate_until_terminal_cleanup() {
        let clock = clock();
        {
            let mut hook = HolderHook::<false>::new(&clock, None);
            hook.reserve_upper_metadata().unwrap();
            assert!(clock.general_gate_held());
            // No pre_install call: the core rejected final validation.
        }
        assert!(!clock.general_gate_held());
        assert_eq!(clock.current_stamp(), 0);
    }

    static AFTER_MINT_HEALTH: AtomicU8 = AtomicU8::new(0);

    fn reject_after_mint(phase: TestCommitPhase, _: Option<MakoTimestamp>) {
        if phase == TestCommitPhase::MakoTimestampAllocated {
            AFTER_MINT_HEALTH.store(1, Ordering::Release);
        }
    }

    #[test]
    fn health_is_rechecked_after_timestamp_allocation() {
        fn check<const RESTRICTED: bool>() {
            let clock = clock();
            AFTER_MINT_HEALTH.store(0, Ordering::Release);
            let mut hook = HolderHook::<RESTRICTED>::new(&clock, Some(&AFTER_MINT_HEALTH));
            hook.observer = Some(reject_after_mint);
            hook.reserve_upper_metadata().unwrap();
            assert_eq!(hook.pre_install(), Err(CommitHookError::Rejected));
            assert_eq!(hook.failure, Some(Error::CommitHookRejected));
            assert!(hook.minted_stamp != 0);
            assert_eq!(hook.accepted_stamp, 0);
            assert_eq!(clock.general_gate_held(), !RESTRICTED);
            drop(hook);
            assert!(!clock.general_gate_held());
        }
        check::<true>();
        check::<false>();
    }

    #[test]
    fn exhaustion_never_accepts_and_releases_general_gate_on_drop() {
        fn check<const RESTRICTED: bool>() {
            let clock = clock();
            clock.observe_stamp(mako_clock::STAMP_MAX);
            let mut hook = HolderHook::<RESTRICTED>::new(&clock, None);
            hook.reserve_upper_metadata().unwrap();
            assert_eq!(hook.pre_install(), Err(CommitHookError::Rejected));
            assert_eq!(hook.failure, Some(Error::TimestampExhausted));
            assert_eq!(hook.minted_stamp, 0);
            assert_eq!(hook.accepted_stamp, 0);
            assert_eq!(clock.general_gate_held(), !RESTRICTED);
            drop(hook);
            assert!(!clock.general_gate_held());
        }
        check::<true>();
        check::<false>();
    }
}
