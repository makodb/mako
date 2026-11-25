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

}
