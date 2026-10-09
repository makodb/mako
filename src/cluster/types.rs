//! Concrete protocol identities. Byte coordinates retain the legacy lexicographic
//! order; an absent upper bound means infinity, not an empty interval.
use vstd::prelude::*;

verus! {

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct Grant {
    pub owner: u32,
    pub epoch: u64,
}

/// One sequential transaction stream per originating worker. Sequence numbers
/// never wrap; a retired sequence cannot reopen an old participant session.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct TxnId {
    pub client: u64,
    pub sequence: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Status {
    Ok = 0,
    NotFound = 1,
    Retry = 2,
    Invalid = 3,
    Busy = 4,
    Exhausted = 5,
    Io = 6,
}

pub struct KeyRange {
    pub table: u64,
    pub lo: Vec<u8>,
    pub hi: Option<Vec<u8>>,
}

pub struct Boundary {
    pub start: Vec<u8>,
    pub grant: Grant,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Role { Empty, Serving, Frozen, Retired, Stage, Ready }

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Phase { Copy, Freezing, Final, Retiring, Committed, Aborted }

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Command { Start, Freeze, Final, Retire, Commit, Abort }

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Certificate { Drained, Ready, Retired, SourceDone, DestinationDone }

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Outcome {
    pub generation: u64,
    pub committed: bool,
}


#[derive(Clone, Copy, PartialEq, Eq)]
pub struct ReplicaMeta {
    pub epoch: u64,
    pub fence: u64,
    pub terminal: bool,
    pub role: Role,
    pub round: u64,
}

/// The coordinator's immutable pre-handoff table snapshot accompanies controls.
/// Participants check their own metadata against it, never the master's phase.
pub struct MigrationPlan {
    pub generation: u64,
    pub nonce: TxnId,
    pub source: u32,
    pub destination: u32,
    pub range: KeyRange,
    pub old: Vec<Boundary>,
}
/// Canonical addressing is independent of a node's physical table-id window.
/// A warehouse alias gives every row in that warehouse the same coordinate;
/// ordinary tables use their raw row key as their coordinate.
pub struct Row {
    pub coordinate: Vec<u8>,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}

} // verus!
