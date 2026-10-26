//! Singleton typed dispatch and the larger erased dispatch share one protocol.

use super::*;

struct PhaseHook {
    harness: Arc<Harness>,
    keys: Vec<u64>,
    calls: Vec<FailurePhase>,
    reject: Option<FailurePhase>,
    panic: Option<FailurePhase>,
}

impl PhaseHook {
    fn new(harness: &Arc<Harness>, keys: Vec<u64>) -> Self {
        Self {
            harness: Arc::clone(harness),
            keys,
            calls: Vec::new(),
            reject: None,
            panic: None,
        }
    }

    fn check(&mut self, phase: FailurePhase) -> Result<(), CommitHookError> {
        self.calls.push(phase);
        assert_ne!(self.panic, Some(phase), "injected singleton hook panic");
        if self.reject == Some(phase) {
            Err(CommitHookError::Rejected)
        } else {
            Ok(())
        }
    }
}

impl CommitHook for PhaseHook {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        let events = self.harness.events();
        assert_eq!(callback_keys(&events, Callback::Acquire), self.keys);
        assert!(callback_keys(&events, Callback::Validation).is_empty());
        assert!(callback_keys(&events, Callback::Install).is_empty());
        assert!(release_events(&events).is_empty());
        self.check(FailurePhase::UpperMetadata)
    }

    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        let events = self.harness.events();
        assert_eq!(callback_keys(&events, Callback::Validation), self.keys);
        assert!(callback_keys(&events, Callback::Install).is_empty());
        assert!(release_events(&events).is_empty());
        self.check(FailurePhase::PreinstallHook)
    }

    fn post_install(&mut self) {
        let events = self.harness.events();
        assert_eq!(callback_keys(&events, Callback::Install), self.keys);
        assert!(release_events(&events).is_empty());
        self.check(FailurePhase::PostInstallHook).unwrap();
    }
}

#[test]
fn single_and_multi_direct_hooks_preserve_phase_order() {
    let modes = [
        (DirectIdentityMode::Checked, false),
        (DirectIdentityMode::Checked, true),
        (DirectIdentityMode::Injective, false),
        (DirectIdentityMode::Injective, true),
        // This fixture's owned capability requires its finish callback.
        #[cfg(feature = "test-install-observer")]
        (DirectIdentityMode::Owned, false),
    ];
    for (mode, drop_only) in modes {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) =
            register_direct_resource_with_modes(&runtime, Vec::new(), false, drop_only, mode);
        let mut worker = runtime.attach().unwrap();
        for count in [1, 3, 1, 3] {
            harness.clear();
            let keys: Vec<_> = (1..=count).collect();
            let mut transaction = worker.begin().unwrap();
            for &key in &keys {
                stage_direct_read_write(&mut transaction, &resource, key);
            }
            let mut hook = PhaseHook::new(&harness, keys.clone());
            assert!(matches!(
                transaction.commit_with_hook(&mut hook).unwrap(),
                CommitOutcome::Committed(_)
            ));
            assert_eq!(
                hook.calls,
                [
                    FailurePhase::UpperMetadata,
                    FailurePhase::PreinstallHook,
                    FailurePhase::PostInstallHook
                ]
            );
            let events = harness.events();
            assert_eq!(callback_keys(&events, Callback::Preflight), keys);
            assert_eq!(callback_keys(&events, Callback::Install), keys);
            assert_eq!(
                release_events(&events)
                    .iter()
                    .map(|(key, _)| *key)
                    .collect::<Vec<_>>(),
                keys.iter().rev().copied().collect::<Vec<_>>()
            );
            assert!(release_events(&events)
                .iter()
                .all(|(_, disposition)| matches!(
                    disposition,
                    LockDisposition::Committed {
                        occ_commit_id: Some(_)
                    }
                )));
            assert_eq!(
                finish_events(&events).len(),
                if drop_only { 0 } else { count as usize }
            );
            assert_eq!(runtime.health(), RuntimeHealth::Healthy);
        }
    }
}

#[test]
fn singleton_repeated_write_retires_one_commit_item_and_reuses_the_worker() {
    let fixture = direct_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    for attempt in 0..24 {
        fixture.harness.clear();
        let mut transaction = worker.begin().unwrap();
        stage_direct_read_write(&mut transaction, &fixture.resource, 1);
        stage_direct_write(&mut transaction, &fixture.resource, 1);
        match attempt % 3 {
            0 => {
                transaction.commit().unwrap();
            }
            1 => {
                transaction.abort();
            }
            _ => drop(transaction),
        }
        let events = fixture.harness.events();
        assert_eq!(callback_keys(&events, Callback::IntentDrop), [1, 1]);
        assert_eq!(finish_events(&events).len(), 1);
        assert_eq!(
            callback_keys(&events, Callback::Install),
            if attempt % 3 == 0 {
                vec![1]
            } else {
                Vec::new()
            }
        );
        assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
    }
}

fn assert_abort_cleanup(harness: &Harness, acquired: bool) {
    let events = harness.events();
    assert!(callback_keys(&events, Callback::Install).is_empty());
    assert_eq!(
        finish_events(&events),
        [(1, CleanupDisposition::Aborted, false)]
    );
    assert_eq!(
        release_events(&events),
        if acquired {
            vec![(1, LockDisposition::Aborted)]
        } else {
            Vec::new()
        }
    );
    if acquired {
        let release = events
            .iter()
            .position(|event| matches!(event, Event::Release(..)))
            .unwrap();
        let finish = events
            .iter()
            .position(|event| matches!(event, Event::Finish { .. }))
            .unwrap();
        assert!(release < finish);
    }
}

#[test]
fn singleton_hook_rejections_abort_without_installing_and_keep_worker_reusable() {
    for phase in [FailurePhase::UpperMetadata, FailurePhase::PreinstallHook] {
        let fixture = direct_fixture(Vec::new());
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_read_write(&mut transaction, &fixture.resource, 1);
        let mut hook = PhaseHook::new(&fixture.harness, vec![1]);
        hook.reject = Some(phase);
        assert_eq!(
            transaction.commit_with_hook(&mut hook).unwrap(),
            CommitOutcome::Aborted(AbortReason::HookRejected)
        );
        assert_abort_cleanup(&fixture.harness, true);
        assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
        worker.begin().unwrap().commit().unwrap();
    }
}

#[test]
fn singleton_preinstall_panics_preserve_definite_abort_and_poison() {
    for (callback, phase) in [
        (Callback::Preflight, FailurePhase::Preflight),
        (Callback::Validation, FailurePhase::Validation),
    ] {
        let fixture = direct_fixture(vec![(callback, 1, Action::Panic)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_read_write(&mut transaction, &fixture.resource, 1);
        assert_poisoned_aborted(transaction.commit(), phase);
        assert_abort_cleanup(&fixture.harness, phase == FailurePhase::Validation);
        assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
        assert!(worker.begin().is_err());
    }
    for phase in [FailurePhase::UpperMetadata, FailurePhase::PreinstallHook] {
        let fixture = direct_fixture(Vec::new());
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_read_write(&mut transaction, &fixture.resource, 1);
        let mut hook = PhaseHook::new(&fixture.harness, vec![1]);
        hook.panic = Some(phase);
        assert_poisoned_aborted(transaction.commit_with_hook(&mut hook), phase);
        assert_abort_cleanup(&fixture.harness, true);
        assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
        assert!(worker.begin().is_err());
    }
}

#[test]
fn singleton_conflicts_and_faults_keep_exact_cleanup() {
    for callback in [Callback::Preflight, Callback::Acquire, Callback::Validation] {
        for action in [Action::Conflict, Action::Fault] {
            let fixture = direct_fixture(vec![(callback, 1, action)]);
            let mut worker = fixture.runtime.attach().unwrap();
            let mut transaction = worker.begin().unwrap();
            stage_direct_read_write(&mut transaction, &fixture.resource, 1);
            let result = transaction.commit();
            if action == Action::Conflict {
                assert!(matches!(
                    result,
                    Ok(CommitOutcome::Aborted(AbortReason::Conflict(_)))
                ));
                assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
                worker.begin().unwrap().abort();
            } else {
                assert!(matches!(
                    result,
                    Err(CommitFailure::Poisoned {
                        outcome: DefiniteOutcome::Aborted(_),
                        ..
                    })
                ));
                assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
            }
            assert_abort_cleanup(&fixture.harness, callback == Callback::Validation);
        }
    }
}

// Called from the existing exact quarantine tests so their leak/Miri
// qualification continues to describe every intentionally retained failure.
pub(super) fn singleton_uncertain_boundaries() {
    for panic_on_drop in [false, true] {
        let fixture = direct_fixture(Vec::new());
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_read_write(&mut transaction, &fixture.resource, 1);
        let mut hook = PayloadHook {
            harness: Arc::clone(&fixture.harness),
            phase: FailurePhase::PostInstallHook,
            panic_on_drop,
            dropped: Arc::new(AtomicUsize::new(0)),
        };
        assert!(matches!(
            transaction.commit_with_hook(&mut hook),
            Err(CommitFailure::Indeterminate(_))
        ));
        assert_eq!(hook.dropped.load(Ordering::Relaxed), 1);
        assert_eq!(release_events(&fixture.harness.events()).len(), 1);
        assert!(finish_events(&fixture.harness.events()).is_empty());
    }
    for (callback, phase) in [
        (Callback::Install, FailurePhase::Install),
        (Callback::ReleaseCommitted, FailurePhase::Release),
        (Callback::Finish, FailurePhase::Finish),
        (Callback::GuardDrop, FailurePhase::Release),
    ] {
        let fixture = direct_fixture(vec![(callback, 1, Action::Panic)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_read_write(&mut transaction, &fixture.resource, 1);
        let result = transaction.commit();
        if matches!(callback, Callback::Finish | Callback::GuardDrop) {
            let Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(_),
                info,
            }) = result
            else {
                panic!("published singleton must remain committed: {result:?}")
            };
            assert_eq!(info.phase(), phase);
            assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
        } else {
            let Err(CommitFailure::Indeterminate(info)) = result else {
                panic!("irreversible singleton must remain indeterminate: {result:?}")
            };
            assert_eq!(info.phase(), phase);
            assert_eq!(fixture.runtime.health(), RuntimeHealth::Indeterminate);
            assert!(finish_events(&fixture.harness.events()).is_empty());
        }
        assert!(worker.begin().is_err());
    }
    let fixture = direct_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &fixture.resource, 1);
    let mut hook = PhaseHook::new(&fixture.harness, vec![1]);
    hook.panic = Some(FailurePhase::PostInstallHook);
    let Err(CommitFailure::Indeterminate(info)) = transaction.commit_with_hook(&mut hook) else {
        panic!("post-install hook panic must remain indeterminate")
    };
    assert_eq!(info.phase(), FailurePhase::PostInstallHook);
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Indeterminate);
    assert!(finish_events(&fixture.harness.events()).is_empty());
}

pub(super) fn singleton_drop_only_teardown_failure() {
    let fixture = drop_only_direct_fixture(vec![(Callback::IntentDrop, 1, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_tracked_direct_read_write(&mut transaction, &fixture.resource, 1);
    let Err(CommitFailure::Poisoned {
        outcome: DefiniteOutcome::Committed(_),
        info,
    }) = transaction.commit()
    else {
        panic!("drop-only singleton teardown must preserve its published commit")
    };
    assert_eq!(info.phase(), FailurePhase::Finish);
    assert!(finish_events(&fixture.harness.events()).is_empty());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

struct LatePanicPayload {
    harness: Arc<Harness>,
    irreversible: bool,
    panic_on_drop: bool,
    dropped: Arc<AtomicUsize>,
}

impl Drop for LatePanicPayload {
    fn drop(&mut self) {
        let events = self.harness.events();
        assert_eq!(
            release_events(&events).len(),
            1,
            "the original catch result outlives lock cleanup"
        );
        assert_eq!(
            finish_events(&events).len(),
            usize::from(!self.irreversible),
            "reversible cleanup must finish before the panic payload drops"
        );
        self.dropped.fetch_add(1, Ordering::Relaxed);
        assert!(
            !self.panic_on_drop,
            "injected panic-payload destructor panic"
        );
    }
}

struct PayloadHook {
    harness: Arc<Harness>,
    phase: FailurePhase,
    panic_on_drop: bool,
    dropped: Arc<AtomicUsize>,
}

impl PayloadHook {
    fn invoke(&self, phase: FailurePhase) {
        if self.phase == phase {
            std::panic::panic_any(LatePanicPayload {
                harness: Arc::clone(&self.harness),
                irreversible: phase == FailurePhase::PostInstallHook,
                panic_on_drop: self.panic_on_drop,
                dropped: Arc::clone(&self.dropped),
            });
        }
    }
}

impl CommitHook for PayloadHook {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        self.invoke(FailurePhase::UpperMetadata);
        Ok(())
    }
    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        self.invoke(FailurePhase::PreinstallHook);
        Ok(())
    }
    fn post_install(&mut self) {
        self.invoke(FailurePhase::PostInstallHook);
    }
}

#[test]
fn singleton_panic_payload_outlives_abort_cleanup_even_when_its_drop_panics() {
    for phase in [FailurePhase::UpperMetadata, FailurePhase::PreinstallHook] {
        for panic_on_drop in [false, true] {
            let fixture = direct_fixture(Vec::new());
            let mut worker = fixture.runtime.attach().unwrap();
            let mut transaction = worker.begin().unwrap();
            stage_direct_read_write(&mut transaction, &fixture.resource, 1);
            let mut hook = PayloadHook {
                harness: Arc::clone(&fixture.harness),
                phase,
                panic_on_drop,
                dropped: Arc::new(AtomicUsize::new(0)),
            };
            assert_poisoned_aborted(transaction.commit_with_hook(&mut hook), phase);
            assert_eq!(hook.dropped.load(Ordering::Relaxed), 1);
            assert_abort_cleanup(&fixture.harness, true);
            assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
            assert!(worker.begin().is_err());
        }
    }
}
