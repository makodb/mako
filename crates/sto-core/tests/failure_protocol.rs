use std::{
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc, Mutex, MutexGuard,
    },
};

use sto_core::{
    AbortReason, AccessError, AcquireContext, AcquireError, Active, AdapterFault, AdapterFaultKind,
    AdapterPhase, BorrowedInjectiveLockCommitCapability, BorrowedLockToken,
    BorrowedUniqueLockCommitCapability, CheckError, CommitFailure, CommitHook, CommitHookError,
    CommitOutcome, Conflict, DefiniteOutcome, DirectBorrowedLockTarget, DirectCommitCapability,
    DirectInstallContext, DirectLockMut, DirectLockRef, DirectTokenLock, DirectValidationContext,
    DirectValidationItem, Entry, ExecutionCheckContext, FailurePhase, FinishContext,
    FinishDisposition, FinishItem, InstallContext, InstallItem, InternalError, InvalidUse,
    ItemInitError, LockClass, LockDisposition, LockIdentity, LockNamespaceId, LockRequest, LockUse,
    NoPredicate, ObservationOrder, OpacityToken, PoisonInfo, PredicateContext, PreflightContext,
    PreflightFreeReadCapability, PreflightFreeValidationContext, PreflightItem, PrepareError,
    RegisteredResource, ResourceClass, Runtime, RuntimeConfig, RuntimeHealth,
    TerminalReadBatchCapability, Transaction, TransactionLock, TransactionalResource, TxnArray,
    UniqueItemKeys, ValidationContext,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Callback {
    ItemInit,
    DirectCapability,
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
    IntentDrop,
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
    IntentDrop(u64),
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

struct BorrowedInjectLock {
    harness: Arc<Harness>,
}

impl TransactionLock for BorrowedInjectLock {
    type Guard = InjectGuard;

    fn try_acquire(
        &self,
        identity: &LockIdentity,
        _cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        if identity.runtime_id() != self.harness.runtime_id
            || identity.namespace_id() != self.harness.namespace
            || identity.class() != LockClass::new(1).unwrap()
        {
            return Err(injected_fault(AdapterPhase::Acquire).into());
        }
        let key = identity
            .key()
            .as_u64()
            .ok_or_else(|| injected_fault(AdapterPhase::Acquire))?;
        self.harness.record(Event::Acquire(key));
        match self.harness.action(Callback::Acquire, key) {
            None => Ok(InjectGuard {
                key,
                harness: Arc::clone(&self.harness),
            }),
            Some(Action::Conflict) => Err(Conflict::LockBusy.into()),
            Some(Action::Fault) => Err(injected_fault(AdapterPhase::Acquire).into()),
            Some(Action::Panic) => panic!("injected borrowed acquisition panic"),
        }
    }

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        _cx: &sto_core::ReleaseContext<'_>,
    ) {
        let key = guard.key;
        self.harness.record(Event::Release(key, disposition));
        let callback = match disposition {
            LockDisposition::Aborted => Callback::ReleaseAborted,
            LockDisposition::Committed { .. } => Callback::ReleaseCommitted,
            LockDisposition::Indeterminate { .. } => Callback::ReleaseIndeterminate,
        };
        if self.harness.action(callback, key).is_some() {
            panic!("injected borrowed release panic");
        }
    }
}

// SAFETY: the fixture's stable target address plus one u64 key is its complete
// lock identity. The implementation checks the runtime before recording an
// acquisition and returns the same InjectGuard semantics as TransactionLock.
#[allow(
    unsafe_code,
    reason = "the fixture deliberately exercises the compact target/token contract"
)]
unsafe impl DirectTokenLock for BorrowedInjectLock {
    type Token = u64;

    fn try_acquire_token(
        &self,
        runtime_id: sto_core::RuntimeId,
        key: Self::Token,
        _cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        if runtime_id != self.harness.runtime_id {
            return Err(injected_fault(AdapterPhase::Acquire).into());
        }
        self.harness.record(Event::Acquire(key));
        match self.harness.action(Callback::Acquire, key) {
            None => Ok(InjectGuard {
                key,
                harness: Arc::clone(&self.harness),
            }),
            Some(Action::Conflict) => Err(Conflict::LockBusy.into()),
            Some(Action::Fault) => Err(injected_fault(AdapterPhase::Acquire).into()),
            Some(Action::Panic) => panic!("injected borrowed token acquisition panic"),
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

struct DirectIntent {
    key: u64,
    harness: Arc<Harness>,
}

impl DirectIntent {
    fn new(key: u64, harness: &Arc<Harness>) -> Self {
        Self {
            key,
            harness: Arc::clone(harness),
        }
    }
}

impl Drop for DirectIntent {
    fn drop(&mut self) {
        self.harness.record(Event::IntentDrop(self.key));
        if self
            .harness
            .action(Callback::IntentDrop, self.key)
            .is_some()
        {
            panic!("injected direct intent-drop panic");
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
    terminal_reads: bool,
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

    fn terminal_read_batch_capability(&self) -> Option<&'static TerminalReadBatchCapability<Self>> {
        self.terminal_reads.then_some(&INJECT_TERMINAL_READ_BATCH)
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

#[derive(Clone, Copy)]
enum DirectIdentityMode {
    Checked,
    CheckedAliasTenth,
    Injective,
    InjectiveWrongRuntimeTenth,
    InjectiveMissingTenth,
    InjectiveUnexpectedTenth,
}

impl DirectIdentityMode {
    fn is_injective(self) -> bool {
        matches!(
            self,
            Self::Injective
                | Self::InjectiveWrongRuntimeTenth
                | Self::InjectiveMissingTenth
                | Self::InjectiveUnexpectedTenth
        )
    }
}

struct DirectInjectAdapter {
    harness: Arc<Harness>,
    target: BorrowedInjectLock,
    alternate_target: BorrowedInjectLock,
    target_calls: AtomicUsize,
    capability_calls: AtomicUsize,
    direct_capability_enabled: bool,
    unstable_target: bool,
    drop_only_committed_finish: bool,
    identity_mode: DirectIdentityMode,
}

impl Drop for DirectInjectAdapter {
    fn drop(&mut self) {
        self.harness.record(Event::AdapterDrop);
        if self.harness.action(Callback::AdapterDrop, 0).is_some() {
            panic!("injected direct adapter-drop panic");
        }
    }
}

impl DirectBorrowedLockTarget<BorrowedInjectLock> for DirectInjectAdapter {
    fn direct_borrowed_lock_target(&self) -> &BorrowedInjectLock {
        if self.unstable_target && self.target_calls.fetch_add(1, Ordering::Relaxed) & 1 == 1 {
            &self.alternate_target
        } else {
            &self.target
        }
    }
}

impl TransactionalResource for DirectInjectAdapter {
    type Key = u64;
    type Local = ();
    type Observation = Token;
    type Predicate = NoPredicate;
    type Intent = DirectIntent;
    type Prepared = Prepared;

    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        self.harness.record(Event::ItemInit(*key));
        match self.harness.action(Callback::ItemInit, *key) {
            None | Some(Action::Conflict) => Ok(()),
            Some(Action::Fault) => Err(injected_fault(AdapterPhase::ItemInit).into()),
            Some(Action::Panic) => panic!("injected direct item-init panic"),
        }
    }

    fn direct_commit_capability(&self) -> Option<&'static DirectCommitCapability<Self>> {
        self.capability_calls.fetch_add(1, Ordering::Relaxed);
        if self.harness.action(Callback::DirectCapability, 0).is_some() {
            panic!("injected direct capability panic");
        }
        if !self.direct_capability_enabled {
            return None;
        }
        Some(
            match (
                self.drop_only_committed_finish,
                self.identity_mode.is_injective(),
            ) {
                (false, false) => &INJECT_DIRECT_COMMIT,
                (true, false) => &INJECT_DROP_ONLY_DIRECT_COMMIT,
                (false, true) => &INJECT_INJECTIVE_DIRECT_COMMIT,
                (true, true) => &INJECT_DROP_ONLY_INJECTIVE_DIRECT_COMMIT,
            },
        )
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
            Some(Action::Panic) => panic!("injected fallback preflight panic"),
        }
        let lock_use = if item.intent().is_some() {
            let identity = LockIdentity::new(
                self.harness.runtime_id,
                self.harness.namespace,
                LockClass::new(1).unwrap(),
                *key,
            );
            Some(cx.require_lock(LockRequest::new(
                identity,
                Arc::new(InjectLock {
                    key: *key,
                    harness: Arc::clone(&self.harness),
                }),
            ))?)
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
        predicate: &Self::Predicate,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        match *predicate {}
    }

    fn upgrade_predicate(
        &self,
        _key: &Self::Key,
        predicate: &Self::Predicate,
        _prepared: &Self::Prepared,
        _cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError> {
        match *predicate {}
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
            "injected fallback validation panic",
        )
    }

    fn install(
        &self,
        key: &Self::Key,
        _item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let guard = cx
            .guard_mut(
                prepared
                    .lock_use
                    .as_ref()
                    .expect("fallback direct fixture write prepares a lock"),
            )
            .expect("fallback direct fixture lock resolves");
        assert_eq!(guard.key, *key);
        self.harness.record(Event::Install(*key));
        if self.harness.action(Callback::Install, *key).is_some() {
            panic!("injected fallback install panic");
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
            panic!("injected direct finish panic");
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

static INJECT_TERMINAL_READ_BATCH: TerminalReadBatchCapability<InjectAdapter> =
    TerminalReadBatchCapability::new_drop_only(validate_inject_preflight_free_read);

static INJECT_DIRECT_UNIQUE_LOCKS: BorrowedUniqueLockCommitCapability<
    DirectInjectAdapter,
    BorrowedInjectLock,
> = BorrowedUniqueLockCommitCapability::new(
    prepare_inject_direct,
    validate_inject_direct,
    install_inject_direct,
);

static INJECT_DIRECT_TOKEN_LOCKS: BorrowedInjectiveLockCommitCapability<
    DirectInjectAdapter,
    BorrowedInjectLock,
> = BorrowedInjectiveLockCommitCapability::new(
    prepare_inject_direct_token,
    validate_inject_direct,
    install_inject_direct,
);

static INJECT_DIRECT_COMMIT: DirectCommitCapability<DirectInjectAdapter> =
    DirectCommitCapability::borrowed_unique_lock(&INJECT_DIRECT_UNIQUE_LOCKS);

static INJECT_DROP_ONLY_DIRECT_COMMIT: DirectCommitCapability<DirectInjectAdapter> =
    DirectCommitCapability::borrowed_unique_lock(&INJECT_DIRECT_UNIQUE_LOCKS)
        .with_drop_only_committed_finish();

#[allow(
    unsafe_code,
    reason = "every injective fixture mode keeps the distinct target/token proof"
)]
static INJECT_INJECTIVE_DIRECT_COMMIT: DirectCommitCapability<DirectInjectAdapter> =
    unsafe { DirectCommitCapability::borrowed_injective_token_lock(&INJECT_DIRECT_TOKEN_LOCKS) };

#[allow(
    unsafe_code,
    reason = "every injective fixture mode keeps the distinct target/token proof"
)]
static INJECT_DROP_ONLY_INJECTIVE_DIRECT_COMMIT: DirectCommitCapability<DirectInjectAdapter> =
    unsafe { DirectCommitCapability::borrowed_injective_token_lock(&INJECT_DIRECT_TOKEN_LOCKS) }
        .with_drop_only_committed_finish();

fn prepare_inject_direct(
    adapter: &DirectInjectAdapter,
    key: &u64,
    item: PreflightItem<'_, DirectInjectAdapter>,
) -> Result<Option<LockIdentity>, PrepareError> {
    adapter.harness.record(Event::Preflight(*key));
    match adapter.harness.action(Callback::Preflight, *key) {
        None => {}
        Some(Action::Conflict) => return Err(Conflict::ReadValidation.into()),
        Some(Action::Fault) => return Err(injected_fault(AdapterPhase::Preflight).into()),
        Some(Action::Panic) => panic!("injected direct preflight panic"),
    }
    if matches!(
        adapter.identity_mode,
        DirectIdentityMode::InjectiveMissingTenth
    ) && *key == 10
    {
        return Ok(None);
    }
    let emit_identity = item.intent().is_some()
        || (matches!(
            adapter.identity_mode,
            DirectIdentityMode::InjectiveUnexpectedTenth
        ) && *key == 10);
    Ok(emit_identity.then(|| {
        let identity_key = if matches!(adapter.identity_mode, DirectIdentityMode::CheckedAliasTenth)
            && *key == 10
        {
            1
        } else {
            *key
        };
        let runtime_id = if matches!(
            adapter.identity_mode,
            DirectIdentityMode::InjectiveWrongRuntimeTenth
        ) && *key == 10
        {
            sto_core::RuntimeId::new(u64::MAX).unwrap()
        } else {
            adapter.harness.runtime_id
        };
        LockIdentity::new(
            runtime_id,
            adapter.harness.namespace,
            LockClass::new(1).unwrap(),
            identity_key,
        )
    }))
}

fn prepare_inject_direct_token(
    adapter: &DirectInjectAdapter,
    key: &u64,
    item: PreflightItem<'_, DirectInjectAdapter>,
) -> Result<Option<BorrowedLockToken<u64>>, PrepareError> {
    adapter.harness.record(Event::Preflight(*key));
    match adapter.harness.action(Callback::Preflight, *key) {
        None => {}
        Some(Action::Conflict) => return Err(Conflict::ReadValidation.into()),
        Some(Action::Fault) => return Err(injected_fault(AdapterPhase::Preflight).into()),
        Some(Action::Panic) => panic!("injected direct token preflight panic"),
    }
    if matches!(
        adapter.identity_mode,
        DirectIdentityMode::InjectiveMissingTenth
    ) && *key == 10
    {
        return Ok(None);
    }
    let emit_token = item.intent().is_some()
        || (matches!(
            adapter.identity_mode,
            DirectIdentityMode::InjectiveUnexpectedTenth
        ) && *key == 10);
    Ok(emit_token.then(|| {
        let runtime_id = if matches!(
            adapter.identity_mode,
            DirectIdentityMode::InjectiveWrongRuntimeTenth
        ) && *key == 10
        {
            sto_core::RuntimeId::new(u64::MAX).unwrap()
        } else {
            adapter.harness.runtime_id
        };
        BorrowedLockToken::new(runtime_id, *key)
    }))
}

fn validate_inject_direct(
    adapter: &DirectInjectAdapter,
    key: &u64,
    item: DirectValidationItem<'_, DirectInjectAdapter>,
    lock: Option<DirectLockRef<'_, BorrowedInjectLock>>,
    _cx: &DirectValidationContext,
) -> Result<(), CheckError> {
    adapter.harness.record(Event::Validation(*key));
    match (item.intent().is_some(), lock) {
        (false, None) => {}
        (true, Some(lock)) => {
            if !std::ptr::eq(lock.target(), &adapter.target) || lock.guard().key != *key {
                return Err(injected_fault(AdapterPhase::Validation).into());
            }
        }
        _ => return Err(injected_fault(AdapterPhase::Validation).into()),
    }
    check_action(
        adapter.harness.action(Callback::Validation, *key),
        AdapterPhase::Validation,
        Conflict::ReadValidation,
        "injected direct validation panic",
    )
}

fn install_inject_direct(
    adapter: &DirectInjectAdapter,
    key: &u64,
    _item: InstallItem<'_, DirectInjectAdapter>,
    mut lock: DirectLockMut<'_, BorrowedInjectLock>,
    _cx: &mut DirectInstallContext,
) {
    assert!(std::ptr::eq(lock.target(), &adapter.target));
    assert_eq!(lock.guard_mut().key, *key);
    adapter.harness.record(Event::Install(*key));
    if adapter.harness.action(Callback::Install, *key).is_some() {
        panic!("injected direct install panic");
    }
}

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

fn terminal_read_fixture(actions: Vec<(Callback, u64, Action)>) -> Fixture {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_inject_resource_with_protocols(
        &runtime,
        99,
        actions,
        PreflightFreeReadMode::Disabled,
        true,
    );
    Fixture {
        runtime,
        resource,
        harness,
    }
}

struct DirectFixture {
    runtime: Arc<Runtime>,
    resource: RegisteredResource<DirectInjectAdapter>,
    harness: Arc<Harness>,
}

fn direct_fixture(actions: Vec<(Callback, u64, Action)>) -> DirectFixture {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_direct_resource(&runtime, actions);
    DirectFixture {
        runtime,
        resource,
        harness,
    }
}

fn drop_only_direct_fixture(actions: Vec<(Callback, u64, Action)>) -> DirectFixture {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_direct_resource_with_modes(
        &runtime,
        actions,
        false,
        true,
        DirectIdentityMode::Checked,
    );
    DirectFixture {
        runtime,
        resource,
        harness,
    }
}

fn register_direct_resource(
    runtime: &Arc<Runtime>,
    actions: Vec<(Callback, u64, Action)>,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    register_direct_resource_with_modes(runtime, actions, false, false, DirectIdentityMode::Checked)
}

fn register_direct_resource_with_target_mode(
    runtime: &Arc<Runtime>,
    actions: Vec<(Callback, u64, Action)>,
    unstable_target: bool,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    register_direct_resource_with_modes(
        runtime,
        actions,
        unstable_target,
        false,
        DirectIdentityMode::Checked,
    )
}

fn register_direct_resource_with_identity_alias(
    runtime: &Arc<Runtime>,
    actions: Vec<(Callback, u64, Action)>,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    register_direct_resource_with_modes(
        runtime,
        actions,
        false,
        false,
        DirectIdentityMode::CheckedAliasTenth,
    )
}

fn register_injective_direct_resource(
    runtime: &Arc<Runtime>,
    identity_mode: DirectIdentityMode,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    register_injective_direct_resource_with_actions(runtime, Vec::new(), false, identity_mode)
}

fn register_injective_direct_resource_with_actions(
    runtime: &Arc<Runtime>,
    actions: Vec<(Callback, u64, Action)>,
    unstable_target: bool,
    identity_mode: DirectIdentityMode,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    assert!(identity_mode.is_injective());
    register_direct_resource_with_modes(runtime, actions, unstable_target, false, identity_mode)
}

fn register_direct_resource_with_modes(
    runtime: &Arc<Runtime>,
    actions: Vec<(Callback, u64, Action)>,
    unstable_target: bool,
    drop_only_committed_finish: bool,
    identity_mode: DirectIdentityMode,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    register_direct_resource_with_capability(
        runtime,
        actions,
        unstable_target,
        drop_only_committed_finish,
        identity_mode,
        true,
    )
}

fn register_direct_resource_with_capability(
    runtime: &Arc<Runtime>,
    actions: Vec<(Callback, u64, Action)>,
    unstable_target: bool,
    drop_only_committed_finish: bool,
    identity_mode: DirectIdentityMode,
    direct_capability_enabled: bool,
) -> (RegisteredResource<DirectInjectAdapter>, Arc<Harness>) {
    let object = runtime.register_object().unwrap();
    let harness = Arc::new(Harness {
        runtime_id: object.runtime_id(),
        namespace: LockNamespaceId::new(object.object_id().get()).unwrap(),
        actions,
        events: Mutex::new(Vec::new()),
    });
    let resource = object
        .register_resource(
            ResourceClass::new(99).unwrap(),
            DirectInjectAdapter {
                harness: Arc::clone(&harness),
                target: BorrowedInjectLock {
                    harness: Arc::clone(&harness),
                },
                alternate_target: BorrowedInjectLock {
                    harness: Arc::clone(&harness),
                },
                target_calls: AtomicUsize::new(0),
                capability_calls: AtomicUsize::new(0),
                direct_capability_enabled,
                unstable_target,
                drop_only_committed_finish,
                identity_mode,
            },
        )
        .unwrap();
    (resource, harness)
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
    register_inject_resource_with_protocols(
        runtime,
        resource_class,
        actions,
        preflight_free_reads,
        false,
    )
}

fn register_inject_resource_with_protocols(
    runtime: &Arc<Runtime>,
    resource_class: u32,
    actions: Vec<(Callback, u64, Action)>,
    preflight_free_reads: PreflightFreeReadMode,
    terminal_reads: bool,
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
                terminal_reads,
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

fn stage_direct_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<DirectInjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| {
            entry.stage(DirectIntent::new(key, &resource.adapter().harness))
        })
        .unwrap();
}

fn stage_direct_read_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<DirectInjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| {
            entry.record_read(Token::inert())?;
            entry.stage(DirectIntent::new(key, &resource.adapter().harness))
        })
        .unwrap();
}

fn stage_tracked_direct_read_write(
    transaction: &mut Transaction<'_, Active>,
    resource: &RegisteredResource<DirectInjectAdapter>,
    key: u64,
) {
    transaction
        .with_item(resource, key, |entry| {
            entry.record_read(Token::upgraded(key, &resource.adapter().harness))?;
            entry.stage(DirectIntent::new(key, &resource.adapter().harness))
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
            | (Callback::Install, Event::Install(key))
            | (Callback::ObservationDrop, Event::ObservationDrop(key))
            | (Callback::IntentDrop, Event::IntentDrop(key)) => Some(*key),
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
fn direct_typed_commit_uses_request_order_and_finishes_without_generic_preparation() {
    let fixture = direct_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [2, 3, 1] {
        stage_direct_read_write(&mut transaction, &fixture.resource, key);
    }
    assert_eq!(
        fixture
            .resource
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        1,
        "one exact binding contributes its capability once during execution"
    );
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(
        fixture
            .resource
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        1,
        "commit selection must use the incremental summary"
    );

    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Preflight), vec![2, 3, 1]);
    assert_eq!(callback_keys(&events, Callback::Acquire), vec![2, 3, 1]);
    assert_eq!(callback_keys(&events, Callback::Validation), vec![2, 3, 1]);
    assert_eq!(callback_keys(&events, Callback::Install), vec![2, 3, 1]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [
            (1, LockDisposition::Committed { .. }),
            (3, LockDisposition::Committed { .. }),
            (2, LockDisposition::Committed { .. }),
        ]
    ));
    assert_eq!(
        finish_events(&events),
        vec![
            (1, FinishDisposition::Committed, false),
            (3, FinishDisposition::Committed, false),
            (2, FinishDisposition::Committed, false),
        ]
    );

    // Exercise the same worker-pooled concrete plan with a shorter attempt.
    fixture.harness.clear();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &fixture.resource, 7);
    assert_eq!(
        fixture
            .resource
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        2,
        "finishing an attempt resets the distinct-binding summary"
    );
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(
        fixture
            .resource
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        2
    );
    assert_eq!(
        callback_keys(&fixture.harness.events(), Callback::Acquire),
        [7]
    );
}

#[test]
fn direct_capability_panic_is_contained_during_first_binding_activation() {
    let fixture = direct_fixture(vec![(Callback::DirectCapability, 0, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let result = transaction.with_item(&fixture.resource, 7, |entry| {
        entry.stage(DirectIntent::new(7, &fixture.resource.adapter().harness))
    });

    assert!(matches!(
        result,
        Err(AccessError::Fault(ref fault))
            if fault.phase() == AdapterPhase::Execute
                && matches!(fault.kind(), AdapterFaultKind::Panic)
    ));
    assert!(transaction.is_doomed());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
    assert!(fixture.harness.events().is_empty());
    transaction.abort();
}

#[test]
fn direct_capability_panic_on_second_binding_aborts_the_live_prefix_once() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_direct_resource(&runtime, Vec::new());
    let (second, second_harness) = register_direct_resource(
        &runtime,
        vec![(Callback::DirectCapability, 0, Action::Panic)],
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_write(&mut transaction, &first, 1);

    let access = catch_unwind(AssertUnwindSafe(|| {
        transaction.with_item(&second, 2, |entry| {
            entry.stage(DirectIntent::new(2, &second.adapter().harness))
        })
    }));
    assert!(matches!(
        access,
        Ok(Err(AccessError::Fault(ref fault)))
            if fault.phase() == AdapterPhase::Execute
                && matches!(fault.kind(), AdapterFaultKind::Panic)
    ));
    assert!(transaction.is_doomed());
    assert_eq!(first.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert!(second_harness.events().is_empty());
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);

    assert_poisoned_aborted(transaction.commit(), FailurePhase::Execution);
    assert_eq!(
        finish_events(&first_harness.events()),
        [(1, FinishDisposition::Aborted, false)]
    );
    assert!(second_harness.events().is_empty());
}

#[test]
fn indexed_direct_plan_rejects_a_late_physical_identity_alias_before_acquisition() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_direct_resource_with_identity_alias(&runtime, Vec::new());
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=10 {
        stage_direct_read_write(&mut transaction, &resource, key);
    }

    assert_poisoned_aborted(transaction.commit(), FailurePhase::Preflight);
    let events = harness.events();
    assert_eq!(
        callback_keys(&events, Callback::Preflight),
        (1..=10).collect::<Vec<_>>()
    );
    assert!(callback_keys(&events, Callback::Acquire).is_empty());
    assert_eq!(
        finish_events(&events)
            .into_iter()
            .map(|(key, _, _)| key)
            .collect::<Vec<_>>(),
        (1..=10).rev().collect::<Vec<_>>()
    );
}

#[test]
fn injective_borrowed_direct_plan_commits_distinct_items_in_request_order() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) =
        register_injective_direct_resource(&runtime, DirectIdentityMode::Injective);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [7, 3, 11] {
        stage_direct_read_write(&mut transaction, &resource, key);
    }

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = harness.events();
    assert_eq!(callback_keys(&events, Callback::Preflight), [7, 3, 11]);
    assert_eq!(callback_keys(&events, Callback::Acquire), [7, 3, 11]);
    assert_eq!(callback_keys(&events, Callback::Validation), [7, 3, 11]);
    assert_eq!(callback_keys(&events, Callback::Install), [7, 3, 11]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [
            (11, LockDisposition::Committed { .. }),
            (3, LockDisposition::Committed { .. }),
            (7, LockDisposition::Committed { .. }),
        ]
    ));
    let last_release = events
        .iter()
        .rposition(|event| matches!(event, Event::Release(..)))
        .unwrap();
    let first_guard_drop = events
        .iter()
        .position(|event| matches!(event, Event::GuardDrop(_)))
        .unwrap();
    assert!(last_release < first_guard_drop);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);

    // Reuse the same worker-pooled compact plan with fewer active frames.
    harness.clear();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &resource, 19);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = harness.events();
    assert_eq!(callback_keys(&events, Callback::Acquire), [19]);
    assert!(matches!(
        release_events(&events).as_slice(),
        [(19, LockDisposition::Committed { .. })]
    ));
}

#[test]
fn injective_token_plan_rejects_a_changed_target_before_acquisition() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_injective_direct_resource_with_actions(
        &runtime,
        Vec::new(),
        true,
        DirectIdentityMode::Injective,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &resource, 7);

    assert_poisoned_aborted(transaction.commit(), FailurePhase::Acquire);
    let events = harness.events();
    assert!(callback_keys(&events, Callback::Acquire).is_empty());
    assert_eq!(
        finish_events(&events),
        [(7, FinishDisposition::Aborted, false)]
    );
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn injective_token_acquire_failures_release_prefix_and_preserve_certainty() {
    for action in [Action::Conflict, Action::Fault, Action::Panic] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) = register_injective_direct_resource_with_actions(
            &runtime,
            vec![(Callback::Acquire, 2, action)],
            false,
            DirectIdentityMode::Injective,
        );
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        for key in 1..=3 {
            stage_direct_read_write(&mut transaction, &resource, key);
        }

        let result = transaction.commit();
        match action {
            Action::Conflict => assert_eq!(
                result.unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::LockBusy))
            ),
            Action::Fault | Action::Panic => assert_poisoned_aborted(result, FailurePhase::Acquire),
        }
        let events = harness.events();
        assert_eq!(callback_keys(&events, Callback::Acquire), [1, 2]);
        assert_eq!(release_events(&events), [(1, LockDisposition::Aborted)]);
        assert!(callback_keys(&events, Callback::Validation).is_empty());
        assert!(callback_keys(&events, Callback::Install).is_empty());
        if action == Action::Panic {
            assert!(finish_events(&events).is_empty());
            assert!(events
                .iter()
                .all(|event| !matches!(event, Event::GuardDrop(_))));
        } else {
            assert_eq!(
                finish_events(&events),
                [
                    (3, FinishDisposition::Aborted, false),
                    (2, FinishDisposition::Aborted, false),
                    (1, FinishDisposition::Aborted, false),
                ]
            );
            assert_eq!(
                events
                    .iter()
                    .filter(|event| matches!(event, Event::GuardDrop(1)))
                    .count(),
                1
            );
        }
    }
}

#[test]
fn injective_token_rollback_release_panic_quarantines_the_plan() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_injective_direct_resource_with_actions(
        &runtime,
        vec![
            (Callback::Acquire, 3, Action::Conflict),
            (Callback::ReleaseAborted, 2, Action::Panic),
        ],
        false,
        DirectIdentityMode::Injective,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_direct_read_write(&mut transaction, &resource, key);
    }

    assert_poisoned_aborted(transaction.commit(), FailurePhase::Acquire);
    let events = harness.events();
    assert_eq!(callback_keys(&events, Callback::Acquire), [1, 2, 3]);
    assert_eq!(
        release_events(&events),
        [(2, LockDisposition::Aborted), (1, LockDisposition::Aborted)]
    );
    assert!(finish_events(&events).is_empty());
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::GuardDrop(_))));
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn injective_token_release_panic_quarantines_the_uncertain_guard() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_injective_direct_resource_with_actions(
        &runtime,
        vec![(Callback::ReleaseCommitted, 2, Action::Panic)],
        false,
        DirectIdentityMode::Injective,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_direct_write(&mut transaction, &resource, key);
    }

    let result = transaction.commit();
    let Err(CommitFailure::Indeterminate(info)) = result else {
        panic!("expected indeterminate compact-token release failure, got {result:?}");
    };
    assert_eq!(info.phase(), FailurePhase::Release);
    let events = harness.events();
    assert!(matches!(
        release_events(&events).as_slice(),
        [
            (3, LockDisposition::Committed { .. }),
            (2, LockDisposition::Committed { .. }),
            (1, LockDisposition::Indeterminate { .. }),
        ]
    ));
    assert!(finish_events(&events).is_empty());
    assert!(events
        .iter()
        .all(|event| !matches!(event, Event::GuardDrop(_))));
    assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
}

#[test]
fn injective_token_released_guard_drop_panic_is_contained() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_injective_direct_resource_with_actions(
        &runtime,
        vec![(Callback::GuardDrop, 3, Action::Panic)],
        false,
        DirectIdentityMode::Injective,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_direct_write(&mut transaction, &resource, key);
    }

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("compact-token guard drop panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Release);
    let events = harness.events();
    assert!(release_events(&events)
        .iter()
        .all(|(_, disposition)| matches!(disposition, LockDisposition::Committed { .. })));
    let dropped: Vec<_> = events
        .iter()
        .filter_map(|event| match event {
            Event::GuardDrop(key) => Some(*key),
            _ => None,
        })
        .collect();
    assert_eq!(dropped, [3]);
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn injective_borrowed_direct_plan_still_rejects_a_wrong_runtime() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_injective_direct_resource(
        &runtime,
        DirectIdentityMode::InjectiveWrongRuntimeTenth,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &resource, 10);

    assert_poisoned_aborted(transaction.commit(), FailurePhase::Preflight);
    let events = harness.events();
    assert_eq!(callback_keys(&events, Callback::Preflight), [10]);
    assert!(callback_keys(&events, Callback::Acquire).is_empty());
}

#[test]
fn injective_borrowed_direct_plan_still_checks_both_intent_shapes() {
    for identity_mode in [
        DirectIdentityMode::InjectiveMissingTenth,
        DirectIdentityMode::InjectiveUnexpectedTenth,
    ] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) = register_injective_direct_resource(&runtime, identity_mode);
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        if matches!(identity_mode, DirectIdentityMode::InjectiveMissingTenth) {
            stage_direct_read_write(&mut transaction, &resource, 10);
        } else {
            stage_direct_read_write(&mut transaction, &resource, 1);
            transaction
                .with_item(&resource, 10, |entry| entry.record_read(Token::inert()))
                .unwrap();
        }

        assert_poisoned_aborted(transaction.commit(), FailurePhase::Preflight);
        assert!(callback_keys(&harness.events(), Callback::Acquire).is_empty());
    }
}

#[test]
fn injective_borrowed_direct_plan_still_enforces_the_lock_limit() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(1)).unwrap();
    let (resource, harness) =
        register_injective_direct_resource(&runtime, DirectIdentityMode::Injective);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &resource, 1);
    stage_direct_read_write(&mut transaction, &resource, 2);

    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Capacity(sto_core::CapacityError::LockLimit))
    );
    let events = harness.events();
    assert_eq!(callback_keys(&events, Callback::Preflight), [1, 2]);
    assert!(callback_keys(&events, Callback::Acquire).is_empty());
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn drop_only_direct_commit_skips_finish_and_tears_down_owned_state_in_reverse() {
    // A configured finish panic proves that the committed callback is not
    // merely empty: this transaction can commit only if core skips it.
    let fixture = drop_only_direct_fixture(vec![(Callback::Finish, 3, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [2, 3, 1] {
        stage_tracked_direct_read_write(&mut transaction, &fixture.resource, key);
    }

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Install), [2, 3, 1]);
    assert!(finish_events(&events).is_empty());
    assert_eq!(callback_keys(&events, Callback::IntentDrop), [1, 3, 2]);
    assert_eq!(callback_keys(&events, Callback::ObservationDrop), [1, 3, 2]);
    let owned_teardown: Vec<_> = events
        .iter()
        .filter(|event| matches!(event, Event::IntentDrop(_) | Event::ObservationDrop(_)))
        .cloned()
        .collect();
    assert_eq!(
        owned_teardown,
        [
            Event::IntentDrop(1),
            Event::ObservationDrop(1),
            Event::IntentDrop(3),
            Event::ObservationDrop(3),
            Event::IntentDrop(2),
            Event::ObservationDrop(2),
        ]
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn drop_only_direct_abort_still_runs_finish_for_conflicts_faults_and_panics() {
    for action in [Action::Conflict, Action::Fault] {
        let fixture = drop_only_direct_fixture(vec![(Callback::Validation, 2, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        for key in 1..=3 {
            stage_tracked_direct_read_write(&mut transaction, &fixture.resource, key);
        }

        let result = transaction.commit();
        match action {
            Action::Conflict => assert_eq!(
                result.unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
            ),
            Action::Fault => assert_poisoned_aborted(result, FailurePhase::Validation),
            Action::Panic => unreachable!(),
        }
        let events = fixture.harness.events();
        assert_eq!(
            finish_events(&events),
            [
                (3, FinishDisposition::Aborted, false),
                (2, FinishDisposition::Aborted, false),
                (1, FinishDisposition::Aborted, false),
            ]
        );
        assert_eq!(callback_keys(&events, Callback::IntentDrop), [3, 2, 1]);
        assert_eq!(callback_keys(&events, Callback::ObservationDrop), [3, 2, 1]);
    }

    let fixture = drop_only_direct_fixture(vec![
        (Callback::Validation, 2, Action::Conflict),
        (Callback::Finish, 3, Action::Panic),
    ]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_tracked_direct_read_write(&mut transaction, &fixture.resource, key);
    }
    assert_poisoned_aborted(transaction.commit(), FailurePhase::Finish);
    assert_eq!(
        finish_events(&fixture.harness.events()),
        [(3, FinishDisposition::Aborted, false)]
    );
}

#[test]
fn drop_only_direct_teardown_panic_is_a_definite_committed_failure() {
    let fixture = drop_only_direct_fixture(vec![(Callback::IntentDrop, 2, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_tracked_direct_read_write(&mut transaction, &fixture.resource, key);
    }

    let Err(CommitFailure::Poisoned { outcome, info }) = transaction.commit() else {
        panic!("drop-only direct teardown panic must poison a definite commit");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    let events = fixture.harness.events();
    assert!(finish_events(&events).is_empty());
    assert_eq!(callback_keys(&events, Callback::IntentDrop), [3, 2]);
    assert_eq!(callback_keys(&events, Callback::ObservationDrop), [3]);
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn drop_only_direct_install_panic_keeps_indeterminate_state_quarantined() {
    let fixture = drop_only_direct_fixture(vec![(Callback::Install, 2, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in 1..=3 {
        stage_tracked_direct_read_write(&mut transaction, &fixture.resource, key);
    }

    let Err(CommitFailure::Indeterminate(info)) = transaction.commit() else {
        panic!("direct install panic must remain indeterminate");
    };
    assert_eq!(info.phase(), FailurePhase::Install);
    let events = fixture.harness.events();
    assert!(finish_events(&events).is_empty());
    assert!(callback_keys(&events, Callback::IntentDrop).is_empty());
    assert!(callback_keys(&events, Callback::ObservationDrop).is_empty());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Indeterminate);
}

#[test]
fn borrowed_direct_plan_rejects_a_changed_target_before_acquisition() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_direct_resource_with_target_mode(&runtime, Vec::new(), true);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &resource, 7);

    let result = transaction.commit();
    let Err(CommitFailure::Poisoned { outcome, info }) = result else {
        panic!("expected a poisoned borrowed-target failure, got {result:?}");
    };
    assert!(matches!(outcome, DefiniteOutcome::Aborted(_)));
    assert_eq!(info.phase(), FailurePhase::Acquire);
    let events = harness.events();
    assert!(callback_keys(&events, Callback::Acquire).is_empty());
    assert_eq!(
        finish_events(&events),
        [(7, FinishDisposition::Aborted, false)]
    );
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn distinct_resource_unique_groups_remain_direct_commit_eligible() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) =
        register_injective_direct_resource(&runtime, DirectIdentityMode::Injective);
    let (second, second_harness) =
        register_injective_direct_resource(&runtime, DirectIdentityMode::Injective);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [2_u64, 1];
    let second_keys = [2_u64, 3];

    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |index, entry| {
                entry.record_read(Token::inert())?;
                entry.stage(DirectIntent::new(
                    first_keys[index],
                    &first.adapter().harness,
                ))
            },
        )
        .unwrap();
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&second_keys).unwrap(),
            |index, entry| {
                entry.record_read(Token::inert())?;
                entry.stage(DirectIntent::new(
                    second_keys[index],
                    &second.adapter().harness,
                ))
            },
        )
        .unwrap();
    assert_eq!(first.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(first.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);

    let first_events = first_harness.events();
    let second_events = second_harness.events();
    assert_eq!(callback_keys(&first_events, Callback::Preflight), [2, 1]);
    assert_eq!(callback_keys(&second_events, Callback::Preflight), [2, 3]);
    assert_eq!(callback_keys(&first_events, Callback::Acquire), [2, 1]);
    assert_eq!(callback_keys(&second_events, Callback::Acquire), [2, 3]);
    assert_eq!(
        finish_events(&first_events),
        [
            (1, FinishDisposition::Committed, false),
            (2, FinishDisposition::Committed, false),
        ],
        "both groups must finish through the preparation-free direct plan"
    );
    assert_eq!(
        finish_events(&second_events),
        [
            (3, FinishDisposition::Committed, false),
            (2, FinishDisposition::Committed, false),
        ],
        "the second resource group must remain in the same direct plan"
    );
}

#[test]
fn rebound_unique_batch_queries_direct_capability_once_for_its_binding() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, _) = register_direct_resource(&runtime, Vec::new());
    let (second, _) = register_direct_resource(&runtime, Vec::new());
    let mut worker = runtime.attach().unwrap();

    let first_keys = [1_u64, 2, 3];
    let mut transaction = worker.begin().unwrap();
    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert_eq!(first.adapter().capability_calls.load(Ordering::Relaxed), 1);
    transaction.abort();

    // All three pooled slots retain the first exact binding. Rebinding them to
    // the second resource must query its capability for slot zero only.
    let second_keys = [4_u64, 5, 6];
    let mut transaction = worker.begin().unwrap();
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&second_keys).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);
    transaction.abort();
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn distinct_resource_direct_capability_mismatch_uses_generic_commit() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_direct_resource(&runtime, Vec::new());
    let (second, second_harness) = register_direct_resource_with_modes(
        &runtime,
        Vec::new(),
        false,
        true,
        DirectIdentityMode::Checked,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &first, 1);
    stage_direct_read_write(&mut transaction, &second, 2);

    assert_eq!(first.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(
        finish_events(&first_harness.events()),
        [(1, FinishDisposition::Committed, true)]
    );
    assert_eq!(
        finish_events(&second_harness.events()),
        [(2, FinishDisposition::Committed, true)]
    );
}

#[test]
fn direct_capability_summary_resets_after_generic_commit_and_explicit_abort() {
    for commit_ineligible_attempt in [true, false] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (first, first_harness) = register_direct_resource(&runtime, Vec::new());
        let (second, second_harness) = register_direct_resource_with_modes(
            &runtime,
            Vec::new(),
            false,
            true,
            DirectIdentityMode::Checked,
        );
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_write(&mut transaction, &first, 1);
        stage_direct_write(&mut transaction, &second, 2);

        if commit_ineligible_attempt {
            assert!(matches!(
                transaction.commit().unwrap(),
                CommitOutcome::Committed(_)
            ));
            assert_eq!(
                finish_events(&first_harness.events()),
                [(1, FinishDisposition::Committed, true)]
            );
            assert_eq!(
                finish_events(&second_harness.events()),
                [(2, FinishDisposition::Committed, true)]
            );
        } else {
            assert_eq!(transaction.abort().reason(), &AbortReason::Explicit);
        }

        first_harness.clear();
        second_harness.clear();
        let mut transaction = worker.begin().unwrap();
        stage_direct_write(&mut transaction, &first, 3);
        assert_eq!(
            first.adapter().capability_calls.load(Ordering::Relaxed),
            2,
            "the next attempt must probe its first distinct binding again"
        );
        assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);
        assert!(matches!(
            transaction.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
        assert_eq!(
            finish_events(&first_harness.events()),
            [(3, FinishDisposition::Committed, false)],
            "the eligible successor must select direct commit"
        );
        assert!(second_harness.events().is_empty());
    }
}

#[test]
fn one_distinct_resource_without_direct_capability_uses_generic_commit() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_direct_resource(&runtime, Vec::new());
    let (second, second_harness) = register_direct_resource_with_capability(
        &runtime,
        Vec::new(),
        false,
        false,
        DirectIdentityMode::Checked,
        false,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &first, 1);
    stage_direct_read_write(&mut transaction, &second, 2);

    assert_eq!(first.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert_eq!(second.adapter().capability_calls.load(Ordering::Relaxed), 1);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(
        finish_events(&first_harness.events()),
        [(1, FinishDisposition::Committed, true)]
    );
    assert_eq!(
        finish_events(&second_harness.events()),
        [(2, FinishDisposition::Committed, true)]
    );
}

#[test]
fn colliding_binding_filter_bits_still_contribute_each_exact_capability() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    // Pigeonhole principle forces at least one collision in the batch's
    // 64-bit advisory binding filter.
    let resources: Vec<_> = (0..65)
        .map(|_| register_direct_resource(&runtime, Vec::new()).0)
        .collect();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for (index, resource) in resources.iter().enumerate() {
        stage_direct_read_write(&mut transaction, resource, index as u64 + 1);
    }

    assert!(resources
        .iter()
        .all(|resource| { resource.adapter().capability_calls.load(Ordering::Relaxed) == 1 }));
    transaction.abort();

    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &resources[0], 100);
    assert_eq!(
        resources[0]
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        2,
        "abort must reset both the exact capability and binding summaries"
    );
    transaction.abort();
}

#[test]
fn indexed_typed_prefix_and_unique_suffix_remain_direct_commit_eligible() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_direct_resource(&runtime, Vec::new());
    let (second, second_harness) = register_direct_resource(&runtime, Vec::new());
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    for key in [2_u64, 1] {
        stage_direct_read_write(&mut transaction, &first, key);
    }
    let suffix = [2_u64, 3];
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&suffix).unwrap(),
            |index, entry| {
                entry.record_read(Token::inert())?;
                entry.stage(DirectIntent::new(suffix[index], &second.adapter().harness))
            },
        )
        .unwrap();

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let first_events = first_harness.events();
    let second_events = second_harness.events();
    assert_eq!(callback_keys(&first_events, Callback::Preflight), [2, 1]);
    assert_eq!(callback_keys(&second_events, Callback::Preflight), [2, 3]);
    assert_eq!(callback_keys(&first_events, Callback::Acquire), [2, 1]);
    assert_eq!(callback_keys(&second_events, Callback::Acquire), [2, 3]);
    assert_eq!(
        finish_events(&first_events),
        [
            (1, FinishDisposition::Committed, false),
            (2, FinishDisposition::Committed, false),
        ],
        "the indexed prefix must finish through the preparation-free direct plan"
    );
    assert_eq!(
        finish_events(&second_events),
        [
            (3, FinishDisposition::Committed, false),
            (2, FinishDisposition::Committed, false),
        ],
        "the unindexed suffix must remain in the same direct plan"
    );
}

#[test]
fn indexed_typed_prefix_and_unique_suffix_apply_drop_only_committed_finish() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_direct_resource_with_modes(
        &runtime,
        Vec::new(),
        false,
        true,
        DirectIdentityMode::Checked,
    );
    let (second, second_harness) = register_direct_resource_with_modes(
        &runtime,
        Vec::new(),
        false,
        true,
        DirectIdentityMode::Checked,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    for key in [2_u64, 1] {
        stage_tracked_direct_read_write(&mut transaction, &first, key);
    }
    let suffix = [2_u64, 3];
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&suffix).unwrap(),
            |index, entry| {
                let key = suffix[index];
                entry.record_read(Token::upgraded(key, &second.adapter().harness))?;
                entry.stage(DirectIntent::new(key, &second.adapter().harness))
            },
        )
        .unwrap();

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let first_events = first_harness.events();
    let second_events = second_harness.events();
    assert!(finish_events(&first_events).is_empty());
    assert!(finish_events(&second_events).is_empty());
    assert_eq!(callback_keys(&first_events, Callback::IntentDrop), [1, 2]);
    assert_eq!(
        callback_keys(&first_events, Callback::ObservationDrop),
        [1, 2]
    );
    assert_eq!(callback_keys(&second_events, Callback::IntentDrop), [3, 2]);
    assert_eq!(
        callback_keys(&second_events, Callback::ObservationDrop),
        [3, 2]
    );
}

#[test]
fn direct_acquisition_failure_releases_the_held_prefix_and_definitely_aborts() {
    let fixture = direct_fixture(vec![(Callback::Acquire, 2, Action::Conflict)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3] {
        stage_direct_read_write(&mut transaction, &fixture.resource, key);
    }
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::LockBusy))
    );
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Acquire), [1, 2]);
    assert_eq!(release_events(&events), [(1, LockDisposition::Aborted)]);
    assert!(callback_keys(&events, Callback::Validation).is_empty());
    assert!(callback_keys(&events, Callback::Install).is_empty());
    assert_eq!(
        finish_events(&events),
        vec![
            (3, FinishDisposition::Aborted, false),
            (2, FinishDisposition::Aborted, false),
            (1, FinishDisposition::Aborted, false),
        ]
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
    worker.begin().unwrap().abort();
}

#[test]
fn direct_validation_conflict_releases_every_guard_before_abort_finish() {
    let fixture = direct_fixture(vec![(Callback::Validation, 2, Action::Conflict)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3] {
        stage_direct_read_write(&mut transaction, &fixture.resource, key);
    }
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Validation), [1, 2]);
    assert_eq!(
        release_events(&events),
        [
            (3, LockDisposition::Aborted),
            (2, LockDisposition::Aborted),
            (1, LockDisposition::Aborted),
        ]
    );
    assert!(callback_keys(&events, Callback::Install).is_empty());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn direct_install_release_and_finish_panics_preserve_commit_boundaries() {
    let install = direct_fixture(vec![(Callback::Install, 2, Action::Panic)]);
    let mut worker = install.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3] {
        stage_direct_write(&mut transaction, &install.resource, key);
    }
    let result = transaction.commit();
    let Err(CommitFailure::Indeterminate(info)) = result else {
        panic!("expected indeterminate direct install failure, got {result:?}");
    };
    assert_eq!(info.phase(), FailurePhase::Install);
    assert!(matches!(
        release_events(&install.harness.events()).as_slice(),
        [
            (3, LockDisposition::Indeterminate { .. }),
            (2, LockDisposition::Indeterminate { .. }),
            (1, LockDisposition::Indeterminate { .. }),
        ]
    ));
    assert!(finish_events(&install.harness.events()).is_empty());
    assert_eq!(install.runtime.health(), RuntimeHealth::Indeterminate);

    let release = direct_fixture(vec![(Callback::ReleaseCommitted, 2, Action::Panic)]);
    let mut worker = release.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3] {
        stage_direct_write(&mut transaction, &release.resource, key);
    }
    let result = transaction.commit();
    let Err(CommitFailure::Indeterminate(info)) = result else {
        panic!("expected indeterminate direct release failure, got {result:?}");
    };
    assert_eq!(info.phase(), FailurePhase::Release);
    assert!(matches!(
        release_events(&release.harness.events()).as_slice(),
        [
            (3, LockDisposition::Committed { .. }),
            (2, LockDisposition::Committed { .. }),
            (1, LockDisposition::Indeterminate { .. }),
        ]
    ));
    assert!(finish_events(&release.harness.events()).is_empty());
    assert_eq!(release.runtime.health(), RuntimeHealth::Indeterminate);

    let finish = direct_fixture(vec![(Callback::Finish, 2, Action::Panic)]);
    let mut worker = finish.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3] {
        stage_direct_write(&mut transaction, &finish.resource, key);
    }
    let result = transaction.commit();
    let Err(CommitFailure::Poisoned { outcome, info }) = result else {
        panic!("expected poisoned committed direct finish failure, got {result:?}");
    };
    assert!(matches!(outcome, DefiniteOutcome::Committed(_)));
    assert_eq!(info.phase(), FailurePhase::Finish);
    assert_eq!(
        finish_events(&finish.harness.events()),
        [
            (3, FinishDisposition::Committed, false),
            (2, FinishDisposition::Committed, false),
        ]
    );
    assert_eq!(finish.runtime.health(), RuntimeHealth::Poisoned);
}

#[test]
fn direct_capability_falls_back_after_typed_batch_materialization() {
    let fixture = direct_fixture(Vec::new());
    let array = TxnArray::new(&fixture.runtime, [11_u64]).unwrap();
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &fixture.resource, 1);
    assert_eq!(array.get(&mut transaction, 0).unwrap(), Ok(11));
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = fixture.harness.events();
    assert_eq!(callback_keys(&events, Callback::Acquire), [1]);
    assert_eq!(
        finish_events(&events),
        [(1, FinishDisposition::Committed, true)],
        "materialization must use the ordinary Prepared/LockPlan protocol"
    );
    assert_eq!(
        fixture
            .resource
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        1
    );

    fixture.harness.clear();
    let mut transaction = worker.begin().unwrap();
    stage_direct_read_write(&mut transaction, &fixture.resource, 2);
    assert_eq!(
        fixture
            .resource
            .adapter()
            .capability_calls
            .load(Ordering::Relaxed),
        2,
        "materialization must reset the incremental capability summary"
    );
    transaction.abort();
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
    let array = TxnArray::new(&runtime, [11_u64]).unwrap();
    let mut worker = runtime.attach().unwrap();

    // Leave four genuinely ordinary pooled slots retaining the first adapter.
    // The array access forces the initial homogeneous prefix out of typed
    // storage before commit.
    let mut transaction = worker.begin().unwrap();
    for key in [1, 2, 3, 4] {
        transaction.with_item(&ordinary, key, |_| Ok(())).unwrap();
    }
    assert_eq!(array.get(&mut transaction, 0).unwrap(), Ok(11));
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    ordinary_harness.clear();
    drop(ordinary);

    let mut transaction = worker.begin().unwrap();
    stage_unique_write(&mut transaction, &batched, &[10, 11]);
    // This different adapter type genuinely materializes the two-item typed
    // prefix. Appending it replaces slot two while leaving slot three pooled.
    assert_eq!(array.get(&mut transaction, 0).unwrap(), Ok(11));
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Materialization replaces only the live prefix. The fourth ordinary
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

#[test]
fn terminal_read_preparation_errors_abort_and_apply_runtime_poison_policy() {
    let cases = [
        (
            AccessError::Conflict(Conflict::HiddenLockBusy),
            RuntimeHealth::Healthy,
        ),
        (
            AccessError::Capacity(sto_core::CapacityError::BufferLimit),
            RuntimeHealth::Healthy,
        ),
        (
            AccessError::Fault(injected_fault(AdapterPhase::Execute)),
            RuntimeHealth::Poisoned,
        ),
    ];

    for (error, expected_health) in cases {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let returned = worker
            .begin_terminal_read_batch()
            .unwrap()
            .abort_with_access_error(error);

        assert_eq!(returned, error);
        assert_eq!(runtime.health(), expected_health);
        if expected_health == RuntimeHealth::Healthy {
            // Consuming the open handle must have definitely released the
            // worker and recycled its scratch, not merely marked it doomed.
            worker.begin().unwrap().abort();
        }
    }
}

#[test]
fn terminal_read_commit_uses_only_validation_and_reverse_drop_cleanup() {
    let fixture = terminal_read_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let keys = [1, 2, 3, 2];
    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&fixture.resource, &keys, |_, entry| {
            let key = *entry.key();
            entry.record_read(Token::upgraded(key, &fixture.harness))
        })
        .unwrap();

    assert!(fixture.harness.events().is_empty());
    assert!(matches!(
        ready.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    let events = fixture.harness.events();
    assert_eq!(
        callback_keys(&events, Callback::PreflightFreeValidation),
        keys
    );
    assert_eq!(
        callback_keys(&events, Callback::ObservationDrop),
        [2, 3, 2, 1]
    );
    assert!(!events.iter().any(|event| matches!(
        event,
        Event::ItemInit(_)
            | Event::Preflight(_)
            | Event::Finish { .. }
            | Event::PreflightFreeFinish(_)
    )));
}

#[test]
fn terminal_read_explicit_abort_and_operation_failure_are_drop_only() {
    let fixture = terminal_read_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();

    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&fixture.resource, &[1, 2, 3], |_, entry| {
            let key = *entry.key();
            entry.record_read(Token::upgraded(key, &fixture.harness))
        })
        .unwrap();
    ready.abort();
    assert_eq!(
        callback_keys(&fixture.harness.events(), Callback::ObservationDrop),
        [3, 2, 1]
    );
    assert!(callback_keys(&fixture.harness.events(), Callback::PreflightFreeValidation).is_empty());

    fixture.harness.clear();
    let result = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&fixture.resource, &[4, 5, 6], |index, entry| {
            if index == 1 {
                return Err(InvalidUse::IllegalItemState.into());
            }
            let key = *entry.key();
            entry.record_read(Token::upgraded(key, &fixture.harness))
        });
    assert!(matches!(
        result,
        Err(AccessError::InvalidUse(InvalidUse::IllegalItemState))
    ));
    assert_eq!(
        callback_keys(&fixture.harness.events(), Callback::ObservationDrop),
        [4]
    );
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn terminal_read_operation_unwind_aborts_recorded_prefix_and_reuses_worker() {
    let fixture = terminal_read_fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let unwind = catch_unwind(AssertUnwindSafe(|| {
        let _ = worker
            .begin_terminal_read_batch()
            .unwrap()
            .with_terminal_read_batch(&fixture.resource, &[1, 2, 3], |index, entry| {
                let key = *entry.key();
                entry.record_read(Token::upgraded(key, &fixture.harness))?;
                assert_ne!(index, 1, "injected terminal operation panic");
                Ok(())
            });
    }));
    assert!(unwind.is_err());
    assert_eq!(
        callback_keys(&fixture.harness.events(), Callback::ObservationDrop),
        [2, 1]
    );

    fixture.harness.clear();
    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&fixture.resource, &[9], |_, entry| {
            entry.record_read(Token::inert())
        })
        .unwrap();
    assert!(matches!(
        ready.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn terminal_read_validation_failures_keep_definite_abort_classification() {
    let conflict = terminal_read_fixture(vec![(
        Callback::PreflightFreeValidation,
        2,
        Action::Conflict,
    )]);
    let mut worker = conflict.runtime.attach().unwrap();
    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&conflict.resource, &[1, 2, 3], |_, entry| {
            let key = *entry.key();
            entry.record_read(Token::upgraded(key, &conflict.harness))
        })
        .unwrap();
    assert_eq!(
        ready.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    assert_eq!(conflict.runtime.health(), RuntimeHealth::Healthy);
    assert_eq!(
        callback_keys(&conflict.harness.events(), Callback::ObservationDrop),
        [3, 2, 1]
    );

    for action in [Action::Fault, Action::Panic] {
        let fixture = terminal_read_fixture(vec![(Callback::PreflightFreeValidation, 1, action)]);
        let mut worker = fixture.runtime.attach().unwrap();
        let ready = worker
            .begin_terminal_read_batch()
            .unwrap()
            .with_terminal_read_batch(&fixture.resource, &[1], |_, entry| {
                let key = *entry.key();
                entry.record_read(Token::upgraded(key, &fixture.harness))
            })
            .unwrap();
        assert!(matches!(
            ready.commit(),
            Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(AbortReason::Internal(_)),
                ..
            })
        ));
        assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
        assert_eq!(
            callback_keys(&fixture.harness.events(), Callback::ObservationDrop),
            [1]
        );
    }
}

#[test]
fn terminal_read_post_certification_drop_panic_is_definitely_committed() {
    let fixture = terminal_read_fixture(vec![(Callback::ObservationDrop, 2, Action::Panic)]);
    let mut worker = fixture.runtime.attach().unwrap();
    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&fixture.resource, &[1, 2, 3], |_, entry| {
            let key = *entry.key();
            entry.record_read(Token::upgraded(key, &fixture.harness))
        })
        .unwrap();
    assert!(matches!(
        ready.commit(),
        Err(CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Committed(_),
            ..
        })
    ));
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Poisoned);
    assert_eq!(
        callback_keys(&fixture.harness.events(), Callback::ObservationDrop),
        [3, 2]
    );
}

#[test]
fn terminal_read_requires_its_stronger_explicit_capability() {
    let fixture = fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    {
        let result = worker
            .begin_terminal_read_batch()
            .unwrap()
            .with_terminal_read_batch(&fixture.resource, &[1], |_, entry| {
                entry.record_read(Token::inert())
            });
        assert_eq!(
            result.unwrap_err(),
            AccessError::Unsupported(sto_core::Unsupported::Capability("terminal read batch"))
        );
    }
    assert!(fixture.harness.events().is_empty());
    assert_eq!(fixture.runtime.health(), RuntimeHealth::Healthy);
    assert!(worker.begin().is_ok());
}

#[test]
fn terminal_read_pool_contains_adapter_panic_on_rebind() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_harness) = register_inject_resource_with_protocols(
        &runtime,
        97,
        vec![(Callback::AdapterDrop, 0, Action::Panic)],
        PreflightFreeReadMode::Disabled,
        true,
    );
    let (second, _) = register_inject_resource_with_protocols(
        &runtime,
        96,
        Vec::new(),
        PreflightFreeReadMode::Disabled,
        true,
    );
    let mut worker = runtime.attach().unwrap();

    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&first, &[1], |_, entry| entry.record_read(Token::inert()))
        .unwrap();
    assert!(matches!(
        ready.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    drop(first);

    let result = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&second, &[2], |_, entry| entry.record_read(Token::inert()));
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
}

#[test]
fn terminal_read_pool_contains_adapter_panic_when_worker_drops() {
    let Fixture {
        runtime,
        resource,
        harness,
    } = terminal_read_fixture(vec![(Callback::AdapterDrop, 0, Action::Panic)]);
    let mut worker = runtime.attach().unwrap();
    let ready = worker
        .begin_terminal_read_batch()
        .unwrap()
        .with_terminal_read_batch(&resource, &[1], |_, entry| {
            entry.record_read(Token::inert())
        })
        .unwrap();
    assert!(matches!(
        ready.commit().unwrap(),
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
