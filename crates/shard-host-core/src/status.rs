//! What a storage verb answers for an engine result.
use vstd::prelude::*;

verus! {

// The numbers of `enum MakoShardStatus`. BUSY (4) is not listed: no verb
// answers it.
pub const SHARD_OK: u32 = 0;
pub const SHARD_NOT_FOUND: u32 = 1;
pub const SHARD_RETRY: u32 = 2;
pub const SHARD_INVALID: u32 = 3;
pub const SHARD_EXHAUSTED: u32 = 5;
pub const SHARD_IO: u32 = 6;

// The engine statuses the mapping tells apart, as the protocol exports
// return them. The shell fails to compile if one of them is not the number
// of the engine's own `Status`.
pub const ENGINE_OK: i32 = 0;
pub const ENGINE_RETRY: i32 = 3;
pub const ENGINE_FATAL: i32 = 5;
pub const ENGINE_EXHAUSTED: i32 = 6;
pub const ENGINE_UNKNOWN: i32 = 7;
pub const ENGINE_COMMITTED_ERROR: i32 = 8;

// `StoTpccProtocolReport::disposition`: 0 none, 1 aborted, 2 committed,
// 3 uncertain.
pub const ABORTED: u32 = 1;
pub const COMMITTED: u32 = 2;

/// What a verb answers for an engine result.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Mapped {
    Shard(u32),
    /// The outcome is not definite. The process must stop.
    FailStop,
}

/// Where an engine status came from.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Stage {
    /// A step failed and the closing `finish(.., 0, ..)` has returned OK.
    Closed,
    /// The result of `sto_tpcc_protocol_commit`.
    Commit,
    /// The result of the committing `finish(.., 1, ..)` of a read-only attempt.
    ReadOnlyFinish,
}

/// The engine statuses that do not say whether the effect happened.
pub open spec fn indefinite(status: i32) -> bool {
    status == ENGINE_UNKNOWN || status == ENGINE_COMMITTED_ERROR
}

/// The step succeeded and its report says committed.
pub open spec fn succeeded(stage: Stage, status: i32, disposition: u32) -> bool {
    stage != Stage::Closed && status == ENGINE_OK && disposition == COMMITTED
}

/// A commit that failed with a report that says aborted: nothing was
/// installed.
pub open spec fn commit_refused(status: i32, disposition: u32) -> bool {
    disposition == ABORTED && (status == ENGINE_RETRY || status == ENGINE_EXHAUSTED || status
        == ENGINE_FATAL)
}

/// What the mapping promises. It names every answer, so it leaves exactly one
/// (`contract_names_one_answer`).
pub open spec fn mapping_contract(stage: Stage, status: i32, disposition: u32, mapped: Mapped) -> bool {
    // OK is answered exactly for a step that succeeded.
    &&& mapped == Mapped::Shard(SHARD_OK) <==> succeeded(stage, status, disposition)
    // A commit answers only when its outcome is definite: committed, or one
    // of three failures with an aborted attempt. Everything else stops.
    &&& stage == Stage::Commit ==> (mapped == Mapped::FailStop <==> !(succeeded(
        stage,
        status,
        disposition,
    ) || commit_refused(status, disposition)))
    // Elsewhere the mapping assumes that nothing can have been installed: the
    // attempt was closed as aborted, or it staged no write. Only the two
    // statuses that say otherwise themselves stop the process; the report's
    // disposition is not looked at, so an uncertain one (3) is answered as
    // the status's failure.
    &&& stage != Stage::Commit ==> (mapped == Mapped::FailStop <==> indefinite(status))
    // A failure keeps its name: RETRY and EXHAUSTED are told apart, and every
    // other failure is IO.
    &&& mapped == Mapped::Shard(SHARD_RETRY) <==> mapped != Mapped::FailStop && status
        == ENGINE_RETRY
    &&& mapped == Mapped::Shard(SHARD_EXHAUSTED) <==> mapped != Mapped::FailStop && status
        == ENGINE_EXHAUSTED
    // No engine result is answered NOT_FOUND, INVALID or BUSY.
    &&& match mapped {
        Mapped::Shard(answer) => answer == SHARD_OK || answer == SHARD_RETRY || answer
            == SHARD_EXHAUSTED || answer == SHARD_IO,
        Mapped::FailStop => true,
    }
}

/// Maps an engine status and the disposition of its report (0 none, 1
/// aborted, 2 committed, 3 uncertain) to what the verb answers.
///
/// `Shard(SHARD_OK)` means "this step succeeded": the caller answers OK after
/// `Commit` and goes on with the row or the next lookahead after
/// `ReadOnlyFinish`. A status that reaches `Closed` belongs to a step that
/// failed, so OK there is a caller's bug and is never success.
pub const fn map_engine(stage: Stage, status: i32, disposition: u32) -> (mapped: Mapped)
    ensures
        mapping_contract(stage, status, disposition, mapped),
{
    match stage {
        Stage::Closed => map_failure(status),
        Stage::Commit => match (status, disposition) {
            (ENGINE_OK, COMMITTED) => Mapped::Shard(SHARD_OK),
            (ENGINE_RETRY | ENGINE_EXHAUSTED | ENGINE_FATAL, ABORTED) => map_failure(status),
            // Anything else may or may not have installed the write.
            _ => Mapped::FailStop,
        },
        Stage::ReadOnlyFinish => match (status, disposition) {
            (ENGINE_OK, COMMITTED) => Mapped::Shard(SHARD_OK),
            (ENGINE_OK, _) => Mapped::Shard(SHARD_IO),
            _ => map_failure(status),
        },
    }
}

/// The answer for a failed step whose attempt is known to be aborted.
const fn map_failure(status: i32) -> (mapped: Mapped)
    ensures
        mapped == Mapped::FailStop <==> indefinite(status),
        mapped == Mapped::Shard(SHARD_RETRY) <==> status == ENGINE_RETRY,
        mapped == Mapped::Shard(SHARD_EXHAUSTED) <==> status == ENGINE_EXHAUSTED,
        mapped == Mapped::Shard(SHARD_IO) <==> !indefinite(status) && status != ENGINE_RETRY
            && status != ENGINE_EXHAUSTED,
{
    match status {
        ENGINE_RETRY => Mapped::Shard(SHARD_RETRY),
        ENGINE_EXHAUSTED => Mapped::Shard(SHARD_EXHAUSTED),
        ENGINE_UNKNOWN | ENGINE_COMMITTED_ERROR => Mapped::FailStop,
        _ => Mapped::Shard(SHARD_IO),
    }
}

/// The contract leaves the mapping no choice: two answers that both satisfy
/// it are the same answer.
pub proof fn contract_names_one_answer(
    stage: Stage,
    status: i32,
    disposition: u32,
    first: Mapped,
    second: Mapped,
)
    requires
        mapping_contract(stage, status, disposition, first),
        mapping_contract(stage, status, disposition, second),
    ensures
        first == second,
{
}

/// An engine status that does not say whether the effect happened is never
/// turned into a status the sharding crate could act on.
pub proof fn indefinite_status_stops(stage: Stage, status: i32, disposition: u32, mapped: Mapped)
    requires
        mapping_contract(stage, status, disposition, mapped),
        indefinite(status),
    ensures
        mapped == Mapped::FailStop,
{
}

/// A commit is answered OK only when it is known committed and with an error
/// only when it is known aborted. A report that says neither stops the
/// process.
pub proof fn commit_answers_only_definite_outcomes(
    status: i32,
    disposition: u32,
    mapped: Mapped,
)
    requires
        mapping_contract(Stage::Commit, status, disposition, mapped),
    ensures
        mapped == Mapped::Shard(SHARD_OK) ==> status == ENGINE_OK && disposition == COMMITTED,
        mapped != Mapped::FailStop && mapped != Mapped::Shard(SHARD_OK) ==> disposition
            == ABORTED,
        disposition != ABORTED && disposition != COMMITTED ==> mapped == Mapped::FailStop,
{
}

} // verus!
