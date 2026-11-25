//! Checkpoint and recovery policy over facts supplied by storage adapters.
//! The adapter establishes the row cut (including committed overlays), complete
//! protocol export and durable file/reference publication. These predicates do
//! not prove filesystem I/O, serialization or a native engine's snapshot copy.
use vstd::prelude::*;
verus! {

pub fn checkpoint_ready(
    applied: u64, row_cut: u64, metadata_cut: u64,
    required_prepares: u64, saved_prepares: u64, paused: bool,
) -> (ready: bool)
    ensures ready <==> paused && row_cut == applied && metadata_cut == applied
        && saved_prepares == required_prepares,
{
    paused && row_cut == applied && metadata_cut == applied
        && saved_prepares == required_prepares
}

pub fn may_trim(requested: u64, checkpoint: u64, files_validated: bool, files_published: bool) -> (allowed: bool)
    ensures allowed <==> files_validated && files_published && requested <= checkpoint,
{
    files_validated && files_published && requested <= checkpoint
}

/// A previous RAM/application watermark cannot advance the start of replay.
/// Admission separately requires restoring the suffix and prepared protection.
pub fn replay_start(previous_ram: u64, checkpoint: u64) -> (start: u64)
    ensures start == checkpoint,
{
    let _ = previous_ram;
    checkpoint
}

/// An accelerator can lead the durable image only while the committed log
/// covers the gap. These facts refer to externally observable application cuts.
pub fn materialization_order(materialized: u64, applied: u64, committed: u64) -> (valid: bool)
    ensures valid <==> materialized <= applied && applied <= committed,
{
    materialized <= applied && applied <= committed
}

/// Atomic materialization must begin at the next durable position and end no
/// later than the applied committed image. The adapter checks every intervening
/// position and its exact term/fingerprint before publishing the native batch.
pub fn materialization_batch(materialized: u64, first: u64, last: u64, applied: u64) -> (valid: bool)
    ensures valid <==> materialized < u64::MAX && first == materialized + 1
        && first <= last && last <= applied,
{
    materialized < u64::MAX && first == materialized + 1
        && first <= last && last <= applied
}

pub fn materialized_checkpoint(applied: u64, materialized: u64, validated: bool) -> (ready: bool)
    ensures ready <==> validated && materialized == applied,
{
    validated && materialized == applied
}

pub fn materialized_replay(materialized: u64, committed: u64) -> (start: Option<u64>)
    ensures start == if materialized <= committed { Some(materialized) } else { None },
{
    if materialized <= committed { Some(materialized) } else { None }
}

}
