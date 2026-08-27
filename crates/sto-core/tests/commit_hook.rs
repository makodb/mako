use sto_core::{
    AbortReason, CapacityError, CommitFailure, CommitHook, CommitHookError, CommitOutcome,
    DefiniteOutcome, FailurePhase, Runtime, RuntimeConfig, RuntimeHealth, TxnCell,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Event {
    Reserve,
    PreInstall,
}

#[derive(Clone, Copy)]
enum Behavior {
    Accept,
    Reject,
    Capacity,
    Panic,
}

struct TraceHook {
    reserve: Behavior,
    pre_install: Behavior,
    events: Vec<Event>,
}

impl TraceHook {
    fn new(reserve: Behavior, pre_install: Behavior) -> Self {
        Self {
            reserve,
            pre_install,
            events: Vec::new(),
        }
    }

    fn apply(behavior: Behavior) -> Result<(), CommitHookError> {
        match behavior {
            Behavior::Accept => Ok(()),
            Behavior::Reject => Err(CommitHookError::Rejected),
            Behavior::Capacity => Err(CapacityError::BufferLimit.into()),
            Behavior::Panic => panic!("injected commit-hook panic"),
        }
    }
}

impl CommitHook for TraceHook {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        self.events.push(Event::Reserve);
        Self::apply(self.reserve)
    }

    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        self.events.push(Event::PreInstall);
        Self::apply(self.pre_install)
    }
}

#[test]
fn writing_commit_invokes_both_hook_phases_once_before_install() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    cell.set(&mut transaction, 2).unwrap();
    let mut hook = TraceHook::new(Behavior::Accept, Behavior::Accept);

    assert!(matches!(
        transaction.commit_with_hook(&mut hook).unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(hook.events, [Event::Reserve, Event::PreInstall]);

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 2);
    transaction.commit().unwrap();
}

#[test]
fn read_only_commit_skips_the_upper_hook() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 1);
    let mut hook = TraceHook::new(Behavior::Panic, Behavior::Panic);

    assert!(matches!(
        transaction.commit_with_hook(&mut hook).unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert!(hook.events.is_empty());
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn reservation_rejection_and_capacity_are_definite_aborts() {
    for (behavior, expected) in [
        (Behavior::Reject, AbortReason::HookRejected),
        (
            Behavior::Capacity,
            AbortReason::Capacity(CapacityError::BufferLimit),
        ),
    ] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, 1_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        cell.set(&mut transaction, 2).unwrap();
        let mut hook = TraceHook::new(behavior, Behavior::Panic);

        assert_eq!(
            transaction.commit_with_hook(&mut hook).unwrap(),
            CommitOutcome::Aborted(expected)
        );
        assert_eq!(hook.events, [Event::Reserve]);
        assert_eq!(runtime.health(), RuntimeHealth::Healthy);

        let mut transaction = worker.begin().unwrap();
        assert_eq!(cell.get(&mut transaction).unwrap(), 1);
        transaction.commit().unwrap();
    }
}

#[test]
fn pre_install_rejection_is_definite_and_installs_nothing() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    cell.set(&mut transaction, 2).unwrap();
    let mut hook = TraceHook::new(Behavior::Accept, Behavior::Reject);

    assert_eq!(
        transaction.commit_with_hook(&mut hook).unwrap(),
        CommitOutcome::Aborted(AbortReason::HookRejected)
    );
    assert_eq!(hook.events, [Event::Reserve, Event::PreInstall]);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 1);
    transaction.commit().unwrap();
}

#[test]
fn hook_panics_are_contained_as_poisoned_definite_aborts() {
    for (reserve, pre_install, expected_phase, expected_events) in [
        (
            Behavior::Panic,
            Behavior::Accept,
            FailurePhase::UpperMetadata,
            vec![Event::Reserve],
        ),
        (
            Behavior::Accept,
            Behavior::Panic,
            FailurePhase::PreinstallHook,
            vec![Event::Reserve, Event::PreInstall],
        ),
    ] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, 1_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        cell.set(&mut transaction, 2).unwrap();
        let mut hook = TraceHook::new(reserve, pre_install);

        let failure = transaction.commit_with_hook(&mut hook).unwrap_err();
        assert!(matches!(
            failure,
            CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(AbortReason::Internal(error)),
                info,
            } if error.phase() == expected_phase && info.phase() == expected_phase
        ));
        assert_eq!(hook.events, expected_events);
        assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    }
}
