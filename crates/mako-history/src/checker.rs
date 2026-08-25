use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fmt;

use crate::{
    History, Observation, Operation, Row, ScanDirection, State, StateKey, TerminalKind,
    TerminalOutcome, Tick, TimedOperation, Transaction, TxnId,
};

/// Correctness property requested from the history checker.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Semantics {
    /// Check the committed projection for a strict-serializable execution.
    StrictSerializability,
    /// Check every relevant prefix including aborted and live observations.
    Opacity,
}

/// Deterministic resource bounds for one checker invocation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CheckOptions {
    /// Maximum number of DFS states across the complete check.
    pub max_search_nodes: usize,
}

impl Default for CheckOptions {
    fn default() -> Self {
        Self {
            max_search_nodes: 1_000_000,
        }
    }
}

/// A legal serial execution found by the checker.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CheckWitness {
    /// Property that was checked.
    pub semantics: Semantics,
    /// Legal order for the full history or final opacity prefix.
    pub serialization: Vec<TxnId>,
    /// Pending commit calls completed as commits in that final-prefix witness.
    pub pending_commits_applied: Vec<TxnId>,
    /// Number of opacity prefixes checked (one for strict serializability).
    pub prefixes_checked: usize,
    /// Total DFS nodes visited.
    pub explored_nodes: usize,
}

/// Broad reason a history check failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CheckFailureKind {
    /// Invocation/response records are not a well-formed transaction history.
    MalformedHistory,
    /// Exhaustive bounded search found no legal serialization.
    NoLegalSerialization,
    /// The configured search budget was exhausted, so the result is inconclusive.
    SearchBudgetExceeded,
}

/// Diagnostic returned for a malformed or nonserializable history.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CheckFailure {
    /// Broad failure class.
    pub kind: CheckFailureKind,
    /// Property being checked.
    pub semantics: Semantics,
    /// Opacity prefix tick, or `None` for a whole-history failure.
    pub prefix_tick: Option<Tick>,
    /// Number of DFS nodes visited before failure.
    pub explored_nodes: usize,
    /// Deepest partial serial order reached by deterministic search.
    pub deepest_serialization: Vec<TxnId>,
    /// First useful deterministic explanation at the deepest search point.
    pub detail: String,
    /// Binary-safe replay transcript for failure output and CI artifacts.
    pub replay: String,
}

impl fmt::Display for CheckFailure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?} {:?}", self.semantics, self.kind)?;
        if let Some(prefix) = self.prefix_tick {
            write!(f, " at prefix tick {prefix}")?;
        }
        write!(
            f,
            ": {}; explored {} nodes; deepest order {:?}\n{}",
            self.detail, self.explored_nodes, self.deepest_serialization, self.replay
        )
    }
}

impl std::error::Error for CheckFailure {}

/// Check `history` using the requested correctness property.
pub fn check(
    history: &History,
    semantics: Semantics,
    options: CheckOptions,
) -> Result<CheckWitness, CheckFailure> {
    if let Err(detail) = validate(history) {
        return Err(failure(
            history,
            CheckFailureKind::MalformedHistory,
            semantics,
            None,
            0,
            Vec::new(),
            detail,
        ));
    }
    match semantics {
        Semantics::StrictSerializability => check_strict_validated(history, options),
        Semantics::Opacity => check_opacity_validated(history, options),
    }
}

/// Check the committed projection for strict serializability.
pub fn check_strict_serializability(
    history: &History,
    options: CheckOptions,
) -> Result<CheckWitness, CheckFailure> {
    check(history, Semantics::StrictSerializability, options)
}

/// Check every response-bearing/final prefix for opacity.
pub fn check_opacity(
    history: &History,
    options: CheckOptions,
) -> Result<CheckWitness, CheckFailure> {
    check(history, Semantics::Opacity, options)
}

fn failure(
    history: &History,
    kind: CheckFailureKind,
    semantics: Semantics,
    prefix_tick: Option<Tick>,
    explored_nodes: usize,
    deepest_serialization: Vec<TxnId>,
    detail: String,
) -> CheckFailure {
    CheckFailure {
        kind,
        semantics,
        prefix_tick,
        explored_nodes,
        deepest_serialization,
        detail,
        replay: history.to_replay_text(),
    }
}

fn validate(history: &History) -> Result<(), String> {
    let mut ids = BTreeSet::new();
    let mut ticks: BTreeMap<Tick, String> = BTreeMap::new();

    for transaction in &history.transactions {
        if !ids.insert(transaction.id) {
            return Err(format!("duplicate transaction id T{}", transaction.id));
        }
        record_tick(
            &mut ticks,
            transaction.begin.invocation,
            format!("T{} begin invocation", transaction.id),
        )?;
        validate_interval(transaction.id, "begin", transaction.begin, &mut ticks)?;

        let Some(mut previous_response) = transaction.begin.response else {
            if !transaction.operations.is_empty() || transaction.terminal.is_some() {
                return Err(format!(
                    "T{} has operations or a terminal call while begin is pending",
                    transaction.id
                ));
            }
            continue;
        };

        let mut pending_operation = false;
        let mut operation_conflict = false;
        for (index, operation) in transaction.operations.iter().enumerate() {
            if pending_operation {
                return Err(format!(
                    "T{} operation {} follows a pending operation",
                    transaction.id, index
                ));
            }
            if operation_conflict {
                return Err(format!(
                    "T{} operation {} follows a conflict response",
                    transaction.id, index
                ));
            }
            if operation.interval.invocation <= previous_response {
                return Err(format!(
                    "T{} operation {} begins at {} before the prior response at {}",
                    transaction.id, index, operation.interval.invocation, previous_response
                ));
            }
            record_tick(
                &mut ticks,
                operation.interval.invocation,
                format!("T{} operation {} invocation", transaction.id, index),
            )?;
            validate_interval(
                transaction.id,
                &format!("operation {index}"),
                operation.interval,
                &mut ticks,
            )?;
            match (operation.interval.response, operation.observation.as_ref()) {
                (Some(response), Some(observation)) => {
                    validate_observation(transaction.id, index, &operation.operation, observation)?;
                    previous_response = response;
                    operation_conflict = matches!(observation, Observation::Conflict);
                }
                (None, None) => pending_operation = true,
                (Some(_), None) => {
                    return Err(format!(
                        "T{} operation {} has a response but no observation",
                        transaction.id, index
                    ));
                }
                (None, Some(_)) => {
                    return Err(format!(
                        "T{} operation {} has an observation but no response",
                        transaction.id, index
                    ));
                }
            }
        }

        if operation_conflict && transaction.terminal.is_some() {
            return Err(format!(
                "T{} has a terminal call after an operation conflict",
                transaction.id
            ));
        }

        if let Some(terminal) = &transaction.terminal {
            if pending_operation {
                return Err(format!(
                    "T{} has a terminal call while an operation is pending",
                    transaction.id
                ));
            }
            if terminal.interval.invocation <= previous_response {
                return Err(format!(
                    "T{} terminal call begins at {} before the prior response at {}",
                    transaction.id, terminal.interval.invocation, previous_response
                ));
            }
            record_tick(
                &mut ticks,
                terminal.interval.invocation,
                format!("T{} terminal invocation", transaction.id),
            )?;
            validate_interval(transaction.id, "terminal", terminal.interval, &mut ticks)?;
            match (terminal.interval.response, terminal.outcome) {
                (Some(_), Some(outcome)) => match terminal.kind {
                    TerminalKind::Commit => match outcome {
                        TerminalOutcome::Committed
                        | TerminalOutcome::Aborted
                        | TerminalOutcome::Conflict => {}
                    },
                    TerminalKind::Abort if outcome == TerminalOutcome::Aborted => {}
                    TerminalKind::Abort => {
                        return Err(format!(
                            "T{} explicit abort returned {:?}",
                            transaction.id, outcome
                        ));
                    }
                },
                (None, None) => {}
                (Some(_), None) => {
                    return Err(format!(
                        "T{} terminal call has a response but no outcome",
                        transaction.id
                    ));
                }
                (None, Some(_)) => {
                    return Err(format!(
                        "T{} terminal call has an outcome but no response",
                        transaction.id
                    ));
                }
            }
        }
    }
    Ok(())
}

fn record_tick(
    ticks: &mut BTreeMap<Tick, String>,
    tick: Tick,
    label: String,
) -> Result<(), String> {
    if let Some(previous) = ticks.insert(tick, label.clone()) {
        return Err(format!(
            "logical tick {tick} is shared by {previous} and {label}"
        ));
    }
    Ok(())
}

fn validate_interval(
    transaction: TxnId,
    label: &str,
    interval: crate::Interval,
    ticks: &mut BTreeMap<Tick, String>,
) -> Result<(), String> {
    if let Some(response) = interval.response {
        if response <= interval.invocation {
            return Err(format!(
                "T{transaction} {label} responds at {response} no later than invocation at {}",
                interval.invocation
            ));
        }
        record_tick(ticks, response, format!("T{transaction} {label} response"))?;
    }
    Ok(())
}

fn validate_observation(
    transaction: TxnId,
    index: usize,
    operation: &Operation,
    observation: &Observation,
) -> Result<(), String> {
    let matches = matches!(observation, Observation::Conflict)
        || matches!(
            (operation, observation),
            (Operation::Get { .. }, Observation::Get(_))
                | (Operation::Put { .. }, Observation::Put { .. })
                | (Operation::Insert { .. }, Observation::Insert { .. })
                | (Operation::Remove { .. }, Observation::Remove { .. })
                | (Operation::Scan { .. }, Observation::Scan(_))
        );
    if matches {
        Ok(())
    } else {
        Err(format!(
            "T{transaction} operation {index} {:?} has incompatible observation {:?}",
            operation, observation
        ))
    }
}

#[derive(Clone, Debug)]
struct Candidate<'a> {
    id: TxnId,
    start: Tick,
    end: Option<Tick>,
    operations: Vec<&'a TimedOperation>,
    apply_choices: Vec<bool>,
    pending_commit: bool,
}

fn check_strict_validated(
    history: &History,
    options: CheckOptions,
) -> Result<CheckWitness, CheckFailure> {
    let mut candidates: Vec<_> = history
        .transactions
        .iter()
        .filter_map(strict_candidate)
        .collect();
    candidates.sort_by_key(|candidate| candidate.id);
    let search = search(
        &candidates,
        &history.initial_state,
        history.observed_final_state.as_ref(),
        options.max_search_nodes,
    );
    search_result(
        history,
        Semantics::StrictSerializability,
        None,
        1,
        0,
        search,
    )
}

fn strict_candidate(transaction: &Transaction) -> Option<Candidate<'_>> {
    let terminal = transaction.terminal.as_ref()?;
    if terminal.kind != TerminalKind::Commit || terminal.outcome != Some(TerminalOutcome::Committed)
    {
        return None;
    }
    Some(Candidate {
        id: transaction.id,
        start: transaction.begin.invocation,
        end: terminal.interval.response,
        operations: transaction.operations.iter().collect(),
        apply_choices: vec![true],
        pending_commit: false,
    })
}

fn check_opacity_validated(
    history: &History,
    options: CheckOptions,
) -> Result<CheckWitness, CheckFailure> {
    let prefixes = relevant_prefixes(history);
    let final_tick = *prefixes.last().expect("at least one opacity prefix");
    let mut total_explored = 0usize;
    let mut final_order = Vec::new();
    let mut final_pending = Vec::new();

    for prefix in prefixes.iter().copied() {
        let mut candidates: Vec<_> = history
            .transactions
            .iter()
            .filter_map(|transaction| opacity_candidate(transaction, prefix))
            .collect();
        candidates.sort_by_key(|candidate| candidate.id);
        let remaining = options.max_search_nodes.saturating_sub(total_explored);
        let expected_final = (prefix == final_tick)
            .then_some(history.observed_final_state.as_ref())
            .flatten();
        let search = search(
            &candidates,
            &history.initial_state,
            expected_final,
            remaining,
        );
        match search {
            SearchResult::Found {
                order,
                pending_commits_applied,
                explored,
            } => {
                total_explored = total_explored.saturating_add(explored);
                final_order = order;
                final_pending = pending_commits_applied;
            }
            other => {
                return search_result(
                    history,
                    Semantics::Opacity,
                    Some(prefix),
                    prefixes.len(),
                    total_explored,
                    other,
                );
            }
        }
    }

    Ok(CheckWitness {
        semantics: Semantics::Opacity,
        serialization: final_order,
        pending_commits_applied: final_pending,
        prefixes_checked: prefixes.len(),
        explored_nodes: total_explored,
    })
}

fn relevant_prefixes(history: &History) -> Vec<Tick> {
    let mut prefixes = BTreeSet::new();
    let mut final_tick = None;
    for transaction in &history.transactions {
        add_interval_ticks(transaction.begin, &mut prefixes, &mut final_tick);
        for operation in &transaction.operations {
            add_interval_ticks(operation.interval, &mut prefixes, &mut final_tick);
        }
        if let Some(terminal) = &transaction.terminal {
            add_interval_ticks(terminal.interval, &mut prefixes, &mut final_tick);
        }
    }
    if let Some(final_tick) = final_tick {
        prefixes.insert(final_tick);
    }
    if prefixes.is_empty() {
        prefixes.insert(0);
    }
    prefixes.into_iter().collect()
}

fn add_interval_ticks(
    interval: crate::Interval,
    responses: &mut BTreeSet<Tick>,
    final_tick: &mut Option<Tick>,
) {
    *final_tick =
        Some(final_tick.map_or(interval.invocation, |tick| tick.max(interval.invocation)));
    if let Some(response) = interval.response {
        responses.insert(response);
        *final_tick = Some(final_tick.map_or(response, |tick| tick.max(response)));
    }
}

fn opacity_candidate(transaction: &Transaction, prefix: Tick) -> Option<Candidate<'_>> {
    if transaction.begin.invocation > prefix {
        return None;
    }

    let operations: Vec<_> = transaction
        .operations
        .iter()
        .filter(|operation| {
            operation
                .interval
                .response
                .is_some_and(|tick| tick <= prefix)
        })
        .collect();
    if let Some(conflict) = operations
        .iter()
        .find(|operation| matches!(operation.observation, Some(Observation::Conflict)))
    {
        return Some(Candidate {
            id: transaction.id,
            start: transaction.begin.invocation,
            end: conflict.interval.response,
            operations,
            apply_choices: vec![false],
            pending_commit: false,
        });
    }

    let mut end = None;
    let mut apply_choices = vec![false];
    let mut pending_commit = false;
    if let Some(terminal) = transaction
        .terminal
        .as_ref()
        .filter(|terminal| terminal.interval.invocation <= prefix)
    {
        match terminal
            .interval
            .response
            .filter(|response| *response <= prefix)
        {
            Some(response) => {
                end = Some(response);
                if terminal.outcome == Some(TerminalOutcome::Committed) {
                    apply_choices = vec![true];
                }
            }
            None if terminal.kind == TerminalKind::Commit => {
                apply_choices = vec![false, true];
                pending_commit = true;
            }
            None => {}
        }
    }

    Some(Candidate {
        id: transaction.id,
        start: transaction.begin.invocation,
        end,
        operations,
        apply_choices,
        pending_commit,
    })
}

fn search_result(
    history: &History,
    semantics: Semantics,
    prefix_tick: Option<Tick>,
    prefixes_checked: usize,
    previously_explored: usize,
    result: SearchResult,
) -> Result<CheckWitness, CheckFailure> {
    match result {
        SearchResult::Found {
            order,
            pending_commits_applied,
            explored,
        } => Ok(CheckWitness {
            semantics,
            serialization: order,
            pending_commits_applied,
            prefixes_checked,
            explored_nodes: previously_explored.saturating_add(explored),
        }),
        SearchResult::NoLegal {
            explored,
            deepest,
            detail,
        } => Err(failure(
            history,
            CheckFailureKind::NoLegalSerialization,
            semantics,
            prefix_tick,
            previously_explored.saturating_add(explored),
            deepest,
            detail,
        )),
        SearchResult::BudgetExceeded {
            explored,
            deepest,
            detail,
        } => Err(failure(
            history,
            CheckFailureKind::SearchBudgetExceeded,
            semantics,
            prefix_tick,
            previously_explored.saturating_add(explored),
            deepest,
            detail,
        )),
    }
}

#[derive(Debug)]
enum SearchResult {
    Found {
        order: Vec<TxnId>,
        pending_commits_applied: Vec<TxnId>,
        explored: usize,
    },
    NoLegal {
        explored: usize,
        deepest: Vec<TxnId>,
        detail: String,
    },
    BudgetExceeded {
        explored: usize,
        deepest: Vec<TxnId>,
        detail: String,
    },
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct MemoKey {
    scheduled: Vec<bool>,
    state: Vec<(StateKey, Vec<u8>)>,
}

#[derive(Debug)]
struct SearchContext<'a> {
    candidates: &'a [Candidate<'a>],
    predecessors: Vec<Vec<usize>>,
    expected_final: Option<&'a State>,
    budget: usize,
    explored: usize,
    memo: HashSet<MemoKey>,
    best_depth: usize,
    best_order: Vec<TxnId>,
    best_detail: String,
}

#[derive(Debug)]
enum DfsResult {
    Found {
        order: Vec<TxnId>,
        pending_commits_applied: Vec<TxnId>,
    },
    NoLegal,
    BudgetExceeded,
}

fn search(
    candidates: &[Candidate<'_>],
    initial: &State,
    expected_final: Option<&State>,
    budget: usize,
) -> SearchResult {
    let mut predecessors = vec![Vec::new(); candidates.len()];
    for (before_index, before) in candidates.iter().enumerate() {
        let Some(end) = before.end else {
            continue;
        };
        for (after_index, after) in candidates.iter().enumerate() {
            if before_index != after_index && end < after.start {
                predecessors[after_index].push(before_index);
            }
        }
    }
    let mut context = SearchContext {
        candidates,
        predecessors,
        expected_final,
        budget,
        explored: 0,
        memo: HashSet::new(),
        best_depth: 0,
        best_order: Vec::new(),
        best_detail: "no candidate serialization was explored".to_owned(),
    };
    let mut scheduled = vec![false; candidates.len()];
    let mut order = Vec::new();
    let mut pending_commits_applied = Vec::new();
    let outcome = dfs(
        &mut context,
        initial,
        &mut scheduled,
        &mut order,
        &mut pending_commits_applied,
    );
    match outcome {
        DfsResult::Found {
            order,
            pending_commits_applied,
        } => SearchResult::Found {
            order,
            pending_commits_applied,
            explored: context.explored,
        },
        DfsResult::NoLegal => SearchResult::NoLegal {
            explored: context.explored,
            deepest: context.best_order,
            detail: context.best_detail,
        },
        DfsResult::BudgetExceeded => SearchResult::BudgetExceeded {
            explored: context.explored,
            deepest: context.best_order,
            detail: format!(
                "search budget of {} nodes exhausted; {}",
                context.budget, context.best_detail
            ),
        },
    }
}

fn dfs(
    context: &mut SearchContext<'_>,
    state: &State,
    scheduled: &mut [bool],
    order: &mut Vec<TxnId>,
    pending_commits_applied: &mut Vec<TxnId>,
) -> DfsResult {
    if context.explored >= context.budget {
        return DfsResult::BudgetExceeded;
    }
    context.explored += 1;

    if scheduled.iter().all(|scheduled| *scheduled) {
        if let Some(expected) = context.expected_final {
            if state != expected {
                note_failure(context, order, format_state_difference(expected, state));
                return DfsResult::NoLegal;
            }
        }
        return DfsResult::Found {
            order: order.clone(),
            pending_commits_applied: pending_commits_applied.clone(),
        };
    }

    let memo_key = MemoKey {
        scheduled: scheduled.to_vec(),
        state: state
            .iter()
            .map(|(key, value)| (key.clone(), value.clone()))
            .collect(),
    };
    if context.memo.contains(&memo_key) {
        return DfsResult::NoLegal;
    }

    let mut eligible = false;
    for index in 0..context.candidates.len() {
        if scheduled[index]
            || !context.predecessors[index]
                .iter()
                .all(|predecessor| scheduled[*predecessor])
        {
            continue;
        }
        eligible = true;
        let candidate_id = context.candidates[index].id;
        let evaluated = match evaluate(&context.candidates[index], state) {
            Ok(evaluated) => evaluated,
            Err(detail) => {
                note_failure(
                    context,
                    order,
                    format!("candidate T{candidate_id} rejected: {detail}"),
                );
                continue;
            }
        };
        let choices = context.candidates[index].apply_choices.clone();
        let pending_commit = context.candidates[index].pending_commit;
        for apply in choices {
            scheduled[index] = true;
            order.push(candidate_id);
            if pending_commit && apply {
                pending_commits_applied.push(candidate_id);
            }
            let next_state = if apply { &evaluated } else { state };
            match dfs(
                context,
                next_state,
                scheduled,
                order,
                pending_commits_applied,
            ) {
                found @ DfsResult::Found { .. } => return found,
                DfsResult::BudgetExceeded => return DfsResult::BudgetExceeded,
                DfsResult::NoLegal => {}
            }
            if pending_commit && apply {
                pending_commits_applied.pop();
            }
            order.pop();
            scheduled[index] = false;
        }
    }

    if !eligible {
        note_failure(
            context,
            order,
            "no transaction satisfies the remaining real-time predecessors".to_owned(),
        );
    }
    context.memo.insert(memo_key);
    DfsResult::NoLegal
}

fn note_failure(context: &mut SearchContext<'_>, order: &[TxnId], detail: String) {
    if order.len() > context.best_depth
        || (order.len() == context.best_depth
            && context.best_detail == "no candidate serialization was explored")
    {
        context.best_depth = order.len();
        context.best_order = order.to_vec();
        context.best_detail = detail;
    }
}

fn evaluate(candidate: &Candidate<'_>, base: &State) -> Result<State, String> {
    let mut view = base.clone();
    for (index, timed) in candidate.operations.iter().enumerate() {
        let observation = timed
            .observation
            .as_ref()
            .expect("candidate operations are completed");
        if matches!(observation, Observation::Conflict) {
            break;
        }
        match (&timed.operation, observation) {
            (Operation::Get { table, key }, Observation::Get(observed)) => {
                let expected = view.get(&StateKey::new(*table, key.clone())).cloned();
                if &expected != observed {
                    return Err(format!(
                        "operation {index} GET table {table} key {} expected {}, observed {}",
                        hex(key),
                        optional_bytes(&expected),
                        optional_bytes(observed)
                    ));
                }
            }
            (Operation::Put { table, key, value }, Observation::Put { created }) => {
                let state_key = StateKey::new(*table, key.clone());
                let expected = !view.contains_key(&state_key);
                if *created != expected {
                    return Err(format!(
                        "operation {index} PUT table {table} key {} expected created={expected}, observed created={created}",
                        hex(key)
                    ));
                }
                view.insert(state_key, value.clone());
            }
            (Operation::Insert { table, key, value }, Observation::Insert { inserted }) => {
                let state_key = StateKey::new(*table, key.clone());
                let expected = !view.contains_key(&state_key);
                if *inserted != expected {
                    return Err(format!(
                        "operation {index} INSERT table {table} key {} expected inserted={expected}, observed inserted={inserted}",
                        hex(key)
                    ));
                }
                if expected {
                    view.insert(state_key, value.clone());
                }
            }
            (Operation::Remove { table, key }, Observation::Remove { existed }) => {
                let state_key = StateKey::new(*table, key.clone());
                let expected = view.contains_key(&state_key);
                if *existed != expected {
                    return Err(format!(
                        "operation {index} REMOVE table {table} key {} expected existed={expected}, observed existed={existed}",
                        hex(key)
                    ));
                }
                view.remove(&state_key);
            }
            (
                Operation::Scan {
                    table,
                    lower,
                    upper,
                    direction,
                },
                Observation::Scan(observed),
            ) => {
                let mut expected: Vec<Row> = view
                    .iter()
                    .filter(|(state_key, _)| {
                        state_key.table == *table
                            && state_key.key.as_slice() >= lower.as_slice()
                            && upper
                                .as_deref()
                                .is_none_or(|upper| state_key.key.as_slice() < upper)
                    })
                    .map(|(state_key, value)| Row::new(state_key.key.clone(), value.clone()))
                    .collect();
                if *direction == ScanDirection::Reverse {
                    expected.reverse();
                }
                if &expected != observed {
                    return Err(format!(
                        "operation {index} SCAN table {table} range [{}, {}) {:?} expected {}, observed {}",
                        hex(lower),
                        upper.as_deref().map(hex).unwrap_or_else(|| "unbounded".to_owned()),
                        direction,
                        rows(&expected),
                        rows(observed)
                    ));
                }
            }
            _ => unreachable!("history validation rejects mismatched observations"),
        }
    }
    Ok(view)
}

fn format_state_difference(expected: &State, actual: &State) -> String {
    for (key, expected_value) in expected {
        match actual.get(key) {
            Some(actual_value) if actual_value == expected_value => {}
            Some(actual_value) => {
                return format!(
                    "final state mismatch at table {} key {}: expected {}, model produced {}",
                    key.table,
                    hex(&key.key),
                    hex(expected_value),
                    hex(actual_value)
                );
            }
            None => {
                return format!(
                    "final state is missing table {} key {} with expected value {}",
                    key.table,
                    hex(&key.key),
                    hex(expected_value)
                );
            }
        }
    }
    for (key, actual_value) in actual {
        if !expected.contains_key(key) {
            return format!(
                "final state has unexpected table {} key {} value {}",
                key.table,
                hex(&key.key),
                hex(actual_value)
            );
        }
    }
    "final state differs".to_owned()
}

fn hex(bytes: &[u8]) -> String {
    crate::replay::hex(bytes)
}

fn optional_bytes(bytes: &Option<Vec<u8>>) -> String {
    bytes
        .as_deref()
        .map(hex)
        .unwrap_or_else(|| "missing".to_owned())
}

fn rows(rows: &[Row]) -> String {
    let body = rows
        .iter()
        .map(|row| format!("{}={}", hex(&row.key), hex(&row.value)))
        .collect::<Vec<_>>()
        .join(",");
    format!("[{body}]")
}
