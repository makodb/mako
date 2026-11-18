//! The singleton view must preserve the general plan's callback cuts.

use super::*;

struct TargetPositions<'a> {
    adapter: &'a DirectInjectAdapter,
    count: usize,
}

impl CommitHook for TargetPositions<'_> {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        assert_eq!(
            self.adapter.target_calls.load(Ordering::Relaxed),
            2 * self.count
        );
        Ok(())
    }
    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        assert_eq!(
            self.adapter.target_calls.load(Ordering::Relaxed),
            3 * self.count
        );
        Ok(())
    }
    fn post_install(&mut self) {
        assert_eq!(
            self.adapter.target_calls.load(Ordering::Relaxed),
            4 * self.count
        );
    }
}

#[test]
fn prepared_view_preserves_target_callback_positions_and_pool_reuse() {
    for mode in [DirectIdentityMode::Checked, DirectIdentityMode::Injective] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) =
            register_direct_resource_with_modes(&runtime, Vec::new(), false, false, mode);
        let mut worker = runtime.attach().unwrap();
        for count in [1, 3, 1, 2, 1] {
            resource.adapter().target_calls.store(0, Ordering::Relaxed);
            harness.clear();
            let mut txn = worker.begin().unwrap();
            for key in 1..=count {
                stage_direct_read_write(&mut txn, &resource, key as u64);
            }
            let mut hook = TargetPositions {
                adapter: resource.adapter(),
                count,
            };
            assert!(matches!(
                txn.commit_with_hook(&mut hook).unwrap(),
                CommitOutcome::Committed(_)
            ));
            assert_eq!(
                resource.adapter().target_calls.load(Ordering::Relaxed),
                5 * count
            );
            assert_eq!(
                callback_keys(&harness.events(), Callback::Acquire).len(),
                count
            );
            assert_eq!(
                callback_keys(&harness.events(), Callback::Install).len(),
                count
            );
            assert_eq!(finish_events(&harness.events()).len(), count);
            assert_eq!(runtime.health(), RuntimeHealth::Healthy);
        }
    }
}

fn target_faults(action: Action) {
    for (call, phase) in [
        (2, FailurePhase::Acquire),
        (3, FailurePhase::Validation),
        (4, FailurePhase::Install),
        (5, FailurePhase::Release),
    ] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) = register_injective_direct_resource_with_actions(
            &runtime,
            vec![(Callback::TargetBorrow, call, action)],
            false,
            DirectIdentityMode::Injective,
        );
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        stage_direct_read_write(&mut txn, &resource, 7);
        let result = txn.commit();
        if matches!(phase, FailurePhase::Acquire | FailurePhase::Validation) {
            assert_poisoned_aborted(result, phase);
            assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
            assert!(callback_keys(&harness.events(), Callback::Install).is_empty());
        } else {
            let Err(CommitFailure::Indeterminate(info)) = result else {
                panic!("expected indeterminate {phase:?}, got {result:?}");
            };
            assert_eq!(info.phase(), phase);
            assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
            assert!(finish_events(&harness.events()).is_empty());
        }
        if phase == FailurePhase::Release {
            assert!(release_events(&harness.events()).is_empty());
            assert!(callback_keys(&harness.events(), Callback::GuardDrop).is_empty());
        }
    }
}

#[test]
fn singleton_changed_targets_keep_each_failure_phase() {
    target_faults(Action::Fault);
}

#[test]
fn singleton_target_borrow_panics_keep_each_failure_phase() {
    target_faults(Action::Panic);
}

#[test]
fn singleton_guard_and_finish_panics_keep_cleanup_boundaries() {
    for (callback, phase, committed) in [
        (Callback::Install, FailurePhase::Install, false),
        (Callback::ReleaseCommitted, FailurePhase::Release, false),
        (Callback::GuardDrop, FailurePhase::Release, true),
        (Callback::Finish, FailurePhase::Finish, true),
    ] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) = register_injective_direct_resource_with_actions(
            &runtime,
            vec![(callback, 7, Action::Panic)],
            false,
            DirectIdentityMode::Injective,
        );
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        stage_direct_read_write(&mut txn, &resource, 7);
        let result = txn.commit();
        if committed {
            let Err(CommitFailure::Poisoned { outcome, info }) = result else {
                panic!("expected committed poison at {phase:?}, got {result:?}");
            };
            assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
            assert_eq!(info.phase(), phase);
            assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
        } else {
            let Err(CommitFailure::Indeterminate(info)) = result else {
                panic!("expected indeterminate at {phase:?}, got {result:?}");
            };
            assert_eq!(info.phase(), phase);
            assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
            assert!(finish_events(&harness.events()).is_empty());
        }
    }
}
