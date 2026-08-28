use std::{
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{Arc, Mutex, MutexGuard},
};

use sto_core::{
    AbortReason, AccessError, AcquireContext, AcquireError, Active, AdapterFault, AdapterFaultKind,
    AdapterPhase, CheckError, CommitFailure, CommitHook, CommitHookError, CommitOutcome, Conflict,
    DefiniteOutcome, Entry, ExecutionCheckContext, FailurePhase, FinishContext, FinishDisposition,
    FinishItem, InstallContext, InstallItem, InternalError, ItemInitError, LockClass,
    LockDisposition, LockIdentity, LockNamespaceId, LockRequest, LockUse, ObservationOrder,
    OpacityToken, PoisonInfo, PredicateContext, PreflightContext, PreflightFreeReadCapability,
    PreflightFreeValidationContext, PreflightItem, PrepareError, RegisteredResource, ResourceClass,
    Runtime, RuntimeConfig, RuntimeHealth, Transaction, TransactionLock, TransactionalResource,
    TxnArray, UniqueItemKeys, ValidationContext,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Callback {
    ItemInit,
    Preflight,
    Acquire,
    PredicateUpgrade,
    Validation,
    PreflightFreeValidation,
    Install,
    ReleaseAborted,
    ReleaseCommitted,
    ReleaseIndeterminate,
    GuardDrop,
    TargetDrop,
    ObservationDrop,
    PredicateDrop,
    AdapterDrop,
    Finish,
    PreflightFreeFinish,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Action {
    Conflict,
    Fault,
    Panic,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Event {
    ItemInit(u64),
    Preflight(u64),
    Acquire(u64),
    PredicateUpgrade(u64),
    Validation(u64),
    PreflightFreeValidation(u64),
    Install(u64),
    Release(u64, LockDisposition),
    GuardDrop(u64),
    TargetDrop(u64),
    ObservationDrop(u64),
    PredicateDrop(u64),
    AdapterDrop,
    Finish {
        key: u64,
        disposition: FinishDisposition,
        prepared: bool,
    },
    PreflightFreeFinish(u64),
}

struct Harness {
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    actions: Vec<(Callback, u64, Action)>,
    events: Mutex<Vec<Event>>,
}

impl Harness {
    fn action(&self, callback: Callback, key: u64) -> Option<Action> {
        self.actions
            .iter()
            .find_map(|&(candidate, candidate_key, action)| {
                (candidate == callback && candidate_key == key).then_some(action)
            })
    }

    fn record(&self, event: Event) {
        recover_lock(&self.events).push(event);
    }

    fn events(&self) -> Vec<Event> {
        recover_lock(&self.events).clone()
    }

    fn clear(&self) {
        recover_lock(&self.events).clear();
    }
}

fn recover_lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

struct InjectGuard {
    key: u64,
    harness: Arc<Harness>,
}

impl Drop for InjectGuard {
    fn drop(&mut self) {
        self.harness.record(Event::GuardDrop(self.key));
        if self.harness.action(Callback::GuardDrop, self.key).is_some() {
            panic!("injected guard-drop panic");
        }
    }
}

struct InjectLock {
    key: u64,
    harness: Arc<Harness>,
}

impl Drop for InjectLock {
    fn drop(&mut self) {
        self.harness.record(Event::TargetDrop(self.key));
        if self
            .harness
            .action(Callback::TargetDrop, self.key)
            .is_some()
        {
            panic!("injected target-drop panic");
        }
    }
}

impl TransactionLock for InjectLock {
    type Guard = InjectGuard;

    fn try_acquire(
        &self,
        _identity: &LockIdentity,
        _cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        self.harness.record(Event::Acquire(self.key));
        match self.harness.action(Callback::Acquire, self.key) {
            None => Ok(InjectGuard {
                key: self.key,
                harness: Arc::clone(&self.harness),
            }),
            Some(Action::Conflict) => Err(Conflict::LockBusy.into()),
            Some(Action::Fault) => Err(injected_fault(AdapterPhase::Acquire).into()),
            Some(Action::Panic) => panic!("injected acquisition panic"),
        }
    }

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        _cx: &sto_core::ReleaseContext<'_>,
    ) {
        assert_eq!(guard.key, self.key);
        self.harness.record(Event::Release(self.key, disposition));
        let callback = match disposition {
            LockDisposition::Aborted => Callback::ReleaseAborted,
            LockDisposition::Committed { .. } => Callback::ReleaseCommitted,
            LockDisposition::Indeterminate { .. } => Callback::ReleaseIndeterminate,
        };
        if self.harness.action(callback, self.key).is_some() {
            panic!("injected release panic");
        }
    }
}

struct Token {
    drop_event: Option<(u64, Arc<Harness>)>,
}

impl Token {
    fn inert() -> Self {
        Self { drop_event: None }
    }

    fn upgraded(key: u64, harness: &Arc<Harness>) -> Self {
        Self {
            drop_event: Some((key, Arc::clone(harness))),
        }
    }
}

impl OpacityToken for Token {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Unordered
    }
}

impl Drop for Token {
    fn drop(&mut self) {
        let Some((key, harness)) = self.drop_event.as_ref() else {
            return;
        };
        harness.record(Event::ObservationDrop(*key));
        if harness.action(Callback::ObservationDrop, *key).is_some() {
            panic!("injected observation-drop panic");
        }
    }
}

struct Prepared {
    lock_use: Option<LockUse<InjectLock>>,
}

struct PredicateToken {
    key: u64,
    harness: Arc<Harness>,
}

impl OpacityToken for PredicateToken {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Unordered
    }
}

impl Drop for PredicateToken {
    fn drop(&mut self) {
        self.harness.record(Event::PredicateDrop(self.key));
        if self
            .harness
            .action(Callback::PredicateDrop, self.key)
            .is_some()
        {
            panic!("injected predicate-drop panic");
        }
    }
}

#[derive(Clone, Copy)]
enum PreflightFreeReadMode {
    Disabled,
    Callback,
    DropOnly,
}

struct InjectAdapter {
    harness: Arc<Harness>,
    preflight_free_reads: PreflightFreeReadMode,
}

impl Drop for InjectAdapter {
    fn drop(&mut self) {
        self.harness.record(Event::AdapterDrop);
        if self.harness.action(Callback::AdapterDrop, 0).is_some() {
            panic!("injected adapter-drop panic");
        }
    }
}

impl TransactionalResource for InjectAdapter {
    type Key = u64;
    type Local = ();
    type Observation = Token;
    type Predicate = PredicateToken;
    type Intent = ();
    type Prepared = Prepared;

    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        self.harness.record(Event::ItemInit(*key));
        match self.harness.action(Callback::ItemInit, *key) {
            None | Some(Action::Conflict) => Ok(()),
            Some(Action::Fault) => Err(injected_fault(AdapterPhase::ItemInit).into()),
            Some(Action::Panic) => panic!("injected item-init panic"),
        }
    }

    fn preflight_free_read_capability(&self) -> Option<&'static PreflightFreeReadCapability<Self>> {
        match self.preflight_free_reads {
            PreflightFreeReadMode::Disabled => None,
            PreflightFreeReadMode::Callback => Some(&INJECT_PREFLIGHT_FREE_READ),
            PreflightFreeReadMode::DropOnly => Some(&INJECT_DROP_ONLY_PREFLIGHT_FREE_READ),
        }
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        self.harness.record(Event::Preflight(*key));
        match self.harness.action(Callback::Preflight, *key) {
            None => {}
            Some(Action::Conflict) => return Err(Conflict::ReadValidation.into()),
            Some(Action::Fault) => return Err(injected_fault(AdapterPhase::Preflight).into()),
            Some(Action::Panic) => panic!("injected preflight panic"),
        }

        let lock_use = if item.intent().is_some() {
            let identity = LockIdentity::new(
                self.harness.runtime_id,
                self.harness.namespace,
                LockClass::new(1).unwrap(),
                *key,
            );
            let target = Arc::new(InjectLock {
                key: *key,
                harness: Arc::clone(&self.harness),
            });
            Some(cx.require_lock(LockRequest::new(identity, target))?)
        } else {
            None
        };
        Ok(Prepared { lock_use })
    }

    fn revalidate_read(
        &self,
        _key: &Self::Key,
        _observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        Ok(())
    }

    fn revalidate_predicate(
        &self,
        _key: &Self::Key,
        _predicate: &Self::Predicate,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        Ok(ObservationOrder::Unordered)
    }

    fn upgrade_predicate(
        &self,
        key: &Self::Key,
        _predicate: &Self::Predicate,
        _prepared: &Self::Prepared,
        _cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError> {
        self.harness.record(Event::PredicateUpgrade(*key));
        check_action(
            self.harness.action(Callback::PredicateUpgrade, *key),
            AdapterPhase::PredicateUpgrade,
            Conflict::PredicateValidation,
            "injected predicate-upgrade panic",
        )?;
        Ok(Token::upgraded(*key, &self.harness))
    }

    fn validate_read(
        &self,
        key: &Self::Key,
        _observation: &Self::Observation,
        _prepared: &Self::Prepared,
        _cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        self.harness.record(Event::Validation(*key));
        check_action(
            self.harness.action(Callback::Validation, *key),
            AdapterPhase::Validation,
            Conflict::ReadValidation,
            "injected validation panic",
        )
    }

    fn install(
        &self,
        key: &Self::Key,
        _item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let lock_use = prepared
            .lock_use
            .as_ref()
            .expect("write fixture must prepare a lock");
        let guard = cx
            .guard_mut(lock_use)
            .expect("fixture lock use must resolve");
        assert_eq!(guard.key, *key);
        self.harness.record(Event::Install(*key));
        if self.harness.action(Callback::Install, *key).is_some() {
            panic!("injected install panic");
        }
    }

    fn finish(
        &self,
        key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        prepared: Option<&mut Self::Prepared>,
        disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        self.harness.record(Event::Finish {
            key: *key,
            disposition,
            prepared: prepared.is_some(),
        });
        if self.harness.action(Callback::Finish, *key).is_some() {
            panic!("injected finish panic");
        }
        let _ = item.take_remaining_intent();
    }
}

static INJECT_PREFLIGHT_FREE_READ: PreflightFreeReadCapability<InjectAdapter> =
    PreflightFreeReadCapability::new(
        validate_inject_preflight_free_read,
        finish_inject_preflight_free_read,
    );

static INJECT_DROP_ONLY_PREFLIGHT_FREE_READ: PreflightFreeReadCapability<InjectAdapter> =
    PreflightFreeReadCapability::new_drop_only(validate_inject_preflight_free_read);

fn validate_inject_preflight_free_read(
    adapter: &InjectAdapter,
    key: &u64,
    _observation: &Token,
    _cx: &PreflightFreeValidationContext<'_>,
) -> Result<(), CheckError> {
    adapter.harness.record(Event::PreflightFreeValidation(*key));
    check_action(
        adapter
            .harness
            .action(Callback::PreflightFreeValidation, *key),
        AdapterPhase::Validation,
        Conflict::ReadValidation,
        "injected preflight-free validation panic",
    )
}

fn finish_inject_preflight_free_read(
    adapter: &InjectAdapter,
    key: &u64,
    mut item: FinishItem<'_, InjectAdapter>,
    _cx: &mut FinishContext<'_>,
) {
    adapter.harness.record(Event::PreflightFreeFinish(*key));
    if adapter
        .harness
        .action(Callback::PreflightFreeFinish, *key)
        .is_some()
    {
        panic!("injected preflight-free finish panic");
    }
    assert!(item.remaining_intent().is_none());
    let _ = item.take_remaining_intent();
}

fn injected_fault(phase: AdapterPhase) -> AdapterFault {
    AdapterFault::new(phase, AdapterFaultKind::Other("injected fault"))
}

fn check_action(
    action: Option<Action>,
    phase: AdapterPhase,
    conflict: Conflict,
    panic_message: &'static str,
) -> Result<(), CheckError> {
    match action {
        None => Ok(()),
        Some(Action::Conflict) => Err(conflict.into()),
        Some(Action::Fault) => Err(injected_fault(phase).into()),
        Some(Action::Panic) => panic!("{panic_message}"),
    }
}

struct Fixture {
    runtime: Arc<Runtime>,
    resource: RegisteredResource<InjectAdapter>,
    harness: Arc<Harness>,
}

fn fixture(actions: Vec<(Callback, u64, Action)>) -> Fixture {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_inject_resource(&runtime, 99, actions);
    Fixture {
        runtime,
        resource,
        harness,
    }
}

fn preflight_free_fixture(actions: Vec<(Callback, u64, Action)>) -> Fixture {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_inject_resource_with_mode(&runtime, 99, actions, true);
    Fixture {
        runtime,
        resource,
        harness,
    }
}

fn drop_only_preflight_free_fixture(actions: Vec<(Callback, u64, Action)>) -> Fixture {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_inject_resource_with_preflight_free_mode(
        &runtime,
        99,
        actions,
        PreflightFreeReadMode::DropOnly,
    );
    Fixture {
        runtime,
        resource,
        harness,
    }
}

fn register_inject_resource(
    runtime: &Arc<Runtime>,
    resource_class: u32,
    actions: Vec<(Callback, u64, Action)>,
) -> (RegisteredResource<InjectAdapter>, Arc<Harness>) {
    register_inject_resource_with_mode(runtime, resource_class, actions, false)
}

fn register_inject_resource_with_mode(
    runtime: &Arc<Runtime>,
    resource_class: u32,
    actions: Vec<(Callback, u64, Action)>,
    preflight_free_reads: bool,
) -> (RegisteredResource<InjectAdapter>, Arc<Harness>) {
    let preflight_free_reads = if preflight_free_reads {
        PreflightFreeReadMode::Callback
    } else {
        PreflightFreeReadMode::Disabled
    };
    register_inject_resource_with_preflight_free_mode(
        runtime,
        resource_class,
        actions,
        preflight_free_reads,
    )
}

fn register_inject_resource_with_preflight_free_mode(
    runtime: &Arc<Runtime>,
    resource_class: u32,
    actions: Vec<(Callback, u64, Action)>,
    preflight_free_reads: PreflightFreeReadMode,
) -> (RegisteredResource<InjectAdapter>, Arc<Harness>) {
    let object = runtime.register_object().unwrap();
    let harness = Arc::new(Harness {
        runtime_id: object.runtime_id(),
        namespace: LockNamespaceId::new(object.object_id().get()).unwrap(),
        actions,
        events: Mutex::new(Vec::new()),
    });
    let resource = object
        .register_resource(
            ResourceClass::new(resource_class).unwrap(),
            InjectAdapter {
                harness: Arc::clone(&harness),
                preflight_free_reads,
            },
        )
        .unwrap();
    (resource, harness)
}

fn stage_read(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| entry.record_read(Token::inert()))
        .unwrap();
}

fn stage_tracked_read(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| {
            entry.record_read(Token::upgraded(key, &resource.adapter().harness))
        })
        .unwrap();
}

fn stage_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| entry.stage(()))
        .unwrap();
}

fn stage_read_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| {
            entry.record_read(Token::inert())?;
            entry.stage(())
        })
        .unwrap();
}

fn stage_predicate_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| {
            entry.record_predicate(PredicateToken {
                key,
                harness: Arc::clone(&resource.adapter().harness),
            })?;
            entry.stage(())
        })
        .unwrap();
}

fn stage_unique_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    keys: &[u64],
) {
    transaction
        .with_unique_item_batch(
            resource,
            UniqueItemKeys::try_new(keys).unwrap(),
            |_, entry| entry.stage(()),
        )
        .unwrap();
}

fn stage_unique_read_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    keys: &[u64],
) {
    transaction
        .with_unique_item_batch(
            resource,
            UniqueItemKeys::try_new(keys).unwrap(),
            |_, entry| {
                entry.record_read(Token::inert())?;
                entry.stage(())
            },
        )
        .unwrap();
}

fn stage_unique_predicate_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    keys: &[u64],
) {
    transaction
        .with_unique_item_batch(
            resource,
            UniqueItemKeys::try_new(keys).unwrap(),
            |index, entry| {
                let key = keys[index];
                entry.record_predicate(PredicateToken {
                    key,
                    harness: Arc::clone(&resource.adapter().harness),
                })?;
                entry.stage(())
            },
        )
        .unwrap();
}

fn stage_unique_read(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    keys: &[u64],
) {
    transaction
        .with_unique_item_batch(
            resource,
            UniqueItemKeys::try_new(keys).unwrap(),
            |_, entry| entry.record_read(Token::inert()),
        )
        .unwrap();
}

fn stage_unique_tracked_read(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<InjectAdapter>,
    keys: &[u64],
) {
    transaction
        .with_unique_item_batch(
            resource,
            UniqueItemKeys::try_new(keys).unwrap(),
            |index, entry| {
                let key = keys[index];
                entry.record_read(Token::upgraded(key, &resource.adapter().harness))
            },
        )
        .unwrap();
}

fn finish_events(events: &[Event]) -> Vec<(u64, FinishDisposition, bool)> {
    events
        .iter()
        .filter_map(|event| match event {
            Event::Finish {
                key,
                disposition,
                prepared,
            } => Some((*key, *disposition, *prepared)),
            _ => None,
        })
        .collect()
}

fn release_events(events: &[Event]) -> Vec<(u64, LockDisposition)> {
    events
        .iter()
        .filter_map(|event| match event {
            Event::Release(key, disposition) => Some((*key, *disposition)),
            _ => None,
        })
        .collect()
}

fn callback_keys(events: &[Event], callback: Callback) -> Vec<u64> {
    events
        .iter()
        .filter_map(|event| match (callback, event) {
            (Callback::Preflight, Event::Preflight(key))
            | (Callback::Acquire, Event::Acquire(key))
            | (Callback::PredicateUpgrade, Event::PredicateUpgrade(key))
            | (Callback::Validation, Event::Validation(key))
            | (Callback::PreflightFreeValidation, Event::PreflightFreeValidation(key))
            | (Callback::Install, Event::Install(key)) => Some(*key),
            _ => None,
        })
        .collect()
}

fn assert_poisoned_aborted(
    result: Result<CommitOutcome, CommitFailure>,
    expected_phase: FailurePhase,
) {
    let Err(CommitFailure::Poisoned { outcome, info }) = result else {
        panic!("expected a poisoned definite abort, got {result:?}");
    };
    assert!(matches!(outcome, DefiniteOutcome::Aborted(_)));
    assert_eq!(info.phase(), expected_phase);
}

struct ReadOnlyHookMustNotRun;

impl CommitHook for ReadOnlyHookMustNotRun {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        panic!("read-only fast path invoked upper metadata hook")
    }

    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        panic!("read-only fast path invoked pre-install hook")
    }
}

#[test]
fn explicit_abort_and_drop_finish_once_in_reverse_without_preflight() {
    let fixture = fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_write(&mut transaction, &fixture.resource, key);
    }
    assert_eq!(transaction.abort().reason(), &AbortReason::Explicit);
    assert_eq!(
        finish_events(&fixture.harness.events()),
        vec![
            (3, FinishDisposition::Aborted, false),
            (2, FinishDisposition::Aborted, false),
            (1, FinishDisposition::Aborted, false),
        ]
    );

    fixture.harness.clear();
    {
        let mut transaction = worker.begin().unwrap();
        for key in 1..=3 {
            stage_write(&mut transaction, &fixture.resource, key);
        }
    }
    assert_eq!(
        finish_events(&fixture.harness.events()),
        vec![
            (3, FinishDisposition::Aborted, false),
            (2, FinishDisposition::Aborted, false),
            (1, FinishDisposition::Aborted, false),
        ]
    );

    let transaction = worker.begin().unwrap();
    transaction.abort();
}

#[test]
fn normal_commit_releases_lock_order_and_finishes_reverse_item_order_once() {
    let fixture = fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [2, 3, 1] {
        stage_write(&mut transaction, &fixture.resource, key);
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Acquire), vec![1, 2, 3]);
    assert_eq!(callback_keys(&events, Callback::Install), vec![2, 3, 1]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [
            (3, LockDisposition::Committed { .. }),
            (2, LockDisposition::Committed { .. }),
            (1, LockDisposition::Committed { .. }),
        ]
    ));
    assert_eq!(
        finish_events(&events),
        vec![
            (1, FinishDisposition::Committed, true),
            (3, FinishDisposition::Committed, true),
            (2, FinishDisposition::Committed, true),
        ]
    );
    let last_release = events
        .iter()
        .rposition(|event| matches!(event, Event::Release(..)))
        .unwrap();
    let first_guard_drop = events
        .iter()
        .position(|event| matches!(event, Event::GuardDrop(_)))
        .unwrap();
    assert!(
        last_release < first_guard_drop,
        "inert guard destructors must run only after every lock is released"
    );
    for key in 1..=3 {
        assert_eq!(
            events
                .iter()
                .filter(|event| matches!(event, Event::GuardDrop(candidate) if *candidate == key))
                .count(),
            1
        );
    }
}

#[test]
fn swallowed_access_error_dooms_and_finishes_the_inserted_item() {
    let fixture = fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let result: Result<(), AccessError> = transaction.with_item(
        &fixture.resource,
        7,
        |_entry: &mut Entry<'_, InjectAdapter>| Err(Conflict::ReadValidation.into()),
    );
    assert!(matches!(result, Err(AccessError::Conflict(_))));
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Doomed)
    );
    assert_eq!(
        finish_events(&fixture.harness.events()),
        vec![(7, FinishDisposition::Aborted, false)]
    );
}

#[test]
fn swallowed_fault_poison_and_internal_access_errors_poison_definite_abort() {
    let errors = [
        AccessError::Fault(injected_fault(AdapterPhase::Execute)),
        AccessError::Poisoned(PoisonInfo::new(
            FailurePhase::Execution,
            "injected operation poison",
        )),
        AccessError::Internal(InternalError::new(
            FailurePhase::Execution,
            "injected operation internal failure",
        )),
    ];
    for error in errors {
        let fixture = fixture(Vec::new());
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let result: Result<(), AccessError> =
            transaction.with_item(&fixture.resource, 8, |_entry| Err(error));
        assert_eq!(result, Err(error));
        assert!(transaction.is_doomed());
        assert_poisoned_aborted(transaction.commit(), FailurePhase::Execution);
        assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
        assert_eq!(
            finish_events(&fixture.harness.events()),
            vec![(8, FinishDisposition::Aborted, false)]
        );
    }
}

#[test]
fn item_init_fault_and_panic_are_contained_and_doom_the_transaction() {
    for action in [Action::Fault, Action::Panic] {
        let fixture = fixture(vec![(Callback::ItemInit, 1, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let access = catch_unwind(AssertUnwindSafe(|| {
            transaction.with_item(&fixture.resource, 1, |_entry| Ok(()))
        }));
        assert!(matches!(access, Ok(Err(AccessError::Fault(_)))));
        assert!(transaction.is_doomed());
        assert_poisoned_aborted(transaction.commit(), FailurePhase::Execution);
        assert!(finish_events(&fixture.harness.events()).is_empty());
    }
}

#[test]
fn unique_batch_item_init_failure_aborts_only_the_initialized_prefix() {
    for action in [Action::Fault, Action::Panic] {
        let fixture = fixture(vec![(Callback::ItemInit, 2, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let keys = [1_u64, 2, 3];
        let unique = UniqueItemKeys::try_new(&keys).unwrap();
        let access = catch_unwind(AssertUnwindSafe(|| {
            transaction.with_unique_item_batch(&fixture.resource, unique, |_, _| Ok(()))
        }));
        assert!(matches!(access, Ok(Err(AccessError::Fault(_)))));
        assert!(transaction.is_doomed());
        assert_poisoned_aborted(transaction.commit(), FailurePhase::Execution);

        let events = fixture.harness.events();
        assert_eq!(
            events
                .iter()
                .filter_map(|event| match event {
                    Event::ItemInit(key) => Some(*key),
                    _ => None,
                })
                .collect::<Vec<_>>(),
            vec![1, 2]
        );
        assert_eq!(
            finish_events(&events),
            vec![(1, FinishDisposition::Aborted, false)]
        );
    }
}

#[test]
fn unique_batch_commit_preserves_phase_lock_and_reverse_finish_order() {
    let fixture = fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_read_write(&mut transaction, &fixture.resource, &[2, 3, 1]);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Preflight), vec![2, 3, 1]);
    assert_eq!(callback_keys(&events, Callback::Acquire), vec![1, 2, 3]);
    assert_eq!(callback_keys(&events, Callback::Validation), vec![2, 3, 1]);
    assert_eq!(callback_keys(&events, Callback::Install), vec![2, 3, 1]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [
            (3, LockDisposition::Committed { .. }),
            (2, LockDisposition::Committed { .. }),
            (1, LockDisposition::Committed { .. }),
        ]
    ));
    assert_eq!(
        finish_events(&events),
        vec![
            (1, FinishDisposition::Committed, true),
            (3, FinishDisposition::Committed, true),
            (2, FinishDisposition::Committed, true),
        ]
    );
}

#[test]
fn unique_batch_preflight_failure_tracks_typed_prefix_and_outcome() {
    for action in [Action::Conflict, Action::Fault, Action::Panic] {
        let fixture = fixture(vec![(Callback::Preflight, 2, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_unique_write(&mut transaction, &fixture.resource, &[1, 2, 3]);

        let result = transaction.commit();
        match action {
            Action::Conflict => assert_eq!(
                result.unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
            ),
            Action::Fault | Action::Panic => {
                assert_poisoned_aborted(result, FailurePhase::Preflight)
            }
        }

        let events = fixture.harness.events();
        assert_eq!(callback_keys(&events, Callback::Preflight), vec![1, 2]);
        assert!(callback_keys(&events, Callback::Acquire).is_empty());
        assert!(callback_keys(&events, Callback::Install).is_empty());
        assert_eq!(
            finish_events(&events),
            vec![
                (3, FinishDisposition::Aborted, false),
                (2, FinishDisposition::Aborted, false),
                (1, FinishDisposition::Aborted, true),
            ]
        );
    }
}

#[test]
fn unique_batch_predicate_and_validation_callbacks_preserve_failure_semantics() {
    let success = fixture(Vec::new());
    let mut worker = success.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_predicate_write(&mut transaction, &success.resource, &[3, 1, 2]);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = success.harness.events();
    assert_eq!(
        callback_keys(&events, Callback::PredicateUpgrade),
        vec![3, 1, 2]
    );
    assert_eq!(callback_keys(&events, Callback::Validation), vec![3, 1, 2]);
    assert_eq!(callback_keys(&events, Callback::Install), vec![3, 1, 2]);
    assert_eq!(
        finish_events(&events),
        vec![
            (2, FinishDisposition::Committed, true),
            (1, FinishDisposition::Committed, true),
            (3, FinishDisposition::Committed, true),
        ]
    );

    for (callback, expected_phase, expected_conflict) in [
        (
            Callback::PredicateUpgrade,
            FailurePhase::PredicateUpgrade,
            Conflict::PredicateValidation,
        ),
        (
            Callback::Validation,
            FailurePhase::Validation,
            Conflict::ReadValidation,
        ),
    ] {
        for action in [Action::Conflict, Action::Fault, Action::Panic] {
            let fixture = fixture(vec![(callback, 2, action)]);
            let mut worker = fixture.runtime.attach().unwrap();
            let mut transaction = worker.begin().unwrap();
            if callback == Callback::PredicateUpgrade {
                stage_unique_predicate_write(&mut transaction, &fixture.resource, &[1, 2, 3]);
            } else {
                stage_unique_read_write(&mut transaction, &fixture.resource, &[1, 2, 3]);
            }

            let result = transaction.commit();
            match action {
                Action::Conflict => assert_eq!(
                    result.unwrap(),
                    CommitOutcome::Aborted(AbortReason::Conflict(expected_conflict))
                ),
                Action::Fault | Action::Panic => assert_poisoned_aborted(result, expected_phase),
            }
            let events = fixture.harness.events();
            assert!(callback_keys(&events, Callback::Install).is_empty());
            assert_eq!(
                release_events(&events),
                vec![
                    (3, LockDisposition::Aborted),
                    (2, LockDisposition::Aborted),
                    (1, LockDisposition::Aborted),
                ]
            );
            assert_eq!(
                finish_events(&events),
                vec![
                    (3, FinishDisposition::Aborted, true),
                    (2, FinishDisposition::Aborted, true),
                    (1, FinishDisposition::Aborted, true),
                ]
            );
        }
    }
}

#[test]
fn unique_batch_install_and_finish_panics_keep_boundary_classification() {
    let install = fixture(vec![(Callback::Install, 2, Action::Panic)]);
    let mut worker = install.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_write(&mut transaction, &install.resource, &[1, 2, 3]);
    let Err(CommitFailure::Indeterminate(info)) = transaction.commit() else {
        panic!("typed install panic must be indeterminate");
    };
    assert_eq!(info.phase(), FailurePhase::Install);
    let events = install.harness.events();
    assert_eq!(callback_keys(&events, Callback::Install), vec![1, 2]);
    assert!(release_events(&events)
        .iter()
        .all(|(_, disposition)| matches!(disposition, LockDisposition::Indeterminate { .. })));
    assert!(finish_events(&events).is_empty());
    assert_eq!(install.runtime.health(), RuntimeHealth::Indeterminate);

    let finish = fixture(vec![(Callback::Finish, 2, Action::Panic)]);
    let mut worker = finish.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_write(&mut transaction, &finish.resource, &[1, 2, 3]);
    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("typed finish panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    assert_eq!(
        finish_events(&finish.harness.events()),
        vec![
            (3, FinishDisposition::Committed, true),
            (2, FinishDisposition::Committed, true),
        ]
    );
    assert_eq!(finish.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn unique_batch_preflight_free_path_is_typed_ordered_and_contained() {
    let success = preflight_free_fixture(Vec::new());
    let mut worker = success.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_read(&mut transaction, &success.resource, &[2, 3, 1]);
    let mut hook = ReadOnlyHookMustNotRun;
    let CommitOutcome::Committed(info) = transaction.commit_with_hook(&mut hook).unwrap() else {
        panic!("typed preflight-free batch must commit");
    };
    assert_eq!(info.occ_commit_id(), None);
    let events = success.harness.events();
    assert_eq!(
        callback_keys(&events, Callback::PreflightFreeValidation),
        vec![2, 3, 1]
    );
    assert_eq!(
        events
            .iter()
            .filter_map(|event| match event {
                Event::PreflightFreeFinish(key) => Some(*key),
                _ => None,
            })
            .collect::<Vec<_>>(),
        vec![1, 3, 2]
    );
    assert!(callback_keys(&events, Callback::Preflight).is_empty());
    assert!(finish_events(&events).is_empty());

    for action in [Action::Conflict, Action::Fault, Action::Panic] {
        let fixture = preflight_free_fixture(vec![(Callback::PreflightFreeValidation, 2, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_unique_read(&mut transaction, &fixture.resource, &[1, 2, 3]);
        let result = transaction.commit();
        match action {
            Action::Conflict => assert_eq!(
                result.unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
            ),
            Action::Fault | Action::Panic => {
                assert_poisoned_aborted(result, FailurePhase::Validation)
            }
        }
        let events = fixture.harness.events();
        assert_eq!(
            callback_keys(&events, Callback::PreflightFreeValidation),
            vec![1, 2]
        );
        assert_eq!(
            finish_events(&events),
            vec![
                (3, FinishDisposition::Aborted, false),
                (2, FinishDisposition::Aborted, false),
                (1, FinishDisposition::Aborted, false),
            ]
        );
        assert!(events
            .iter()
            .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
    }

    let finish = preflight_free_fixture(vec![(Callback::PreflightFreeFinish, 2, Action::Panic)]);
    let mut worker = finish.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_read(&mut transaction, &finish.resource, &[1, 2, 3]);
    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("typed preflight-free finish panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    assert_eq!(
        finish
            .harness
            .events()
            .iter()
            .filter_map(|event| match event {
                Event::PreflightFreeFinish(key) => Some(*key),
                _ => None,
            })
            .collect::<Vec<_>>(),
        vec![3, 2]
    );
}

#[test]
fn preflight_failure_at_item_n_tracks_prepared_state_and_reverse_finish() {
    for action in [Action::Conflict, Action::Fault, Action::Panic] {
        let fixture = fixture(vec![(Callback::Preflight, 2, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        for key in 1..=3 {
            stage_write(&mut transaction, &fixture.resource, key);
        }

        let result = transaction.commit();
        match action {
            Action::Conflict => assert_eq!(
                result.unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
            ),
            Action::Fault | Action::Panic => {
                assert_poisoned_aborted(result, FailurePhase::Preflight)
            }
        }

        let events = fixture.harness.events();
        assert_eq!(callback_keys(&events, Callback::Preflight), vec![1, 2]);
        assert_eq!(
            finish_events(&events),
            vec![
                (3, FinishDisposition::Aborted, false),
                (2, FinishDisposition::Aborted, false),
                (1, FinishDisposition::Aborted, true),
            ]
        );
        assert!(callback_keys(&events, Callback::Acquire).is_empty());
        assert!(callback_keys(&events, Callback::Install).is_empty());
    }
}

#[test]
fn preflight_free_read_keeps_final_certification_and_mixes_with_normal_locks() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (fast, fast_harness) = register_inject_resource_with_mode(&runtime, 90, Vec::new(), true);
    let (normal, normal_harness) = register_inject_resource(&runtime, 91, Vec::new());
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read(&mut transaction, &fast, 11);
    stage_read_write(&mut transaction, &normal, 22);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let fast_events = fast_harness.events();
    assert!(callback_keys(&fast_events, Callback::Preflight).is_empty());
    assert!(callback_keys(&fast_events, Callback::Validation).is_empty());
    assert_eq!(
        callback_keys(&fast_events, Callback::PreflightFreeValidation),
        vec![11]
    );
    assert!(finish_events(&fast_events).is_empty());
    assert_eq!(
        fast_events
            .iter()
            .filter_map(|event| match event {
                Event::PreflightFreeFinish(key) => Some(*key),
                _ => None,
            })
            .collect::<Vec<_>>(),
        vec![11]
    );

    let normal_events = normal_harness.events();
    assert_eq!(callback_keys(&normal_events, Callback::Preflight), vec![22]);
    assert_eq!(callback_keys(&normal_events, Callback::Acquire), vec![22]);
    assert_eq!(
        callback_keys(&normal_events, Callback::Validation),
        vec![22]
    );
    assert!(matches!(
        release_events(&normal_events).as_slice(),
        [(22, LockDisposition::Committed { .. })]
    ));
    assert_eq!(
        finish_events(&normal_events),
        vec![(22, FinishDisposition::Committed, true)]
    );
}

#[test]
fn drop_only_preflight_free_read_mixes_with_normal_write_cleanup() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (fast, fast_harness) = register_inject_resource_with_preflight_free_mode(
        &runtime,
        92,
        Vec::new(),
        PreflightFreeReadMode::DropOnly,
    );
    let (normal, normal_harness) = register_inject_resource(&runtime, 93, Vec::new());
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_tracked_read(&mut transaction, &fast, 11);
    stage_read_write(&mut transaction, &normal, 22);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let fast_events = fast_harness.events();
    assert!(callback_keys(&fast_events, Callback::Preflight).is_empty());
    assert!(callback_keys(&fast_events, Callback::Validation).is_empty());
    assert_eq!(
        callback_keys(&fast_events, Callback::PreflightFreeValidation),
        vec![11]
    );
    assert!(finish_events(&fast_events).is_empty());
    assert!(fast_events
        .iter()
        .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
    assert_eq!(
        fast_events
            .iter()
            .filter(|event| matches!(event, Event::ObservationDrop(11)))
            .count(),
        1
    );

    let normal_events = normal_harness.events();
    assert_eq!(callback_keys(&normal_events, Callback::Preflight), vec![22]);
    assert_eq!(callback_keys(&normal_events, Callback::Acquire), vec![22]);
    assert_eq!(
        callback_keys(&normal_events, Callback::Validation),
        vec![22]
    );
    assert_eq!(callback_keys(&normal_events, Callback::Install), vec![22]);
    assert!(matches!(
        release_events(&normal_events).as_slice(),
        [(22, LockDisposition::Committed { .. })]
    ));
    assert_eq!(
        finish_events(&normal_events),
        vec![(22, FinishDisposition::Committed, true)]
    );
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn wholly_preflight_free_read_commit_uses_only_certification_and_committed_finish() {
    let fixture = preflight_free_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read(&mut transaction, &fixture.resource, 1);
    stage_read(&mut transaction, &fixture.resource, 2);
    let mut hook = ReadOnlyHookMustNotRun;

    let CommitOutcome::Committed(info) = transaction.commit_with_hook(&mut hook).unwrap() else {
        panic!("prepared-free read-only transaction must commit");
    };
    assert_eq!(info.occ_commit_id(), None);
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);

    let events = fixture.harness.events();
    assert!(callback_keys(&events, Callback::Preflight).is_empty());
    assert!(callback_keys(&events, Callback::Acquire).is_empty());
    assert!(callback_keys(&events, Callback::Validation).is_empty());
    assert!(release_events(&events).is_empty());
    assert_eq!(
        callback_keys(&events, Callback::PreflightFreeValidation),
        vec![1, 2]
    );
    assert_eq!(
        events
            .iter()
            .filter_map(|event| match event {
                Event::PreflightFreeFinish(key) => Some(*key),
                _ => None,
            })
            .collect::<Vec<_>>(),
        vec![2, 1]
    );
    assert!(finish_events(&events).is_empty());

    // Successful fast commits recycle the frame and its typed item boxes for
    // the attached worker just like the full protocol.
    fixture.harness.clear();
    let mut transaction = worker.begin().unwrap();
    stage_read(&mut transaction, &fixture.resource, 3);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(
        callback_keys(&fixture.harness.events(), Callback::PreflightFreeValidation),
        vec![3]
    );
}

#[test]
fn drop_only_preflight_free_commit_skips_callbacks_and_tears_down_exactly_once() {
    for typed_batch in [false, true] {
        let fixture = drop_only_preflight_free_fixture(Vec::new());
        let Fixture {
            runtime,
            resource,
            harness,
        } = fixture;
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        if typed_batch {
            stage_unique_tracked_read(&mut transaction, &resource, &[1, 2]);
        } else {
            stage_tracked_read(&mut transaction, &resource, 1);
            stage_tracked_read(&mut transaction, &resource, 2);
        }

        assert!(matches!(
            transaction.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
        let events = harness.events();
        assert_eq!(
            callback_keys(&events, Callback::PreflightFreeValidation),
            vec![1, 2]
        );
        assert!(finish_events(&events).is_empty());
        assert!(events
            .iter()
            .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
        assert_eq!(
            events
                .iter()
                .filter_map(|event| match event {
                    Event::ObservationDrop(key) => Some(*key),
                    _ => None,
                })
                .collect::<Vec<_>>(),
            vec![2, 1]
        );
        assert!(!events.contains(&Event::AdapterDrop));

        // Core teardown keeps the binding alive in the worker-local pool.
        // Dropping the public handle therefore cannot destroy the adapter
        // before every adapter-owned item field has been torn down.
        drop(resource);
        assert!(!harness.events().contains(&Event::AdapterDrop));
        drop(worker);
        assert_eq!(
            harness
                .events()
                .iter()
                .filter(|event| matches!(event, Event::AdapterDrop))
                .count(),
            1
        );
        drop(runtime);
    }
}

#[test]
fn drop_only_preflight_free_abort_still_runs_regular_finish_once() {
    let fixture = drop_only_preflight_free_fixture(vec![(
        Callback::PreflightFreeValidation,
        2,
        Action::Conflict,
    )]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_tracked_read(&mut transaction, &fixture.resource, key);
    }

    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    let events = fixture.harness.events();
    assert_eq!(
        callback_keys(&events, Callback::PreflightFreeValidation),
        vec![1, 2]
    );
    assert_eq!(
        finish_events(&events),
        vec![
            (3, FinishDisposition::Aborted, false),
            (2, FinishDisposition::Aborted, false),
            (1, FinishDisposition::Aborted, false),
        ]
    );
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
    assert_eq!(
        events
            .iter()
            .filter_map(|event| match event {
                Event::ObservationDrop(key) => Some(*key),
                _ => None,
            })
            .collect::<Vec<_>>(),
        vec![3, 2, 1]
    );

    let panicking = drop_only_preflight_free_fixture(vec![
        (Callback::PreflightFreeValidation, 2, Action::Conflict),
        (Callback::Finish, 3, Action::Panic),
    ]);
    let mut worker = panicking.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_tracked_read(&mut transaction, &panicking.resource, key);
    }
    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("drop-only abort finish panic must be contained");
    };
    assert!(matches!(outcome, DefiniteOutcome::Aborted(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    assert_eq!(
        finish_events(&panicking.harness.events()),
        vec![(3, FinishDisposition::Aborted, false)]
    );
    assert_eq!(panicking.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn drop_only_preflight_free_teardown_panic_is_contained_after_commit() {
    let fixture =
        drop_only_preflight_free_fixture(vec![(Callback::ObservationDrop, 1, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_tracked_read(&mut transaction, &fixture.resource, 1);

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("drop-only teardown panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    let events = fixture.harness.events();
    assert!(finish_events(&events).is_empty());
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
    assert_eq!(
        events
            .iter()
            .filter(|event| matches!(event, Event::ObservationDrop(1)))
            .count(),
        1
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn empty_transaction_commits_without_a_lock_plan_or_hook() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let mut worker = runtime.attach().unwrap();
    let transaction = worker.begin().unwrap();
    let mut hook = ReadOnlyHookMustNotRun;

    let CommitOutcome::Committed(info) = transaction.commit_with_hook(&mut hook).unwrap() else {
        panic!("empty transaction must commit");
    };
    assert_eq!(info.occ_commit_id(), None);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);

    // The worker remains reusable after the vacuous fast commit.
    let transaction = worker.begin().unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn preflight_free_capability_falls_back_for_intent_predicate_and_unobserved_items() {
    let fixture = preflight_free_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read_write(&mut transaction, &fixture.resource, 1);
    transaction
        .with_item(&fixture.resource, 2, |entry| {
            entry.record_predicate(PredicateToken {
                key: 2,
                harness: Arc::clone(&fixture.harness),
            })
        })
        .unwrap();
    transaction
        .with_item(&fixture.resource, 3, |_entry| Ok(()))
        .unwrap();

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Preflight), vec![1, 2, 3]);
    assert!(callback_keys(&events, Callback::PreflightFreeValidation).is_empty());
    assert_eq!(callback_keys(&events, Callback::Validation), vec![1, 2]);
    assert_eq!(
        finish_events(&events),
        vec![
            (3, FinishDisposition::Committed, true),
            (2, FinishDisposition::Committed, true),
            (1, FinishDisposition::Committed, true),
        ]
    );
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
}

#[test]
fn preflight_free_validation_conflict_fault_and_panic_keep_definite_abort() {
    for action in [Action::Conflict, Action::Fault, Action::Panic] {
        let fixture = preflight_free_fixture(vec![(Callback::PreflightFreeValidation, 1, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_read(&mut transaction, &fixture.resource, 1);

        let result = transaction.commit();
        match action {
            Action::Conflict => {
                assert_eq!(
                    result.unwrap(),
                    CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
                );
                assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
            }
            Action::Fault | Action::Panic => {
                assert_poisoned_aborted(result, FailurePhase::Validation);
                assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
            }
        }

        let events = fixture.harness.events();
        assert!(callback_keys(&events, Callback::Preflight).is_empty());
        assert!(callback_keys(&events, Callback::Acquire).is_empty());
        assert!(release_events(&events).is_empty());
        assert_eq!(
            callback_keys(&events, Callback::PreflightFreeValidation),
            vec![1]
        );
        assert_eq!(
            finish_events(&events),
            vec![(1, FinishDisposition::Aborted, false)]
        );
        assert!(events
            .iter()
            .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
    }
}

#[test]
fn later_fallback_conflict_aborts_an_already_certified_preflight_free_read() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (fast, fast_harness) = register_inject_resource_with_mode(&runtime, 92, Vec::new(), true);
    let (normal, normal_harness) = register_inject_resource(
        &runtime,
        93,
        vec![(Callback::Validation, 2, Action::Conflict)],
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read(&mut transaction, &fast, 1);
    stage_read_write(&mut transaction, &normal, 2);

    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    assert_eq!(
        callback_keys(&fast_harness.events(), Callback::PreflightFreeValidation),
        vec![1]
    );
    assert_eq!(
        finish_events(&fast_harness.events()),
        vec![(1, FinishDisposition::Aborted, false)]
    );
    assert!(fast_harness
        .events()
        .iter()
        .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
    assert_eq!(
        release_events(&normal_harness.events()),
        vec![(2, LockDisposition::Aborted)]
    );
}

#[test]
fn preflight_free_committed_finish_panic_is_definitely_committed_and_poisoned() {
    let fixture = preflight_free_fixture(vec![(Callback::PreflightFreeFinish, 1, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read(&mut transaction, &fixture.resource, 1);

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("preflight-free committed finish panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    let events = fixture.harness.events();
    assert!(finish_events(&events).is_empty());
    assert_eq!(
        events
            .iter()
            .filter_map(|event| match event {
                Event::PreflightFreeFinish(key) => Some(*key),
                _ => None,
            })
            .collect::<Vec<_>>(),
        vec![1]
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn explicit_abort_never_selects_preflight_free_commit_callbacks() {
    let fixture = preflight_free_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read(&mut transaction, &fixture.resource, 1);
    transaction.abort();

    let events = fixture.harness.events();
    assert!(callback_keys(&events, Callback::Preflight).is_empty());
    assert!(callback_keys(&events, Callback::PreflightFreeValidation).is_empty());
    assert_eq!(
        finish_events(&events),
        vec![(1, FinishDisposition::Aborted, false)]
    );
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::PreflightFreeFinish(_))));
}

#[test]
fn acquisition_failures_release_prior_guards_and_panic_quarantines_items() {
    for action in [Action::Conflict, Action::Fault, Action::Panic] {
        let fixture = fixture(vec![(Callback::Acquire, 2, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        for key in 1..=3 {
            stage_write(&mut transaction, &fixture.resource, key);
        }

        let result = transaction.commit();
        match action {
            Action::Conflict => assert_eq!(
                result.unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::LockBusy))
            ),
            Action::Fault | Action::Panic => assert_poisoned_aborted(result, FailurePhase::Acquire),
        }

        let events = fixture.harness.events();
        assert_eq!(callback_keys(&events, Callback::Acquire), vec![1, 2]);
        assert_eq!(release_events(&events), vec![(1, LockDisposition::Aborted)]);
        if action == Action::Panic {
            assert!(
                finish_events(&events).is_empty(),
                "an uncertain acquisition frame forbids post-unlock finish"
            );
        } else {
            assert_eq!(
                finish_events(&events),
                vec![
                    (3, FinishDisposition::Aborted, true),
                    (2, FinishDisposition::Aborted, true),
                    (1, FinishDisposition::Aborted, true),
                ]
            );
        }
        assert_eq!(
            events
                .iter()
                .filter(|event| matches!(event, Event::GuardDrop(1)))
                .count(),
            usize::from(action != Action::Panic),
            "a quarantined plan retains even guards that were made inert"
        );

        if action == Action::Conflict {
            let transaction = worker.begin().unwrap();
            transaction.abort();
        }
    }
}

#[test]
fn rollback_panic_during_acquisition_quarantines_items_and_releases_other_guards() {
    let fixture = fixture(vec![
        (Callback::Acquire, 3, Action::Conflict),
        (Callback::ReleaseAborted, 2, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_write(&mut transaction, &fixture.resource, key);
    }

    assert_poisoned_aborted(transaction.commit(), FailurePhase::Acquire);
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Acquire), vec![1, 2, 3]);
    assert_eq!(
        release_events(&events),
        vec![(2, LockDisposition::Aborted), (1, LockDisposition::Aborted),]
    );
    assert!(
        finish_events(&events).is_empty(),
        "the uncertain rollback frame forbids post-unlock finish"
    );
    assert_eq!(
        events
            .iter()
            .filter(|event| matches!(event, Event::GuardDrop(2)))
            .count(),
        0,
        "the uncertain rollback guard must remain quarantined"
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn predicate_upgrade_success_is_validated_and_conflict_installs_nothing() {
    let success = fixture(Vec::new());
    let mut worker = success.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_predicate_write(&mut transaction, &success.resource, 1);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = success.harness.events();
    assert_eq!(callback_keys(&events, Callback::PredicateUpgrade), vec![1]);
    assert_eq!(callback_keys(&events, Callback::Validation), vec![1]);
    assert_eq!(callback_keys(&events, Callback::Install), vec![1]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [(1, LockDisposition::Committed { .. })]
    ));
    assert_eq!(
        finish_events(&events),
        vec![(1, FinishDisposition::Committed, true)]
    );
    let release = events
        .iter()
        .rposition(|event| matches!(event, Event::Release(..)))
        .unwrap();
    let predicate_drop = events
        .iter()
        .position(|event| matches!(event, Event::PredicateDrop(1)))
        .unwrap();
    assert!(
        release < predicate_drop,
        "an upgraded predicate must be retained until every lock is released"
    );

    let conflict = fixture(vec![(Callback::PredicateUpgrade, 1, Action::Conflict)]);
    let mut worker = conflict.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_predicate_write(&mut transaction, &conflict.resource, 1);
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::PredicateValidation))
    );
    let events = conflict.harness.events();
    assert!(callback_keys(&events, Callback::Validation).is_empty());
    assert!(callback_keys(&events, Callback::Install).is_empty());
    assert_eq!(release_events(&events), vec![(1, LockDisposition::Aborted)]);
}

#[test]
fn observation_destructor_panic_retains_the_separate_upgraded_predicate() {
    let fixture = fixture(vec![
        (Callback::ObservationDrop, 1, Action::Panic),
        (Callback::PredicateDrop, 1, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_predicate_write(&mut transaction, &fixture.resource, 1);

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("observation destructor panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    let events = fixture.harness.events();
    assert_eq!(
        events
            .iter()
            .filter(|event| matches!(event, Event::ObservationDrop(1)))
            .count(),
        1
    );
    assert!(
        events
            .iter()
            .all(|event| !matches!(event, Event::PredicateDrop(1))),
        "the retained predicate must not unwind after observation Drop panics"
    );
}

#[test]
fn predicate_and_validation_faults_or_panics_abort_before_install() {
    for (callback, expected_phase) in [
        (Callback::PredicateUpgrade, FailurePhase::PredicateUpgrade),
        (Callback::Validation, FailurePhase::Validation),
    ] {
        for action in [Action::Fault, Action::Panic] {
            let fixture = fixture(vec![(callback, 1, action)]);
            let mut worker = fixture.runtime.attach().unwrap();
            let mut transaction = worker.begin().unwrap();
            if callback == Callback::PredicateUpgrade {
                stage_predicate_write(&mut transaction, &fixture.resource, 1);
            } else {
                stage_read_write(&mut transaction, &fixture.resource, 1);
            }
            assert_poisoned_aborted(transaction.commit(), expected_phase);
            let events = fixture.harness.events();
            assert!(callback_keys(&events, Callback::Install).is_empty());
            assert_eq!(release_events(&events), vec![(1, LockDisposition::Aborted)]);
            assert_eq!(
                finish_events(&events),
                vec![(1, FinishDisposition::Aborted, true)]
            );
        }
    }
}

#[test]
fn validation_conflict_is_a_definite_abort_and_installs_nothing() {
    let fixture = fixture(vec![(Callback::Validation, 1, Action::Conflict)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_read_write(&mut transaction, &fixture.resource, 1);
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    let events = fixture.harness.events();
    assert!(callback_keys(&events, Callback::Install).is_empty());
    assert_eq!(release_events(&events), vec![(1, LockDisposition::Aborted)]);
    let transaction = worker.begin().unwrap();
    transaction.abort();
}

#[test]
fn install_panic_is_indeterminate_and_never_uses_aborted_disposition() {
    let fixture = fixture(vec![(Callback::Install, 1, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &fixture.resource, 1);
    let result = transaction.commit();
    let Err(CommitFailure::Indeterminate(info)) = result else {
        panic!("expected indeterminate install failure, got {result:?}");
    };
    assert_eq!(info.phase(), FailurePhase::Install);
    assert!(info.occ_commit_id().is_some());
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Install), vec![1]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [(1, LockDisposition::Indeterminate { .. })]
    ));
    assert!(release_events(&events)
        .iter()
        .all(|(_, disposition)| *disposition != LockDisposition::Aborted));
    assert!(finish_events(&events).is_empty());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Indeterminate);
}

#[test]
fn release_panic_quarantines_uncertain_frame_and_advances_remaining_guards() {
    let fixture = fixture(vec![(Callback::ReleaseCommitted, 2, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_write(&mut transaction, &fixture.resource, key);
    }
    let result = transaction.commit();
    let Err(CommitFailure::Indeterminate(info)) = result else {
        panic!("expected indeterminate release failure, got {result:?}");
    };
    assert_eq!(info.phase(), FailurePhase::Release);

    let events = fixture.harness.events();
    let releases = release_events(&events);
    assert!(matches!(
        releases.as_slice(),
        [
            (3, LockDisposition::Committed { .. }),
            (2, LockDisposition::Committed { .. }),
            (1, LockDisposition::Indeterminate { .. }),
        ]
    ));
    assert!(releases
        .iter()
        .all(|(_, disposition)| *disposition != LockDisposition::Aborted));
    assert!(finish_events(&events).is_empty());
    assert_eq!(
        events
            .iter()
            .filter(|event| matches!(event, Event::GuardDrop(2)))
            .count(),
        0,
        "the uncertain release frame must remain quarantined"
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Indeterminate);
}

#[test]
fn finish_panic_after_publication_is_poisoned_but_definitely_committed() {
    let fixture = fixture(vec![(Callback::Finish, 2, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_write(&mut transaction, &fixture.resource, key);
    }
    let result = transaction.commit();
    let Err(CommitFailure::Poisoned { outcome, info }) = result else {
        panic!("expected poisoned committed outcome, got {result:?}");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);

    let events = fixture.harness.events();
    assert!(release_events(&events)
        .iter()
        .all(|(_, disposition)| matches!(disposition, LockDisposition::Committed { .. })));
    assert_eq!(
        finish_events(&events),
        vec![
            (3, FinishDisposition::Committed, true),
            (2, FinishDisposition::Committed, true),
        ]
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn guard_destructor_panic_is_contained_without_dropping_another_guard() {
    let fixture = fixture(vec![
        (Callback::GuardDrop, 1, Action::Panic),
        (Callback::GuardDrop, 2, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &fixture.resource, 1);
    stage_write(&mut transaction, &fixture.resource, 2);

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("guard destructor panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Release);
    let events = fixture.harness.events();
    let dropped: Vec<_> = events
        .iter()
        .filter_map(|event| match event {
            Event::GuardDrop(key) => Some(*key),
            _ => None,
        })
        .collect();
    assert_eq!(dropped, vec![2]);
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::TargetDrop(_))));
}

#[test]
fn target_destructor_runs_after_its_guard_and_stops_before_another_frame() {
    let fixture = fixture(vec![
        (Callback::TargetDrop, 1, Action::Panic),
        (Callback::TargetDrop, 2, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &fixture.resource, 1);
    stage_write(&mut transaction, &fixture.resource, 2);

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("target destructor panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Release);
    let teardown: Vec<_> = fixture
        .harness
        .events()
        .into_iter()
        .filter(|event| matches!(event, Event::GuardDrop(_) | Event::TargetDrop(_)))
        .collect();
    assert_eq!(teardown, vec![Event::GuardDrop(2), Event::TargetDrop(2)]);
}

#[test]
fn abort_release_panic_is_definitely_aborted_but_skips_finish_under_uncertain_lock() {
    let fixture = fixture(vec![
        (Callback::Validation, 3, Action::Conflict),
        (Callback::ReleaseAborted, 2, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_read_write(&mut transaction, &fixture.resource, key);
    }
    let result = transaction.commit();
    assert_poisoned_aborted(result, FailurePhase::Release);

    let events = fixture.harness.events();
    assert_eq!(
        release_events(&events),
        vec![
            (3, LockDisposition::Aborted),
            (2, LockDisposition::Aborted),
            (1, LockDisposition::Aborted),
        ]
    );
    assert!(finish_events(&events).is_empty());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn abort_finish_panic_is_poisoned_with_a_definite_aborted_outcome() {
    let fixture = fixture(vec![
        (Callback::Preflight, 1, Action::Conflict),
        (Callback::Finish, 2, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &fixture.resource, 1);
    stage_write(&mut transaction, &fixture.resource, 2);
    assert_poisoned_aborted(transaction.commit(), FailurePhase::Finish);
    assert_eq!(
        finish_events(&fixture.harness.events()),
        vec![(2, FinishDisposition::Aborted, false)]
    );
}

#[test]
fn pooled_adapter_destructor_panic_is_contained_when_worker_drops() {
    let Fixture {
        runtime,
        resource,
        harness,
    } = fixture(vec![(Callback::AdapterDrop, 0, Action::Panic)]);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    transaction.with_item(&resource, 1, |_| Ok(())).unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Leave the pooled worker binding as the adapter's last strong handle.
    drop(resource);
    let worker_drop = catch_unwind(AssertUnwindSafe(|| drop(worker)));
    assert!(worker_drop.is_ok());
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
}

#[test]
fn pooled_adapter_destructor_panic_is_contained_on_same_type_rebind() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_inject_resource(
        &runtime,
        97,
        vec![(Callback::AdapterDrop, 0, Action::Panic)],
    );
    let (second, _) = register_inject_resource(&runtime, 98, Vec::new());
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    transaction.with_item(&first, 1, |_| Ok(())).unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    drop(first);

    let mut transaction = worker.begin().unwrap();
    let result = transaction.with_item(&second, 2, |_| Ok(()));
    assert!(matches!(
        result,
        Err(AccessError::Fault(fault))
            if fault.phase() == AdapterPhase::ItemInit
                && matches!(fault.kind(), AdapterFaultKind::Panic)
    ));
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        first_harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
    drop(transaction);
}

#[test]
fn unique_batch_contains_pooled_adapter_panic_on_same_type_rebind() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_inject_resource(
        &runtime,
        95,
        vec![(Callback::AdapterDrop, 0, Action::Panic)],
    );
    let (second, _) = register_inject_resource(&runtime, 94, Vec::new());
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    transaction.with_item(&first, 1, |_| Ok(())).unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    drop(first);

    let mut transaction = worker.begin().unwrap();
    let keys = [2_u64];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();
    let result = transaction.with_unique_item_batch(&second, unique, |_, _| Ok(()));
    assert!(matches!(
        result,
        Err(AccessError::Fault(fault))
            if fault.phase() == AdapterPhase::ItemInit
                && matches!(fault.kind(), AdapterFaultKind::Panic)
    ));
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        first_harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
    drop(transaction);
}

#[test]
fn typed_unique_batch_pool_contains_same_type_rebind_panic() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_inject_resource(
        &runtime,
        93,
        vec![(Callback::AdapterDrop, 0, Action::Panic)],
    );
    let (second, _) = register_inject_resource(&runtime, 92, Vec::new());
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    stage_unique_write(&mut transaction, &first, &[1, 2]);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    drop(first);

    let mut transaction = worker.begin().unwrap();
    let keys = [3_u64, 4];
    let result = transaction.with_unique_item_batch(
        &second,
        UniqueItemKeys::try_new(&keys).unwrap(),
        |_, entry| entry.stage(()),
    );
    assert!(matches!(
        result,
        Err(AccessError::Fault(fault))
            if fault.phase() == AdapterPhase::ItemInit
                && matches!(fault.kind(), AdapterFaultKind::Panic)
    ));
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        first_harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
    drop(transaction);
}

#[test]
fn typed_unique_batch_pool_contains_adapter_panic_when_worker_drops() {
    let Fixture {
        runtime,
        resource,
        harness,
    } = fixture(vec![(Callback::AdapterDrop, 0, Action::Panic)]);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_unique_write(&mut transaction, &resource, &[1, 2]);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    drop(resource);
    let worker_drop = catch_unwind(AssertUnwindSafe(|| drop(worker)));
    assert!(worker_drop.is_ok());
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
}

#[test]
fn typed_batch_materialization_preserves_ordinary_pool_tail_lifetime() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (ordinary, ordinary_harness) = register_inject_resource(
        &runtime,
        91,
        vec![(Callback::AdapterDrop, 0, Action::Panic)],
    );
    let (batched, batched_harness) = register_inject_resource(&runtime, 90, Vec::new());
    let mut worker = runtime.attach().unwrap();

    // Leave three ordinary pooled slots retaining the first adapter. A
    // one-item typed batch releases only the corresponding prefix slot.
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3] {
        transaction.with_item(&ordinary, key, |_| Ok(())).unwrap();
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    ordinary_harness.clear();
    drop(ordinary);

    let mut transaction = worker.begin().unwrap();
    stage_unique_write(&mut transaction, &batched, &[10]);
    transaction
        .with_item(&batched, 11, |entry| entry.stage(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Materialization replaces only the live prefix. The third ordinary
    // pooled slot remains the old adapter's last handle instead of being
    // destroyed merely because the representation changed.
    assert_eq!(
        ordinary_harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        0
    );
    assert_eq!(
        finish_events(&batched_harness.events()),
        vec![
            (11, FinishDisposition::Committed, true),
            (10, FinishDisposition::Committed, true),
        ]
    );

    let worker_drop = catch_unwind(AssertUnwindSafe(|| drop(worker)));
    assert!(worker_drop.is_ok());
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        ordinary_harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
}

#[test]
fn pooled_adapter_destructor_panic_is_contained_on_different_type_replacement() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_inject_resource(
        &runtime,
        96,
        vec![(Callback::AdapterDrop, 0, Action::Panic)],
    );
    let array = TxnArray::new(&runtime, [11_u64]).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    transaction.with_item(&resource, 1, |_| Ok(())).unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    drop(resource);

    let mut transaction = worker.begin().unwrap();
    let result = array.get(&mut transaction, 0);
    assert!(matches!(
        result,
        Err(AccessError::Fault(fault))
            if fault.phase() == AdapterPhase::ItemInit
                && matches!(fault.kind(), AdapterFaultKind::Panic)
    ));
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        harness
            .events()
            .iter()
            .filter(|event| matches!(event, Event::AdapterDrop))
            .count(),
        1
    );
    drop(transaction);
}
