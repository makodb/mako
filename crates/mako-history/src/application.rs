//! Application-level checks for the volatile cache and asynchronous backend.
//!
//! The transaction history remains the source of truth for serializability or
//! opacity.  This module adds observations made above that boundary: cache
//! sequence allocation, exact backend batches, volatile/applied frontiers, and
//! `wait_applied` barriers.  A successful backend write means only that the
//! configured backend accepted one atomic batch; it deliberately says nothing
//! about RocksDB WAL synchronization or disk durability.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::{self, Write};
use std::num::NonZeroU64;

use crate::checker::check_with_precedence;
use crate::{
    check, CheckFailure, CheckOptions, CheckWitness, History, Interval, Observation, Operation,
    Semantics, State, StateKey, TerminalKind, TerminalOutcome, Tick, Transaction, TxnId,
};

/// Nonzero sequence allocated to one prepared cache write.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CacheSeq(NonZeroU64);

impl CacheSeq {
    /// Construct a cache sequence, returning `None` for the reserved zero value.
    pub const fn new(value: u64) -> Option<Self> {
        match NonZeroU64::new(value) {
            Some(value) => Some(Self(value)),
            None => None,
        }
    }

    /// Return the underlying nonzero integer.
    pub const fn get(self) -> u64 {
        self.0.get()
    }
}

impl TryFrom<u64> for CacheSeq {
    type Error = &'static str;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        Self::new(value).ok_or("cache sequence zero is reserved")
    }
}

impl From<CacheSeq> for u64 {
    fn from(value: CacheSeq) -> Self {
        value.get()
    }
}

impl fmt::Display for CacheSeq {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.get().fmt(formatter)
    }
}

/// Application-visible disposition of one terminal transaction call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApplicationCommitOutcome {
    /// A nonempty write transaction returned success with its cache sequence.
    AcknowledgedWrite { seq: CacheSeq },
    /// A read-only/no-op transaction returned success without allocating a slot.
    AcknowledgedNoWrite,
    /// OCC rejected the commit.
    Conflict,
    /// The transaction explicitly or voluntarily aborted.
    Aborted,
    /// Commit may have installed native writes, so the allocated slot remains pinned.
    UnknownPinned { seq: CacheSeq },
    /// Native commit is definitely visible, but an earlier unknown slot won
    /// the publication race. This complete record remains pinned and is not
    /// acknowledged or sent to the backend.
    CommittedPinned {
        seq: CacheSeq,
        prior_unknown: CacheSeq,
    },
}

impl ApplicationCommitOutcome {
    fn sequence(self) -> Option<CacheSeq> {
        match self {
            Self::AcknowledgedWrite { seq }
            | Self::UnknownPinned { seq }
            | Self::CommittedPinned { seq, .. } => Some(seq),
            Self::AcknowledgedNoWrite | Self::Conflict | Self::Aborted => None,
        }
    }
}

/// Wrapper-level observation of one transaction's terminal call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplicationCommit {
    /// Transaction in [`ApplicationHistory::transactions`].
    pub transaction: TxnId,
    /// Exact invocation/response interval seen by the application adapter.
    ///
    /// For [`ApplicationCommitOutcome::UnknownPinned`], this has a response even
    /// though the wrapped transaction history deliberately leaves the native
    /// commit pending: the wrapper returned, but cannot complete the native
    /// operation as committed or aborted.
    pub interval: Interval,
    /// Wrapper-level result.
    pub outcome: ApplicationCommitOutcome,
}

impl ApplicationCommit {
    /// Construct one application commit observation.
    pub const fn new(
        transaction: TxnId,
        interval: Interval,
        outcome: ApplicationCommitOutcome,
    ) -> Self {
        Self {
            transaction,
            interval,
            outcome,
        }
    }
}

/// One exact binary mutation carried by a cache record/backend batch.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ModelMutation {
    /// Upsert an exact table/key/value tuple.
    Put {
        /// Target table.
        table: u64,
        /// Exact binary key.
        key: Vec<u8>,
        /// Exact binary value.
        value: Vec<u8>,
    },
    /// Delete an exact table/key tuple.
    Delete {
        /// Target table.
        table: u64,
        /// Exact binary key.
        key: Vec<u8>,
    },
}

impl ModelMutation {
    /// Construct an upsert mutation.
    pub fn put(table: u64, key: impl Into<Vec<u8>>, value: impl Into<Vec<u8>>) -> Self {
        Self::Put {
            table,
            key: key.into(),
            value: value.into(),
        }
    }

    /// Construct a delete mutation.
    pub fn delete(table: u64, key: impl Into<Vec<u8>>) -> Self {
        Self::Delete {
            table,
            key: key.into(),
        }
    }

    fn state_key(&self) -> StateKey {
        match self {
            Self::Put { table, key, .. } | Self::Delete { table, key } => {
                StateKey::new(*table, key.clone())
            }
        }
    }

    fn apply(&self, state: &mut State) {
        match self {
            Self::Put { table, key, value } => {
                state.insert(StateKey::new(*table, key.clone()), value.clone());
            }
            Self::Delete { table, key } => {
                state.remove(&StateKey::new(*table, key.clone()));
            }
        }
    }
}

/// Result of one asynchronous backend batch attempt.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackendAttemptOutcome {
    /// The backend accepted the complete atomic batch.
    Succeeded,
    /// The backend rejected the batch; retrying the same sequence is allowed.
    Failed,
}

/// One asynchronous attempt to apply a complete cache record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BackendAttempt {
    /// Cache record being applied.
    pub seq: CacheSeq,
    /// Backend-call interval.
    pub interval: Interval,
    /// Complete batch presented to the backend.
    pub mutations: Vec<ModelMutation>,
    /// Backend-call result.
    pub outcome: BackendAttemptOutcome,
}

impl BackendAttempt {
    /// Construct one backend attempt.
    pub fn new(
        seq: CacheSeq,
        interval: Interval,
        mutations: Vec<ModelMutation>,
        outcome: BackendAttemptOutcome,
    ) -> Self {
        Self {
            seq,
            interval,
            mutations,
            outcome,
        }
    }
}

/// One simultaneous observation of cache/application frontiers and optional state.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FrontierObservation {
    /// Observation-call interval.
    pub interval: Interval,
    /// Largest sequence whose write commit had been acknowledged, if any.
    ///
    /// This is a maximum, not a dense prefix: an earlier pinned unknown may
    /// exist below it.
    pub highest_acknowledged: Option<CacheSeq>,
    /// Dense prefix published as applied by the asynchronous worker.
    ///
    /// This may lag a batch that the backend has already accepted.
    pub applied: Option<CacheSeq>,
    /// Optional stable snapshot of authoritative native/visible state.
    pub visible_state: Option<State>,
    /// Optional stable snapshot of backend state.
    pub backend_state: Option<State>,
}

/// Result observed from a wait-for-application barrier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitAppliedOutcome {
    /// Every sequence through `target` was applied. `None` is the valid empty
    /// barrier before any write has been acknowledged.
    Ok { target: Option<CacheSeq> },
    /// The first unresolved sequence through the barrier failed in the backend.
    BackendFailed { seq: CacheSeq },
    /// The first unresolved sequence has an unknown native commit outcome.
    Unknown { seq: CacheSeq },
}

impl WaitAppliedOutcome {
    fn target(self) -> Option<CacheSeq> {
        match self {
            Self::Ok { target } => target,
            Self::BackendFailed { seq } | Self::Unknown { seq } => Some(seq),
        }
    }

    fn target_lower_bound(self) -> u64 {
        self.target().map_or(0, CacheSeq::get)
    }
}

/// One completed `wait_applied` call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WaitAppliedObservation {
    /// Wait invocation/response interval.
    pub interval: Interval,
    /// Observed result.
    pub outcome: WaitAppliedOutcome,
}

/// Transaction history plus all observations above the native transaction boundary.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplicationHistory {
    /// Independent operation/transaction history.
    pub transactions: History,
    /// Wrapper-level terminal results.
    pub commits: Vec<ApplicationCommit>,
    /// Backend attempts, including failed attempts and retries.
    pub backend_attempts: Vec<BackendAttempt>,
    /// Cache/application frontier observations.
    pub frontiers: Vec<FrontierObservation>,
    /// Wait-barrier observations.
    pub waits: Vec<WaitAppliedObservation>,
}

impl ApplicationHistory {
    /// Wrap an existing independent transaction history.
    pub fn new(transactions: History) -> Self {
        Self {
            transactions,
            commits: Vec::new(),
            backend_attempts: Vec::new(),
            frontiers: Vec::new(),
            waits: Vec::new(),
        }
    }

    /// Render a stable, binary-safe diagnostic transcript.
    pub fn to_replay_text(&self) -> String {
        render(self)
    }
}

/// Broad application-layer failure class.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApplicationCheckFailureKind {
    /// The underlying strict-serializability/opacity oracle rejected the history.
    TransactionHistory,
    /// Application records are structurally inconsistent with the transaction history.
    MalformedApplicationHistory,
    /// Cache sequence order is not a legal serial order.
    IllegalCacheOrder,
    /// A cache/backend batch differs from the transaction's canonical final writes.
    MutationMismatch,
    /// Backend attempts violate dense, atomic, ordered retry behavior.
    BackendViolation,
    /// A sampled frontier or state is impossible at its real-time interval.
    FrontierViolation,
    /// A `wait_applied` result fails to cover its real-time barrier.
    BarrierViolation,
}

/// Diagnostic for a rejected application history.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplicationCheckFailure {
    /// Broad failure class.
    pub kind: ApplicationCheckFailureKind,
    /// Deterministic explanation.
    pub detail: String,
    /// Underlying oracle failure, when the first or constrained oracle rejected.
    pub transaction_failure: Option<Box<CheckFailure>>,
    /// Full binary-safe application replay transcript.
    pub replay: String,
}

impl fmt::Display for ApplicationCheckFailure {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{:?}: {}\n{}",
            self.kind, self.detail, self.replay
        )
    }
}

impl std::error::Error for ApplicationCheckFailure {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        self.transaction_failure
            .as_deref()
            .map(|source| source as &(dyn std::error::Error + 'static))
    }
}

/// Successful transaction and cache-order witnesses plus accepted backend prefix.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApplicationWitness {
    /// Witness from the requested unconstrained transaction oracle.
    pub transaction: CheckWitness,
    /// Witness from the same oracle constrained by cache sequence order.
    pub cache_order: CheckWitness,
    /// Largest densely successful backend sequence, or zero when none succeeded.
    pub successful_backend_prefix: u64,
}

#[derive(Debug)]
struct Derived<'a> {
    sequence_owners: BTreeMap<CacheSeq, &'a ApplicationCommit>,
    mutations_by_txn: BTreeMap<TxnId, Vec<ModelMutation>>,
    acknowledged_by_seq: BTreeMap<CacheSeq, &'a ApplicationCommit>,
}

/// Check the transaction history first, then every application/cache invariant.
pub fn check_application(
    history: &ApplicationHistory,
    semantics: Semantics,
    options: CheckOptions,
) -> Result<ApplicationWitness, ApplicationCheckFailure> {
    // This intentionally remains the first gate.  Application metadata can
    // never turn a nonserializable transaction execution green.
    let transaction = check(&history.transactions, semantics, options).map_err(|source| {
        failure_with_source(
            history,
            ApplicationCheckFailureKind::TransactionHistory,
            format!("transaction oracle rejected the history: {}", source.detail),
            source,
        )
    })?;

    let derived = validate_commits(history)?;
    validate_application_ticks(history)?;
    let precedence = sequence_precedence(&derived.sequence_owners);
    let cache_order = check_with_precedence(&history.transactions, semantics, options, &precedence)
        .map_err(|source| {
            failure_with_source(
                history,
                ApplicationCheckFailureKind::IllegalCacheOrder,
                format!(
                    "cache sequence order is not a legal {:?} serialization: {}",
                    semantics, source.detail
                ),
                source,
            )
        })?;

    let backend = validate_backend(history, &derived)?;
    validate_waits(history, &derived, &backend)?;
    validate_frontiers(history, &derived, &backend)?;

    Ok(ApplicationWitness {
        transaction,
        cache_order,
        successful_backend_prefix: backend.successful_prefix,
    })
}

/// Enforce the recorder's one-global-clock contract across the transaction
/// oracle and every observation made above it. Application commits
/// intentionally reuse their matching native terminal invocation/response;
/// `validate_commits` has already proved those aliases exact. An unknown
/// outcome has a new wrapper response while the native terminal remains
/// pending, so that response is recorded like every other application event.
fn validate_application_ticks(history: &ApplicationHistory) -> Result<(), ApplicationCheckFailure> {
    let mut ticks = BTreeMap::new();
    let mut record_tick = |tick: Tick, label: String| {
        if let Some(previous) = ticks.insert(tick, label.clone()) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!("logical tick {tick} is shared by {previous} and {label}"),
            ));
        }
        Ok(())
    };

    for transaction in &history.transactions.transactions {
        record_interval_ticks(
            transaction.begin,
            format!("T{} begin", transaction.id),
            &mut record_tick,
        )?;
        for (index, operation) in transaction.operations.iter().enumerate() {
            record_interval_ticks(
                operation.interval,
                format!("T{} operation {index}", transaction.id),
                &mut record_tick,
            )?;
        }
        if let Some(terminal) = &transaction.terminal {
            record_interval_ticks(
                terminal.interval,
                format!("T{} terminal", transaction.id),
                &mut record_tick,
            )?;
        }
    }

    for commit in &history.commits {
        if matches!(
            commit.outcome,
            ApplicationCommitOutcome::UnknownPinned { .. }
        ) {
            record_tick(
                commit
                    .interval
                    .response
                    .expect("validate_commits requires an unknown wrapper response"),
                format!("T{} unknown wrapper response", commit.transaction),
            )?;
        }
    }
    for (index, attempt) in history.backend_attempts.iter().enumerate() {
        record_interval_ticks(
            attempt.interval,
            format!("backend attempt {index} for sequence {}", attempt.seq),
            &mut record_tick,
        )?;
    }
    for (index, frontier) in history.frontiers.iter().enumerate() {
        record_interval_ticks(
            frontier.interval,
            format!("frontier observation {index}"),
            &mut record_tick,
        )?;
    }
    for (index, wait) in history.waits.iter().enumerate() {
        record_interval_ticks(
            wait.interval,
            format!("wait_applied observation {index}"),
            &mut record_tick,
        )?;
    }
    Ok(())
}

fn record_interval_ticks<E>(
    interval: Interval,
    label: String,
    record_tick: &mut impl FnMut(Tick, String) -> Result<(), E>,
) -> Result<(), E> {
    record_tick(interval.invocation, format!("{label} invocation"))?;
    if let Some(response) = interval.response {
        record_tick(response, format!("{label} response"))?;
    }
    Ok(())
}

fn validate_commits<'a>(
    history: &'a ApplicationHistory,
) -> Result<Derived<'a>, ApplicationCheckFailure> {
    let transactions: BTreeMap<_, _> = history
        .transactions
        .transactions
        .iter()
        .map(|transaction| (transaction.id, transaction))
        .collect();
    let mutations_by_txn: BTreeMap<_, _> = transactions
        .iter()
        .map(|(id, transaction)| (*id, canonical_mutations(transaction)))
        .collect();

    let mut commits_by_txn = BTreeMap::new();
    let mut sequence_owners = BTreeMap::new();
    let mut acknowledged_by_seq = BTreeMap::new();
    for commit in &history.commits {
        let Some(transaction) = transactions.get(&commit.transaction).copied() else {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "application commit refers to missing T{}",
                    commit.transaction
                ),
            ));
        };
        if commits_by_txn.insert(commit.transaction, commit).is_some() {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!("duplicate application commit for T{}", commit.transaction),
            ));
        }
        let Some(terminal) = transaction.terminal.as_ref() else {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "application commit for T{} has no terminal transaction call",
                    commit.transaction
                ),
            ));
        };
        let interval_matches = match commit.outcome {
            ApplicationCommitOutcome::UnknownPinned { .. } => {
                terminal.kind == TerminalKind::Commit
                    && terminal.interval.response.is_none()
                    && commit.interval.invocation == terminal.interval.invocation
                    && commit
                        .interval
                        .response
                        .is_some_and(|response| response > commit.interval.invocation)
            }
            _ => commit.interval == terminal.interval,
        };
        if !interval_matches {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "application commit interval {:?} for T{} is incompatible with terminal interval {:?}",
                    commit.interval, commit.transaction, terminal.interval
                ),
            ));
        }

        let mutations = &mutations_by_txn[&commit.transaction];
        let outcome_matches = match (terminal.kind, terminal.outcome, commit.outcome) {
            (
                TerminalKind::Commit,
                Some(TerminalOutcome::Committed),
                ApplicationCommitOutcome::AcknowledgedWrite { .. },
            ) => !mutations.is_empty(),
            (
                TerminalKind::Commit,
                Some(TerminalOutcome::Committed),
                ApplicationCommitOutcome::AcknowledgedNoWrite,
            ) => mutations.is_empty(),
            (
                TerminalKind::Commit,
                Some(TerminalOutcome::Committed),
                ApplicationCommitOutcome::CommittedPinned { .. },
            ) => !mutations.is_empty(),
            (
                TerminalKind::Commit,
                Some(TerminalOutcome::Conflict),
                ApplicationCommitOutcome::Conflict,
            ) => true,
            (
                TerminalKind::Commit,
                Some(TerminalOutcome::Aborted),
                ApplicationCommitOutcome::Aborted,
            ) => true,
            (
                TerminalKind::Abort,
                Some(TerminalOutcome::Aborted),
                ApplicationCommitOutcome::Aborted,
            ) => true,
            (TerminalKind::Commit, None, ApplicationCommitOutcome::UnknownPinned { .. }) => {
                !mutations.is_empty()
            }
            _ => false,
        };
        if !outcome_matches {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "application outcome {:?} for T{} is incompatible with {:?} {:?} and {} canonical mutations",
                    commit.outcome,
                    commit.transaction,
                    terminal.kind,
                    terminal.outcome,
                    mutations.len()
                ),
            ));
        }

        if let Some(seq) = commit.outcome.sequence() {
            if let Some(previous) = sequence_owners.insert(seq, commit) {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::MalformedApplicationHistory,
                    format!(
                        "cache sequence {seq} is owned by both T{} and T{}",
                        previous.transaction, commit.transaction
                    ),
                ));
            }
            if matches!(
                commit.outcome,
                ApplicationCommitOutcome::AcknowledgedWrite { .. }
            ) {
                acknowledged_by_seq.insert(seq, commit);
            }
        }
    }

    for transaction in transactions.values() {
        let requires_mapping = transaction
            .terminal
            .as_ref()
            .is_some_and(|terminal| terminal.interval.response.is_some());
        if requires_mapping && !commits_by_txn.contains_key(&transaction.id) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "T{} terminal call has no application commit mapping",
                    transaction.id
                ),
            ));
        }
    }

    for (expected, seq) in (1_u64..).zip(sequence_owners.keys().copied()) {
        if seq.get() != expected {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "allocated cache sequences are not dense: expected {expected}, found {seq}; acknowledged maxima may have gaps, allocated slots may not"
                ),
            ));
        }
    }

    for (seq, commit) in &sequence_owners {
        let ApplicationCommitOutcome::CommittedPinned { prior_unknown, .. } = commit.outcome else {
            continue;
        };
        if prior_unknown >= *seq {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "committed pinned sequence {seq} names non-prior unknown sequence {prior_unknown}"
                ),
            ));
        }
        let Some(prior) = sequence_owners.get(&prior_unknown) else {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "committed pinned sequence {seq} names unallocated unknown sequence {prior_unknown}"
                ),
            ));
        };
        if !matches!(
            prior.outcome,
            ApplicationCommitOutcome::UnknownPinned { .. }
        ) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "committed pinned sequence {seq} names sequence {prior_unknown}, but T{} is {:?}",
                    prior.transaction, prior.outcome
                ),
            ));
        }
        let first_unknown = sequence_owners
            .iter()
            .find(|(candidate, owner)| {
                **candidate < *seq
                    && matches!(
                        owner.outcome,
                        ApplicationCommitOutcome::UnknownPinned { .. }
                    )
            })
            .map(|(candidate, _)| *candidate)
            .expect("the named prior unknown was validated above");
        if prior_unknown != first_unknown {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MalformedApplicationHistory,
                format!(
                    "committed pinned sequence {seq} names unknown {prior_unknown}, but the first unknown is {first_unknown}"
                ),
            ));
        }
    }

    for (unknown_seq, unknown) in sequence_owners.iter().filter(|(_, commit)| {
        matches!(
            commit.outcome,
            ApplicationCommitOutcome::UnknownPinned { .. }
        )
    }) {
        let unknown_response = unknown
            .interval
            .response
            .expect("application unknown outcomes have a wrapper response");
        for (later_seq, later) in sequence_owners.iter().filter(|(later_seq, later)| {
            *later_seq > unknown_seq
                && matches!(
                    later.outcome,
                    ApplicationCommitOutcome::AcknowledgedWrite { .. }
                        | ApplicationCommitOutcome::CommittedPinned { .. }
                )
        }) {
            if later.interval.invocation >= unknown_response {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::MalformedApplicationHistory,
                    format!(
                        "committed suffix sequence {later_seq} (T{}) began after unknown sequence {unknown_seq} (T{}) had already fail-stopped admission",
                        later.transaction, unknown.transaction
                    ),
                ));
            }
        }
    }

    Ok(Derived {
        sequence_owners,
        mutations_by_txn,
        acknowledged_by_seq,
    })
}

fn canonical_mutations(transaction: &Transaction) -> Vec<ModelMutation> {
    #[derive(Debug)]
    struct KeyView {
        initially_present: bool,
        present: bool,
        value: Option<Vec<u8>>,
        changed: bool,
    }

    let mut views: BTreeMap<StateKey, KeyView> = BTreeMap::new();
    for timed in &transaction.operations {
        let Some(observation) = timed.observation.as_ref() else {
            break;
        };
        if matches!(observation, Observation::Conflict) {
            break;
        }
        match (&timed.operation, observation) {
            (Operation::Put { table, key, value }, Observation::Put { created }) => {
                let state_key = StateKey::new(*table, key.clone());
                let view = views.entry(state_key).or_insert_with(|| KeyView {
                    initially_present: !created,
                    present: !created,
                    value: None,
                    changed: false,
                });
                view.present = true;
                view.value = Some(value.clone());
                view.changed = true;
            }
            (Operation::Put { table, key, value }, Observation::PutWithPrevious { previous }) => {
                let state_key = StateKey::new(*table, key.clone());
                let view = views.entry(state_key).or_insert_with(|| KeyView {
                    initially_present: previous.is_some(),
                    present: previous.is_some(),
                    value: previous.clone(),
                    changed: false,
                });
                view.present = true;
                view.value = Some(value.clone());
                view.changed = true;
            }
            (Operation::Insert { table, key, value }, Observation::Insert { inserted: true }) => {
                let state_key = StateKey::new(*table, key.clone());
                let view = views.entry(state_key).or_insert_with(|| KeyView {
                    initially_present: false,
                    present: false,
                    value: None,
                    changed: false,
                });
                view.present = true;
                view.value = Some(value.clone());
                view.changed = true;
            }
            (
                Operation::Insert { table, key, value },
                Observation::InsertWithPrevious { previous: None },
            ) => {
                let state_key = StateKey::new(*table, key.clone());
                let view = views.entry(state_key).or_insert_with(|| KeyView {
                    initially_present: false,
                    present: false,
                    value: None,
                    changed: false,
                });
                view.present = true;
                view.value = Some(value.clone());
                view.changed = true;
            }
            (Operation::Remove { table, key }, Observation::Remove { existed: true }) => {
                let state_key = StateKey::new(*table, key.clone());
                let view = views.entry(state_key).or_insert_with(|| KeyView {
                    initially_present: true,
                    present: true,
                    value: None,
                    changed: false,
                });
                view.present = false;
                view.value = None;
                view.changed = true;
            }
            (
                Operation::Remove { table, key },
                Observation::RemoveWithPrevious {
                    previous: Some(previous),
                },
            ) => {
                let state_key = StateKey::new(*table, key.clone());
                let view = views.entry(state_key).or_insert_with(|| KeyView {
                    initially_present: true,
                    present: true,
                    value: Some(previous.clone()),
                    changed: false,
                });
                view.present = false;
                view.value = None;
                view.changed = true;
            }
            (
                Operation::Get { .. } | Operation::Scan { .. },
                Observation::Get(_) | Observation::Scan(_),
            ) => {}
            (Operation::Insert { table, key, .. }, Observation::Insert { inserted: false }) => {
                views
                    .entry(StateKey::new(*table, key.clone()))
                    .or_insert(KeyView {
                        initially_present: true,
                        present: true,
                        value: None,
                        changed: false,
                    });
            }
            (
                Operation::Insert { table, key, .. },
                Observation::InsertWithPrevious {
                    previous: Some(previous),
                },
            ) => {
                views
                    .entry(StateKey::new(*table, key.clone()))
                    .or_insert(KeyView {
                        initially_present: true,
                        present: true,
                        value: Some(previous.clone()),
                        changed: false,
                    });
            }
            (Operation::Remove { table, key }, Observation::Remove { existed: false }) => {
                views
                    .entry(StateKey::new(*table, key.clone()))
                    .or_insert(KeyView {
                        initially_present: false,
                        present: false,
                        value: None,
                        changed: false,
                    });
            }
            (
                Operation::Remove { table, key },
                Observation::RemoveWithPrevious { previous: None },
            ) => {
                views
                    .entry(StateKey::new(*table, key.clone()))
                    .or_insert(KeyView {
                        initially_present: false,
                        present: false,
                        value: None,
                        changed: false,
                    });
            }
            _ => unreachable!("the transaction oracle validates operation observations first"),
        }
    }
    views
        .into_iter()
        .filter_map(
            |(key, view)| match (view.initially_present, view.present, view.changed) {
                (false, false, _) | (_, _, false) => None,
                (_, false, true) => Some(ModelMutation::delete(key.table, key.key)),
                (_, true, true) => Some(ModelMutation::put(
                    key.table,
                    key.key,
                    view.value
                        .expect("a changed present key has a staged final value"),
                )),
            },
        )
        .collect()
}

fn sequence_precedence(owners: &BTreeMap<CacheSeq, &ApplicationCommit>) -> Vec<(TxnId, TxnId)> {
    let transactions: Vec<_> = owners.values().map(|commit| commit.transaction).collect();
    let mut precedence = Vec::new();
    for (index, before) in transactions.iter().copied().enumerate() {
        for after in transactions.iter().copied().skip(index + 1) {
            if before != after {
                precedence.push((before, after));
            }
        }
    }
    precedence
}

#[derive(Debug)]
struct BackendDerived<'a> {
    successful_prefix: u64,
    successful_attempts: BTreeMap<CacheSeq, &'a BackendAttempt>,
    attempts_by_seq: BTreeMap<CacheSeq, Vec<&'a BackendAttempt>>,
}

fn validate_backend<'a>(
    history: &'a ApplicationHistory,
    derived: &Derived<'a>,
) -> Result<BackendDerived<'a>, ApplicationCheckFailure> {
    let mut attempts: Vec<_> = history.backend_attempts.iter().collect();
    attempts.sort_by_key(|attempt| (attempt.interval.invocation, attempt.seq));
    let mut attempts_by_seq: BTreeMap<CacheSeq, Vec<&BackendAttempt>> = BTreeMap::new();
    let mut successful_attempts = BTreeMap::new();
    let mut previous: Option<&BackendAttempt> = None;
    let mut successful_prefix = 0_u64;

    for attempt in attempts {
        let response = completed_interval(
            history,
            "backend attempt",
            attempt.interval,
            ApplicationCheckFailureKind::BackendViolation,
        )?;
        let Some(owner) = derived.acknowledged_by_seq.get(&attempt.seq) else {
            let owner_detail = derived
                .sequence_owners
                .get(&attempt.seq)
                .map(|commit| format!("T{} is {:?}", commit.transaction, commit.outcome))
                .unwrap_or_else(|| "the sequence was never allocated".to_owned());
            return Err(failure(
                history,
                ApplicationCheckFailureKind::BackendViolation,
                format!(
                    "backend attempted sequence {} but {owner_detail}",
                    attempt.seq
                ),
            ));
        };
        if attempt.interval.invocation < owner.interval.invocation {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::BackendViolation,
                format!(
                    "backend attempt for sequence {} begins at {} before T{}'s commit invocation at {}",
                    attempt.seq,
                    attempt.interval.invocation,
                    owner.transaction,
                    owner.interval.invocation
                ),
            ));
        }
        let expected = &derived.mutations_by_txn[&owner.transaction];
        validate_exact_mutations(history, attempt, expected, owner.transaction)?;

        if let Some(previous) = previous {
            let previous_response = previous.interval.response.expect("validated completion");
            if attempt.interval.invocation <= previous_response {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::BackendViolation,
                    format!(
                        "backend attempt for sequence {} begins at {} before the prior sequence {} attempt completed at {}",
                        attempt.seq, attempt.interval.invocation, previous.seq, previous_response
                    ),
                ));
            }
            if attempt.seq < previous.seq
                || attempt.seq.get() > previous.seq.get().saturating_add(1)
            {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::BackendViolation,
                    format!(
                        "backend attempts reordered or skipped from sequence {} to {}",
                        previous.seq, attempt.seq
                    ),
                ));
            }
            if attempt.seq > previous.seq && previous.outcome != BackendAttemptOutcome::Succeeded {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::BackendViolation,
                    format!(
                        "backend advanced to sequence {} after sequence {} failed instead of retrying it",
                        attempt.seq, previous.seq
                    ),
                ));
            }
        } else if attempt.seq.get() != 1 {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::BackendViolation,
                format!(
                    "first backend attempt skipped directly to sequence {}",
                    attempt.seq
                ),
            ));
        }

        let per_seq = attempts_by_seq.entry(attempt.seq).or_default();
        if per_seq
            .iter()
            .any(|prior| prior.outcome == BackendAttemptOutcome::Succeeded)
        {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::BackendViolation,
                format!(
                    "backend retried sequence {} after it had succeeded",
                    attempt.seq
                ),
            ));
        }
        per_seq.push(attempt);
        if attempt.outcome == BackendAttemptOutcome::Succeeded {
            if successful_attempts.insert(attempt.seq, attempt).is_some() {
                unreachable!("success-after-success was rejected above");
            }
            if attempt.seq.get() != successful_prefix.saturating_add(1) {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::BackendViolation,
                    format!(
                        "successful backend batches are not a dense prefix: applied {} after prefix {}",
                        attempt.seq, successful_prefix
                    ),
                ));
            }
            successful_prefix = attempt.seq.get();
        }
        previous = Some(attempt);
        let _ = response;
    }

    Ok(BackendDerived {
        successful_prefix,
        successful_attempts,
        attempts_by_seq,
    })
}

fn validate_exact_mutations(
    history: &ApplicationHistory,
    attempt: &BackendAttempt,
    expected: &[ModelMutation],
    transaction: TxnId,
) -> Result<(), ApplicationCheckFailure> {
    let mut actual = BTreeMap::new();
    for mutation in &attempt.mutations {
        let key = mutation.state_key();
        if actual.insert(key.clone(), mutation.clone()).is_some() {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::MutationMismatch,
                format!(
                    "backend sequence {} repeats table {} key {} in one supposedly atomic batch",
                    attempt.seq,
                    key.table,
                    crate::replay::hex(&key.key)
                ),
            ));
        }
    }
    let expected: BTreeMap<_, _> = expected
        .iter()
        .cloned()
        .map(|mutation| (mutation.state_key(), mutation))
        .collect();
    if actual != expected {
        let differing_key = expected
            .keys()
            .chain(actual.keys())
            .find(|key| expected.get(*key) != actual.get(*key));
        let detail = differing_key.map_or_else(
            || "mutation sets differ".to_owned(),
            |key| {
                format!(
                    "table {} key {} expected {}, observed {}",
                    key.table,
                    crate::replay::hex(&key.key),
                    render_optional_mutation(expected.get(key)),
                    render_optional_mutation(actual.get(key))
                )
            },
        );
        return Err(failure(
            history,
            ApplicationCheckFailureKind::MutationMismatch,
            format!(
                "backend sequence {} for T{} is not its canonical final mutation set: {detail}",
                attempt.seq, transaction
            ),
        ));
    }
    Ok(())
}

fn validate_frontiers(
    history: &ApplicationHistory,
    derived: &Derived<'_>,
    backend: &BackendDerived<'_>,
) -> Result<(), ApplicationCheckFailure> {
    for frontier in &history.frontiers {
        let response = completed_interval(
            history,
            "frontier observation",
            frontier.interval,
            ApplicationCheckFailureKind::FrontierViolation,
        )?;
        let commit_required_ack = derived
            .acknowledged_by_seq
            .iter()
            .filter(|(_, commit)| {
                commit
                    .interval
                    .response
                    .is_some_and(|tick| tick < frontier.interval.invocation)
            })
            .map(|(seq, _)| *seq)
            .max();
        let wait_required_ack = history
            .waits
            .iter()
            .filter(|wait| {
                wait.interval
                    .response
                    .is_some_and(|tick| tick < frontier.interval.invocation)
            })
            .map(|wait| wait.outcome.target_lower_bound())
            .max()
            .unwrap_or(0);
        let required_ack =
            CacheSeq::new(option_seq_value(commit_required_ack).max(wait_required_ack));
        if option_seq_value(frontier.highest_acknowledged) < option_seq_value(required_ack) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "highest-acknowledged frontier {} misses real-time acknowledged sequence {}",
                    render_seq(frontier.highest_acknowledged),
                    render_seq(required_ack)
                ),
            ));
        }
        if let Some(observed) = frontier.highest_acknowledged {
            let Some(commit) = derived.acknowledged_by_seq.get(&observed) else {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "highest-acknowledged frontier names non-acknowledged sequence {observed}"
                    ),
                ));
            };
            if commit.interval.invocation >= response {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "frontier observed acknowledged sequence {observed} before T{}'s commit interval began",
                        commit.transaction
                    ),
                ));
            }
        }

        // A successful backend call can precede publication of the applied
        // frontier.  Only a completed wait is direct evidence that publication
        // advanced through its target; backend success alone is merely an upper
        // bound.
        let required_applied = history
            .waits
            .iter()
            .filter_map(|wait| match wait.outcome {
                WaitAppliedOutcome::Ok { target }
                    if wait
                        .interval
                        .response
                        .is_some_and(|tick| tick < frontier.interval.invocation) =>
                {
                    target
                }
                _ => None,
            })
            .max();
        if option_seq_value(frontier.applied) < option_seq_value(required_applied) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "applied frontier {} misses prior wait-confirmed prefix {}",
                    render_seq(frontier.applied),
                    render_seq(required_applied)
                ),
            ));
        }
        let possible_applied = backend
            .successful_attempts
            .iter()
            .filter(|(_, attempt)| {
                attempt
                    .interval
                    .response
                    .is_some_and(|tick| tick < response)
            })
            .map(|(seq, _)| *seq)
            .max();
        if option_seq_value(frontier.applied) > option_seq_value(possible_applied) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "applied frontier {} exceeds the successful backend prefix visible by this observation ({})",
                    render_seq(frontier.applied),
                    render_seq(possible_applied)
                ),
            ));
        }
        if let Some(observed) = frontier.applied {
            let Some(success) = backend.successful_attempts.get(&observed) else {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "applied frontier names sequence {observed} without a successful batch"
                    ),
                ));
            };
            if success
                .interval
                .response
                .is_none_or(|tick| tick >= response)
            {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "frontier observed applied sequence {observed} before its backend success"
                    ),
                ));
            }
        }
        if option_seq_value(frontier.applied) > option_seq_value(frontier.highest_acknowledged) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "applied frontier {} exceeds highest acknowledged {}",
                    render_seq(frontier.applied),
                    render_seq(frontier.highest_acknowledged)
                ),
            ));
        }

        if let Some(observed) = &frontier.visible_state {
            validate_visible_state(history, derived, frontier, response, observed)?;
        }
        if let Some(observed) = &frontier.backend_state {
            validate_backend_state(history, derived, backend, frontier, response, observed)?;
        }
    }

    for (left_index, left) in history.frontiers.iter().enumerate() {
        let Some(left_response) = left.interval.response else {
            continue;
        };
        for right in history.frontiers.iter().skip(left_index + 1) {
            let (earlier, later) = if left_response < right.interval.invocation {
                (left, right)
            } else if right
                .interval
                .response
                .is_some_and(|tick| tick < left.interval.invocation)
            {
                (right, left)
            } else {
                continue;
            };
            if option_seq_value(later.highest_acknowledged)
                < option_seq_value(earlier.highest_acknowledged)
                || option_seq_value(later.applied) < option_seq_value(earlier.applied)
            {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "frontiers move backward in real time: ({}, {}) then ({}, {})",
                        render_seq(earlier.highest_acknowledged),
                        render_seq(earlier.applied),
                        render_seq(later.highest_acknowledged),
                        render_seq(later.applied)
                    ),
                ));
            }
        }
    }
    Ok(())
}

fn validate_visible_state(
    history: &ApplicationHistory,
    derived: &Derived<'_>,
    frontier: &FrontierObservation,
    response: Tick,
    observed: &State,
) -> Result<(), ApplicationCheckFailure> {
    let mut expected = history.transactions.initial_state.clone();
    for transaction in &history.transactions.transactions {
        let pending_write_commit = transaction.terminal.as_ref().is_some_and(|terminal| {
            terminal.kind == TerminalKind::Commit
                && terminal.interval.response.is_none()
                && terminal.interval.invocation < response
                && !derived.mutations_by_txn[&transaction.id].is_empty()
        });
        if pending_write_commit {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "visible state cannot be checked while T{} has a pending write commit",
                    transaction.id
                ),
            ));
        }
    }
    for (seq, commit) in &derived.sequence_owners {
        let terminal_response = commit.interval.response;
        if intervals_overlap(commit.interval, frontier.interval) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "visible state snapshot overlaps T{} sequence {} commit, so it is not stable",
                    commit.transaction, seq
                ),
            ));
        }
        match commit.outcome {
            ApplicationCommitOutcome::AcknowledgedWrite { .. }
            | ApplicationCommitOutcome::CommittedPinned { .. }
                if terminal_response.is_some_and(|tick| tick < frontier.interval.invocation) =>
            {
                apply_mutations(
                    &derived.mutations_by_txn[&commit.transaction],
                    &mut expected,
                );
            }
            ApplicationCommitOutcome::AcknowledgedWrite { .. }
            | ApplicationCommitOutcome::CommittedPinned { .. }
                if commit.interval.invocation >= response => {}
            ApplicationCommitOutcome::UnknownPinned { .. }
                if commit.interval.invocation < response =>
            {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "visible state cannot be checked while T{} sequence {} has an unknown commit outcome",
                        commit.transaction, seq
                    ),
                ));
            }
            ApplicationCommitOutcome::UnknownPinned { .. } => {}
            ApplicationCommitOutcome::AcknowledgedWrite { .. }
            | ApplicationCommitOutcome::CommittedPinned { .. } => {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::FrontierViolation,
                    format!(
                        "visible state snapshot is not separated from T{} sequence {} commit",
                        commit.transaction, seq
                    ),
                ));
            }
            _ => {}
        }
    }
    if &expected != observed {
        return Err(state_failure(history, "visible state", &expected, observed));
    }
    Ok(())
}

fn validate_backend_state(
    history: &ApplicationHistory,
    derived: &Derived<'_>,
    backend: &BackendDerived<'_>,
    frontier: &FrontierObservation,
    response: Tick,
    observed: &State,
) -> Result<(), ApplicationCheckFailure> {
    let mut expected = history.transactions.initial_state.clone();
    for (seq, attempt) in &backend.successful_attempts {
        if intervals_overlap(attempt.interval, frontier.interval) {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!(
                    "backend state snapshot overlaps successful sequence {seq}, so it is not stable"
                ),
            ));
        }
        if attempt
            .interval
            .response
            .is_some_and(|tick| tick < frontier.interval.invocation)
        {
            let commit = derived
                .acknowledged_by_seq
                .get(seq)
                .expect("backend validation requires an acknowledged owner");
            apply_mutations(
                &derived.mutations_by_txn[&commit.transaction],
                &mut expected,
            );
        } else if attempt.interval.invocation < response {
            return Err(failure(
                history,
                ApplicationCheckFailureKind::FrontierViolation,
                format!("backend state snapshot is not separated from sequence {seq}"),
            ));
        }
    }
    if &expected != observed {
        return Err(state_failure(history, "backend state", &expected, observed));
    }
    Ok(())
}

fn validate_waits(
    history: &ApplicationHistory,
    derived: &Derived<'_>,
    backend: &BackendDerived<'_>,
) -> Result<(), ApplicationCheckFailure> {
    for wait in &history.waits {
        let response = completed_interval(
            history,
            "wait_applied observation",
            wait.interval,
            ApplicationCheckFailureKind::BarrierViolation,
        )?;
        let real_time_barrier = derived
            .acknowledged_by_seq
            .iter()
            .filter(|(_, commit)| {
                commit
                    .interval
                    .response
                    .is_some_and(|tick| tick < wait.interval.invocation)
            })
            .map(|(seq, _)| seq.get())
            .max()
            .unwrap_or(0);
        let prior_wait_barrier = history
            .waits
            .iter()
            .filter(|prior| {
                prior
                    .interval
                    .response
                    .is_some_and(|tick| tick < wait.interval.invocation)
            })
            .map(|prior| prior.outcome.target_lower_bound())
            .max()
            .unwrap_or(0);
        let prior_frontier_barrier = history
            .frontiers
            .iter()
            .filter(|frontier| {
                frontier
                    .interval
                    .response
                    .is_some_and(|tick| tick < wait.interval.invocation)
            })
            .map(|frontier| option_seq_value(frontier.highest_acknowledged))
            .max()
            .unwrap_or(0);
        let minimum_target = real_time_barrier
            .max(prior_wait_barrier)
            .max(prior_frontier_barrier);
        let possible_acknowledged = derived
            .acknowledged_by_seq
            .iter()
            .filter(|(_, commit)| commit.interval.invocation < response)
            .map(|(seq, _)| seq.get())
            .max()
            .unwrap_or(0);
        let barrier_target = match wait.outcome {
            WaitAppliedOutcome::Ok { target } => {
                let target_value = option_seq_value(target);
                if target_value < minimum_target {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait target {} regresses below the required acknowledged frontier {minimum_target}",
                            render_seq(target)
                        ),
                    ));
                }
                if target_value > possible_acknowledged {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait target {} exceeds the greatest acknowledgement that could exist by its response ({possible_acknowledged})",
                            render_seq(target)
                        ),
                    ));
                }
                if let Some(target) = target {
                    let Some(owner) = derived.acknowledged_by_seq.get(&target) else {
                        return Err(failure(
                            history,
                            ApplicationCheckFailureKind::BarrierViolation,
                            format!("successful wait names non-acknowledged sequence {target}"),
                        ));
                    };
                    if owner.interval.invocation >= response {
                        return Err(failure(
                            history,
                            ApplicationCheckFailureKind::BarrierViolation,
                            format!(
                                "wait reported sequence {target} before T{}'s commit interval began",
                                owner.transaction
                            ),
                        ));
                    }
                }
                target_value
            }
            WaitAppliedOutcome::BackendFailed { seq } | WaitAppliedOutcome::Unknown { seq } => {
                let target = minimum_target.max(seq.get());
                if target > possible_acknowledged {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait reported unresolved sequence {seq}, but no acknowledged barrier through {target} could exist by its response"
                        ),
                    ));
                }
                target
            }
        };
        if let Some(reported) = wait.outcome.target() {
            let Some(reported_owner) = derived.sequence_owners.get(&reported) else {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::BarrierViolation,
                    format!("wait names unallocated sequence {reported}"),
                ));
            };
            if reported_owner.interval.invocation >= response {
                return Err(failure(
                    history,
                    ApplicationCheckFailureKind::BarrierViolation,
                    format!(
                        "wait reported sequence {reported} before T{}'s commit interval began",
                        reported_owner.transaction
                    ),
                ));
            }
        }

        let first_unresolved = (1..=barrier_target).find_map(|value| {
            let seq = CacheSeq::new(value).expect("positive range");
            let owner = derived.sequence_owners.get(&seq)?;
            if matches!(
                owner.outcome,
                ApplicationCommitOutcome::UnknownPinned { .. }
            ) {
                return Some((seq, "unknown"));
            }
            let succeeded = backend
                .successful_attempts
                .get(&seq)
                .is_some_and(|attempt| {
                    attempt
                        .interval
                        .response
                        .is_some_and(|tick| tick < response)
                });
            (!succeeded).then_some((seq, "backend"))
        });

        match wait.outcome {
            WaitAppliedOutcome::Ok { target } => {
                if let Some((seq, reason)) = first_unresolved {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait reported Ok through {}, but sequence {seq} remained {reason}-unresolved at its response",
                            render_seq(target)
                        ),
                    ));
                }
            }
            WaitAppliedOutcome::Unknown { seq } => {
                if first_unresolved != Some((seq, "unknown")) {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait reported unknown sequence {seq}, but its first unresolved slot was {}",
                            render_unresolved(first_unresolved)
                        ),
                    ));
                }
            }
            WaitAppliedOutcome::BackendFailed { seq } => {
                if first_unresolved != Some((seq, "backend")) {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait reported backend failure at sequence {seq}, but its first unresolved slot was {}",
                            render_unresolved(first_unresolved)
                        ),
                    ));
                }
                let has_visible_failure =
                    backend.attempts_by_seq.get(&seq).is_some_and(|attempts| {
                        let latest = attempts
                            .iter()
                            .filter(|attempt| {
                                attempt
                                    .interval
                                    .response
                                    .is_some_and(|tick| tick < response)
                            })
                            .max_by_key(|attempt| attempt.interval.response);
                        latest
                            .is_some_and(|attempt| attempt.outcome == BackendAttemptOutcome::Failed)
                    });
                if !has_visible_failure {
                    return Err(failure(
                        history,
                        ApplicationCheckFailureKind::BarrierViolation,
                        format!(
                            "wait reported backend failure at sequence {seq} without a completed failed attempt"
                        ),
                    ));
                }
            }
        }
    }
    Ok(())
}

fn completed_interval(
    history: &ApplicationHistory,
    label: &str,
    interval: Interval,
    kind: ApplicationCheckFailureKind,
) -> Result<Tick, ApplicationCheckFailure> {
    let Some(response) = interval.response else {
        return Err(failure(history, kind, format!("{label} is still pending")));
    };
    if response <= interval.invocation {
        return Err(failure(
            history,
            kind,
            format!(
                "{label} responds at {response} no later than invocation at {}",
                interval.invocation
            ),
        ));
    }
    Ok(response)
}

fn intervals_overlap(left: Interval, right: Interval) -> bool {
    let left_response = left.response.unwrap_or(Tick::MAX);
    let right_response = right.response.unwrap_or(Tick::MAX);
    left.invocation < right_response && right.invocation < left_response
}

fn apply_mutations(mutations: &[ModelMutation], state: &mut State) {
    for mutation in mutations {
        mutation.apply(state);
    }
}

fn option_seq_value(seq: Option<CacheSeq>) -> u64 {
    seq.map_or(0, CacheSeq::get)
}

fn render_seq(seq: Option<CacheSeq>) -> String {
    seq.map_or_else(|| "none".to_owned(), |seq| seq.to_string())
}

fn render_unresolved(unresolved: Option<(CacheSeq, &'static str)>) -> String {
    unresolved.map_or_else(
        || "none".to_owned(),
        |(seq, reason)| format!("{seq} ({reason})"),
    )
}

fn state_failure(
    history: &ApplicationHistory,
    label: &str,
    expected: &State,
    observed: &State,
) -> ApplicationCheckFailure {
    let keys: BTreeSet<_> = expected.keys().chain(observed.keys()).collect();
    let detail = keys
        .into_iter()
        .find_map(|key| {
            (expected.get(key) != observed.get(key)).then(|| {
                format!(
                    "{label} differs at table {} key {}: expected {}, observed {}",
                    key.table,
                    crate::replay::hex(&key.key),
                    render_optional_bytes(expected.get(key)),
                    render_optional_bytes(observed.get(key))
                )
            })
        })
        .unwrap_or_else(|| format!("{label} differs"));
    failure(
        history,
        ApplicationCheckFailureKind::FrontierViolation,
        detail,
    )
}

fn render_optional_bytes(value: Option<&Vec<u8>>) -> String {
    value
        .map(|value| crate::replay::hex(value))
        .unwrap_or_else(|| "missing".to_owned())
}

fn render_optional_mutation(mutation: Option<&ModelMutation>) -> String {
    mutation
        .map(render_mutation)
        .unwrap_or_else(|| "missing".to_owned())
}

fn failure(
    history: &ApplicationHistory,
    kind: ApplicationCheckFailureKind,
    detail: String,
) -> ApplicationCheckFailure {
    ApplicationCheckFailure {
        kind,
        detail,
        transaction_failure: None,
        replay: history.to_replay_text(),
    }
}

fn failure_with_source(
    history: &ApplicationHistory,
    kind: ApplicationCheckFailureKind,
    detail: String,
    source: CheckFailure,
) -> ApplicationCheckFailure {
    ApplicationCheckFailure {
        kind,
        detail,
        transaction_failure: Some(Box::new(source)),
        replay: history.to_replay_text(),
    }
}

fn render(history: &ApplicationHistory) -> String {
    let mut output = String::from("mako-application-history-v1\n");
    output.push_str("transaction-history-begin\n");
    output.push_str(&history.transactions.to_replay_text());
    output.push_str("transaction-history-end\n");

    let mut commits: Vec<_> = history.commits.iter().collect();
    commits.sort_by_key(|commit| commit.transaction);
    for commit in commits {
        writeln!(
            &mut output,
            "commit T{} {} {} {}",
            commit.transaction,
            commit.interval.invocation,
            render_response(commit.interval.response),
            render_commit_outcome(commit.outcome)
        )
        .expect("writing to String cannot fail");
    }
    let mut attempts: Vec<_> = history.backend_attempts.iter().collect();
    attempts.sort_by_key(|attempt| (attempt.interval.invocation, attempt.seq));
    for attempt in attempts {
        let mut mutations: Vec<_> = attempt.mutations.iter().collect();
        mutations.sort_by_key(|mutation| mutation.state_key());
        let mutations = mutations
            .into_iter()
            .map(render_mutation)
            .collect::<Vec<_>>()
            .join(",");
        writeln!(
            &mut output,
            "backend {} {} {} {:?} [{}]",
            attempt.seq,
            attempt.interval.invocation,
            render_response(attempt.interval.response),
            attempt.outcome,
            mutations
        )
        .expect("writing to String cannot fail");
    }
    let mut frontiers: Vec<_> = history.frontiers.iter().collect();
    frontiers.sort_by_key(|frontier| frontier.interval.invocation);
    for frontier in frontiers {
        writeln!(
            &mut output,
            "frontier {} {} acknowledged={} applied={} visible={} backend={}",
            frontier.interval.invocation,
            render_response(frontier.interval.response),
            render_seq(frontier.highest_acknowledged),
            render_seq(frontier.applied),
            render_state(frontier.visible_state.as_ref()),
            render_state(frontier.backend_state.as_ref())
        )
        .expect("writing to String cannot fail");
    }
    let mut waits: Vec<_> = history.waits.iter().collect();
    waits.sort_by_key(|wait| wait.interval.invocation);
    for wait in waits {
        writeln!(
            &mut output,
            "wait {} {} {:?}",
            wait.interval.invocation,
            render_response(wait.interval.response),
            wait.outcome
        )
        .expect("writing to String cannot fail");
    }
    output
}

fn render_response(response: Option<Tick>) -> String {
    response.map_or_else(|| "pending".to_owned(), |tick| tick.to_string())
}

fn render_commit_outcome(outcome: ApplicationCommitOutcome) -> String {
    match outcome {
        ApplicationCommitOutcome::AcknowledgedWrite { seq } => format!("ack-write:{seq}"),
        ApplicationCommitOutcome::AcknowledgedNoWrite => "ack-no-write".to_owned(),
        ApplicationCommitOutcome::Conflict => "conflict".to_owned(),
        ApplicationCommitOutcome::Aborted => "aborted".to_owned(),
        ApplicationCommitOutcome::UnknownPinned { seq } => format!("unknown-pinned:{seq}"),
        ApplicationCommitOutcome::CommittedPinned { seq, prior_unknown } => {
            format!("committed-pinned:{seq}:after:{prior_unknown}")
        }
    }
}

fn render_mutation(mutation: &ModelMutation) -> String {
    match mutation {
        ModelMutation::Put { table, key, value } => format!(
            "put:{table}:{}:{}",
            crate::replay::hex(key),
            crate::replay::hex(value)
        ),
        ModelMutation::Delete { table, key } => {
            format!("delete:{table}:{}", crate::replay::hex(key))
        }
    }
}

fn render_state(state: Option<&State>) -> String {
    state.map_or_else(
        || "unobserved".to_owned(),
        |state| {
            let entries = state
                .iter()
                .map(|(key, value)| {
                    format!(
                        "{}:{}={}",
                        key.table,
                        crate::replay::hex(&key.key),
                        crate::replay::hex(value)
                    )
                })
                .collect::<Vec<_>>()
                .join(",");
            format!("[{entries}]")
        },
    )
}
