//! An independent, binary-safe transaction-history correctness oracle.
//!
//! The checker deliberately knows nothing about Silo versions, Mako logical
//! timestamps, commit response order, or any native implementation detail. It
//! searches legal serial executions of the operations a caller observed and
//! constrains them only by transaction real-time precedence.

mod checker;
mod replay;

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};

pub use checker::{
    check, check_opacity, check_strict_serializability, CheckFailure, CheckFailureKind,
    CheckOptions, CheckWitness, Semantics,
};

/// Stable identifier assigned to one recorded transaction.
pub type TxnId = u32;

/// Deterministic logical event time.
pub type Tick = u64;

/// Stable identifier for one table in the independent model.
pub type TableId = u64;

/// One binary key in one table.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct StateKey {
    /// Table containing the key.
    pub table: TableId,
    /// Exact key bytes.
    pub key: Vec<u8>,
}

impl StateKey {
    /// Construct a binary table/key pair.
    pub fn new(table: TableId, key: impl Into<Vec<u8>>) -> Self {
        Self {
            table,
            key: key.into(),
        }
    }
}

/// Complete table state used by the independent model.
pub type State = BTreeMap<StateKey, Vec<u8>>;

/// Insert one table/key/value into a model state.
pub fn state_insert(
    state: &mut State,
    table: TableId,
    key: impl Into<Vec<u8>>,
    value: impl Into<Vec<u8>>,
) -> Option<Vec<u8>> {
    state.insert(StateKey::new(table, key), value.into())
}

/// Invocation/response interval for one synchronous API call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Interval {
    /// Tick immediately before invoking the operation.
    pub invocation: Tick,
    /// Tick immediately after a response, or `None` while the call is pending.
    pub response: Option<Tick>,
}

impl Interval {
    /// Construct a completed interval.
    pub const fn completed(invocation: Tick, response: Tick) -> Self {
        Self {
            invocation,
            response: Some(response),
        }
    }

    /// Construct an invoked call whose response has not been observed.
    pub const fn pending(invocation: Tick) -> Self {
        Self {
            invocation,
            response: None,
        }
    }
}

/// Direction of a transactional range scan.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScanDirection {
    /// Ascending binary-key order.
    Forward,
    /// Descending binary-key order.
    Reverse,
}

/// One exact key/value row returned by a scan.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Row {
    /// Exact key bytes.
    pub key: Vec<u8>,
    /// Exact value bytes.
    pub value: Vec<u8>,
}

impl Row {
    /// Construct a binary scan row.
    pub fn new(key: impl Into<Vec<u8>>, value: impl Into<Vec<u8>>) -> Self {
        Self {
            key: key.into(),
            value: value.into(),
        }
    }
}

/// Operation requested by a transaction.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Operation {
    /// Read one key.
    Get {
        /// Target table.
        table: TableId,
        /// Exact key bytes.
        key: Vec<u8>,
    },
    /// Upsert one key.
    Put {
        /// Target table.
        table: TableId,
        /// Exact key bytes.
        key: Vec<u8>,
        /// Exact staged value bytes.
        value: Vec<u8>,
    },
    /// Insert one key only when absent.
    Insert {
        /// Target table.
        table: TableId,
        /// Exact key bytes.
        key: Vec<u8>,
        /// Exact staged value bytes.
        value: Vec<u8>,
    },
    /// Remove one key.
    Remove {
        /// Target table.
        table: TableId,
        /// Exact key bytes.
        key: Vec<u8>,
    },
    /// Scan the binary range `[lower, upper)`.
    Scan {
        /// Target table.
        table: TableId,
        /// Inclusive lower bound.
        lower: Vec<u8>,
        /// Exclusive upper bound, or no upper bound.
        upper: Option<Vec<u8>>,
        /// Requested result direction.
        direction: ScanDirection,
    },
}

impl Operation {
    /// Construct a point read.
    pub fn get(table: TableId, key: impl Into<Vec<u8>>) -> Self {
        Self::Get {
            table,
            key: key.into(),
        }
    }

    /// Construct an upsert.
    pub fn put(table: TableId, key: impl Into<Vec<u8>>, value: impl Into<Vec<u8>>) -> Self {
        Self::Put {
            table,
            key: key.into(),
            value: value.into(),
        }
    }

    /// Construct an insert-if-absent operation.
    pub fn insert(table: TableId, key: impl Into<Vec<u8>>, value: impl Into<Vec<u8>>) -> Self {
        Self::Insert {
            table,
            key: key.into(),
            value: value.into(),
        }
    }

    /// Construct a remove operation.
    pub fn remove(table: TableId, key: impl Into<Vec<u8>>) -> Self {
        Self::Remove {
            table,
            key: key.into(),
        }
    }

    /// Construct a range scan.
    pub fn scan(
        table: TableId,
        lower: impl Into<Vec<u8>>,
        upper: Option<Vec<u8>>,
        direction: ScanDirection,
    ) -> Self {
        Self::Scan {
            table,
            lower: lower.into(),
            upper,
            direction,
        }
    }
}

/// Value returned by a completed operation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Observation {
    /// Result of [`Operation::Get`].
    Get(Option<Vec<u8>>),
    /// Result of [`Operation::Put`].
    Put {
        /// Whether the key was absent immediately before this operation.
        created: bool,
    },
    /// Result of [`Operation::Insert`].
    Insert {
        /// Whether the value was staged because the key was absent.
        inserted: bool,
    },
    /// Result of [`Operation::Remove`].
    Remove {
        /// Whether a live value existed immediately before removal.
        existed: bool,
    },
    /// Exact result of [`Operation::Scan`].
    Scan(Vec<Row>),
    /// OCC rejected the operation and aborted the transaction.
    Conflict,
}

/// One operation and its observed call interval.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TimedOperation {
    /// Invocation/response interval.
    pub interval: Interval,
    /// Requested operation.
    pub operation: Operation,
    /// Returned observation, or `None` while the call is pending.
    pub observation: Option<Observation>,
}

impl TimedOperation {
    /// Construct a completed operation.
    pub fn completed(
        invocation: Tick,
        response: Tick,
        operation: Operation,
        observation: Observation,
    ) -> Self {
        Self {
            interval: Interval::completed(invocation, response),
            operation,
            observation: Some(observation),
        }
    }

    /// Construct an invoked operation without a response.
    pub fn pending(invocation: Tick, operation: Operation) -> Self {
        Self {
            interval: Interval::pending(invocation),
            operation,
            observation: None,
        }
    }
}

/// Kind of terminal transaction call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TerminalKind {
    /// Validate and commit.
    Commit,
    /// Explicitly abort.
    Abort,
}

/// Observed terminal disposition.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TerminalOutcome {
    /// Writes became visible.
    Committed,
    /// The caller explicitly or voluntarily aborted.
    Aborted,
    /// OCC rejected the commit.
    Conflict,
}

/// Terminal transaction call and its outcome.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TerminalCall {
    /// Commit or abort.
    pub kind: TerminalKind,
    /// Invocation/response interval.
    pub interval: Interval,
    /// Returned outcome, or `None` while the call is pending.
    pub outcome: Option<TerminalOutcome>,
}

impl TerminalCall {
    /// Construct a completed commit call.
    pub const fn commit(invocation: Tick, response: Tick, outcome: TerminalOutcome) -> Self {
        Self {
            kind: TerminalKind::Commit,
            interval: Interval::completed(invocation, response),
            outcome: Some(outcome),
        }
    }

    /// Construct a commit invocation without a response.
    pub const fn pending_commit(invocation: Tick) -> Self {
        Self {
            kind: TerminalKind::Commit,
            interval: Interval::pending(invocation),
            outcome: None,
        }
    }

    /// Construct a completed explicit abort.
    pub const fn abort(invocation: Tick, response: Tick) -> Self {
        Self {
            kind: TerminalKind::Abort,
            interval: Interval::completed(invocation, response),
            outcome: Some(TerminalOutcome::Aborted),
        }
    }

    /// Construct an abort invocation without a response.
    pub const fn pending_abort(invocation: Tick) -> Self {
        Self {
            kind: TerminalKind::Abort,
            interval: Interval::pending(invocation),
            outcome: None,
        }
    }
}

/// Complete recorded information for one transaction.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Transaction {
    /// Stable transaction identifier.
    pub id: TxnId,
    /// Begin invocation/response interval.
    pub begin: Interval,
    /// Completed or pending operations in program order.
    pub operations: Vec<TimedOperation>,
    /// Terminal call, or `None` while the transaction remains active.
    pub terminal: Option<TerminalCall>,
}

impl Transaction {
    /// Construct a transaction with no operations or terminal call.
    pub fn new(id: TxnId, begin: Interval) -> Self {
        Self {
            id,
            begin,
            operations: Vec::new(),
            terminal: None,
        }
    }

    /// Append a recorded operation.
    pub fn push(&mut self, operation: TimedOperation) -> &mut Self {
        self.operations.push(operation);
        self
    }

    /// Set the terminal call.
    pub fn finish(&mut self, terminal: TerminalCall) -> &mut Self {
        self.terminal = Some(terminal);
        self
    }
}

/// A complete history with independent initial and observed final states.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct History {
    /// State before the first recorded transaction.
    pub initial_state: State,
    /// Independently observed final state, when available.
    pub observed_final_state: Option<State>,
    /// Recorded transactions.
    pub transactions: Vec<Transaction>,
}

impl History {
    /// Construct an empty history over `initial_state`.
    pub fn new(initial_state: State) -> Self {
        Self {
            initial_state,
            observed_final_state: None,
            transactions: Vec::new(),
        }
    }

    /// Set the independently observed final state.
    pub fn set_observed_final_state(&mut self, state: State) -> &mut Self {
        self.observed_final_state = Some(state);
        self
    }

    /// Append one transaction.
    pub fn push(&mut self, transaction: Transaction) -> &mut Self {
        self.transactions.push(transaction);
        self
    }

    /// Render a stable, binary-safe diagnostic transcript.
    pub fn to_replay_text(&self) -> String {
        replay::render(self)
    }
}

/// Thread-safe source of strictly increasing logical ticks.
///
/// Recording adapters should call [`Self::next`] immediately before invoking
/// and immediately after receiving each operation. The total tick order is
/// diagnostic only; the checker derives transaction precedence from response
/// before begin-invocation, never from commit response order itself.
#[derive(Debug)]
pub struct LogicalClock {
    next: AtomicU64,
}

impl LogicalClock {
    /// Construct a clock whose first returned tick is `first`.
    pub const fn new(first: Tick) -> Self {
        Self {
            next: AtomicU64::new(first),
        }
    }

    /// Return the next unique logical tick.
    pub fn next(&self) -> Tick {
        self.next.fetch_add(1, Ordering::SeqCst)
    }
}

impl Default for LogicalClock {
    fn default() -> Self {
        Self::new(1)
    }
}
