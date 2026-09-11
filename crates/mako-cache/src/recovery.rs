//! Recover the current versioned checkpoint and its retained log suffix.
//!
//! Payloads are visited one at a time. Only the winner index and timestamp
//! identities remain in Rust memory; old transaction payloads are never
//! collected or replayed. The cache stays private until hydration completes.

use std::collections::HashMap;

use mako_local::{CommitDisposition, LocalDb, MakoTimestamp};
use mrx_core::{BlobError, BlobOp, Blobs};

use crate::checkpoint::{self, LaneMetadata, Row};
use crate::record::{classify_backend_key, split_log_sequence, BackendKey, CommitRecord, Mutation};
use crate::writeback_set::{LaneRecovery, RecoveredWriteback};
use crate::{CommitSeq, Error, DEFAULT_TABLE_ID, DEFAULT_TABLE_NAME};

/// Preserve the cache's structured error when the backend's visitor boundary
/// accepts only BlobError. Backend iteration failures remain backend errors.
fn visit<B: Blobs>(
    backend: &B,
    mut f: impl FnMut(&[u8], &[u8]) -> Result<(), Error>,
) -> Result<(), Error> {
    let mut failure = None;
    let result = backend.for_each_entry(&mut |key, value| match f(key, value) {
        Ok(()) => Ok(()),
        Err(error) => {
            failure = Some(error);
            Err(BlobError("cache recovery validation failed".into()))
        }
    });
    if let Some(error) = failure {
        Err(error)
    } else {
        result.map_err(Error::Backend)
    }
}

fn remember_identity(
    identities: &mut HashMap<MakoTimestamp, CommitSeq>,
    sequences: &mut HashMap<CommitSeq, MakoTimestamp>,
    timestamp: MakoTimestamp,
    sequence: CommitSeq,
) -> Result<(), Error> {
    if let Some(previous) = sequences.get(&sequence) {
        if *previous != timestamp {
            return Err(Error::BackendStateMismatch);
        }
    } else {
        sequences
            .try_reserve(1)
            .map_err(|_| Error::AllocationFailed)?;
        sequences.insert(sequence, timestamp);
    }
    if let Some(previous) = identities.get(&timestamp) {
        if *previous != sequence {
            return Err(Error::BackendStateMismatch);
        }
    } else {
        identities
            .try_reserve(1)
            .map_err(|_| Error::AllocationFailed)?;
        identities.insert(timestamp, sequence);
    }
    Ok(())
}

fn covered_row<'a>(value: &'a [u8], metadata: &[LaneMetadata]) -> Result<Row<'a>, Error> {
    let row = checkpoint::decode_row(value).map_err(|_| Error::BackendStateMismatch)?;
    let tag = checkpoint::lane_tag(row.sequence).map_err(|_| Error::BackendStateMismatch)?;
    let (_, local) = split_log_sequence(row.sequence).ok_or(Error::BackendStateMismatch)?;
    let lane = metadata
        .get(usize::from(tag))
        .ok_or(Error::BackendStateMismatch)?;
    if local > lane.applied || Some(row.timestamp) > lane.max_timestamp {
        return Err(Error::BackendStateMismatch);
    }
    // Tagged workers mint strictly increasing HLCs, so their maximum belongs
    // to exactly A. This evidence survives even when that log was reclaimed.
    // The untagged stream may have reversed timestamps and has no such rule.
    if tag != 0 && ((local == lane.applied) != (Some(row.timestamp) == lane.max_timestamp)) {
        return Err(Error::BackendStateMismatch);
    }
    Ok(row)
}

pub(crate) fn recover<B: Blobs>(
    local: &LocalDb,
    backend: &B,
    max_bytes: usize,
) -> Result<RecoveredWriteback, Error> {
    let mut recovered = RecoveredWriteback::empty();
    let mut saw_key = false;
    let mut saw_format = false;
    let mut seen_lanes = vec![false; mako_local::MAX_WORKERS + 1];

    // First pass establishes all lane coverage before interpreting rows or
    // logs. This also rejects an unknown key anywhere in the namespace.
    visit(backend, |key, value| {
        saw_key = true;
        match classify_backend_key(key) {
            BackendKey::Format => {
                if saw_format {
                    return Err(Error::BackendStateMismatch);
                }
                checkpoint::validate_format(value).map_err(|_| Error::RebuildRequired)?;
                saw_format = true;
            }
            BackendKey::Lane(tag) => {
                let index = usize::from(tag);
                let seen = seen_lanes
                    .get_mut(index)
                    .ok_or(Error::BackendStateMismatch)?;
                if *seen {
                    return Err(Error::BackendStateMismatch);
                }
                *seen = true;
                recovered.metadata[index] =
                    LaneMetadata::decode(value).map_err(|_| Error::BackendStateMismatch)?;
            }
            BackendKey::Log(_) => {}
            BackendKey::Data { table_id, .. } => {
                if table_id != DEFAULT_TABLE_ID {
                    return Err(Error::UnsupportedTable(table_id));
                }
            }
            BackendKey::Foreign => {
                return Err(if key.starts_with(b"\0mako-cache\0") {
                    Error::RebuildRequired
                } else {
                    Error::ForeignBackendKey
                });
            }
        }
        Ok(())
    })?;
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::RecoveryKeysEnumerated);

    if !saw_key {
        let format = checkpoint::encode_format()?;
        backend.write_batch(&[BlobOp::Put {
            key: checkpoint::FORMAT_KEY,
            val: &format,
        }])?;
    } else if !saw_format {
        return Err(Error::BackendStateMismatch);
    }

    let mut identities = HashMap::new();
    let mut sequences = HashMap::new();
    let mut maximum_owners = HashMap::new();
    for (tag, metadata) in recovered.metadata.iter().enumerate() {
        recovered.record_count = recovered
            .record_count
            .checked_add(metadata.applied)
            .ok_or(Error::BackendStateMismatch)?;
        let seed = LaneRecovery {
            local_tail: metadata.applied,
            mako_timestamp: metadata.max_timestamp,
        };
        if tag == 0 {
            recovered.legacy = seed;
        } else {
            recovered.lanes[tag - 1] = seed;
        }
        if let Some(timestamp) = metadata.max_timestamp {
            if tag != 0 {
                let sequence = CommitSeq::new(
                    ((tag as u64) << crate::record::LOG_LANE_SHIFT) | metadata.applied,
                )
                .ok_or(Error::BackendStateMismatch)?;
                remember_identity(&mut identities, &mut sequences, timestamp, sequence)?;
            }
            if maximum_owners.insert(timestamp, tag).is_some() {
                return Err(Error::BackendStateMismatch);
            }
            recovered.maximum_timestamp = Some(
                recovered
                    .maximum_timestamp
                    .map_or(timestamp, |old| old.max(timestamp)),
            );
        }
    }

    // This index includes tombstones, even though Silo exposes absence for a
    // deleted key. No payload Vec is retained during checkpoint validation.
    let mut live_rows = 0usize;
    visit(backend, |key, value| {
        if let BackendKey::Data {
            table_id,
            key: user_key,
        } = classify_backend_key(key)
        {
            let row = covered_row(value, &recovered.metadata)?;
            remember_identity(&mut identities, &mut sequences, row.timestamp, row.sequence)?;
            let tag = checkpoint::lane_tag(row.sequence)?;
            let (_, position) =
                split_log_sequence(row.sequence).ok_or(Error::BackendStateMismatch)?;
            if position > recovered.metadata[usize::from(tag)].reclaimed {
                // If the source log still exists, an envelope must actually
                // be one of its mutations, not an extra row claiming its ID.
                let source_key = checkpoint::log_key(tag, position)?;
                let source_value = backend
                    .get(&source_key)?
                    .ok_or(Error::BackendStateMismatch)?;
                let record = CommitRecord::decode(&source_key, &source_value, max_bytes)?;
                if record.mako_timestamp() != row.timestamp
                    || !record.mutations().iter().any(|mutation| match mutation {
                        Mutation::Put {
                            table_id: table,
                            key,
                            value,
                        } => {
                            *table == table_id
                                && key == user_key
                                && row.value == Some(value.as_slice())
                        }
                        Mutation::Delete {
                            table_id: table,
                            key,
                        } => *table == table_id && key == user_key && row.value.is_none(),
                    })
                {
                    return Err(Error::BackendStateMismatch);
                }
            }
            if let Some(owner) = maximum_owners.get(&row.timestamp) {
                if *owner != usize::from(checkpoint::lane_tag(row.sequence)?) {
                    return Err(Error::BackendStateMismatch);
                }
            }
            recovered
                .latest
                .try_reserve(1)
                .map_err(|_| Error::AllocationFailed)?;
            recovered.latest.insert(key.to_vec(), row.timestamp);
            if row.value.is_some() {
                live_rows = live_rows.checked_add(1).ok_or(Error::AllocationFailed)?;
            }
        }
        Ok(())
    })?;

    let mut tails: Vec<u64> = recovered
        .metadata
        .iter()
        .map(|lane| lane.reclaimed)
        .collect();
    let mut retained_bytes = vec![0u64; recovered.metadata.len()];
    let mut previous_timestamp = vec![None; recovered.metadata.len()];
    let mut validated = 0u64;
    visit(backend, |key, value| {
        let BackendKey::Log(sequence) = classify_backend_key(key) else {
            return Ok(());
        };
        let record = CommitRecord::decode(key, value, max_bytes)?;
        let tag = usize::from(checkpoint::lane_tag(sequence)?);
        let (_, position) = split_log_sequence(sequence).ok_or(Error::BackendStateMismatch)?;
        let lane = recovered
            .metadata
            .get(tag)
            .ok_or(Error::BackendStateMismatch)?;
        if tails[tag].checked_add(1) != Some(position)
            || position > lane.applied
            || Some(record.mako_timestamp()) > lane.max_timestamp
            || (tag != 0
                && previous_timestamp[tag].is_some_and(|old| old >= record.mako_timestamp()))
        {
            return Err(Error::BackendStateMismatch);
        }
        tails[tag] = position;
        previous_timestamp[tag] = Some(record.mako_timestamp());
        retained_bytes[tag] = retained_bytes[tag]
            .checked_add(
                u64::try_from(
                    key.len()
                        .checked_add(value.len())
                        .ok_or(Error::AllocationFailed)?,
                )
                .map_err(|_| Error::AllocationFailed)?,
            )
            .ok_or(Error::BackendStateMismatch)?;
        remember_identity(
            &mut identities,
            &mut sequences,
            record.mako_timestamp(),
            sequence,
        )?;
        if let Some(owner) = maximum_owners.get(&record.mako_timestamp()) {
            if *owner != tag {
                return Err(Error::BackendStateMismatch);
            }
        }
        for (mutation, data_key) in record.mutations().iter().zip(record.data_keys()) {
            match mutation {
                Mutation::Put { table_id, .. } | Mutation::Delete { table_id, .. }
                    if *table_id != DEFAULT_TABLE_ID =>
                {
                    return Err(Error::UnsupportedTable(*table_id))
                }
                _ => {}
            }
            let encoded = backend.get(data_key)?.ok_or(Error::BackendStateMismatch)?;
            let winner = covered_row(&encoded, &recovered.metadata)?;
            if winner.timestamp < record.mako_timestamp() {
                return Err(Error::BackendStateMismatch);
            }
            if winner.timestamp == record.mako_timestamp() {
                let expected = match mutation {
                    Mutation::Put { value, .. } => Some(value.as_slice()),
                    Mutation::Delete { .. } => None,
                };
                if winner.sequence != sequence || winner.value != expected {
                    return Err(Error::BackendStateMismatch);
                }
            }
        }
        validated += 1;
        #[cfg(test)]
        if validated == 1 {
            crate::failpoint::hit(crate::failpoint::Point::RecoveryFirstRecordValidated);
        }
        Ok(())
    })?;
    for (tag, metadata) in recovered.metadata.iter().enumerate() {
        if tails[tag] != metadata.applied
            || retained_bytes[tag] != metadata.retained_bytes
            || (tag != 0
                && metadata.applied > metadata.reclaimed
                && previous_timestamp[tag] != metadata.max_timestamp)
        {
            return Err(Error::BackendStateMismatch);
        }
    }
    #[cfg(test)]
    {
        if validated != 0 {
            crate::failpoint::hit(crate::failpoint::Point::RecoveryLastRecordValidated);
        }
        crate::failpoint::hit(crate::failpoint::Point::RecoveryMaterializedValidated);
    }
    drop(identities);
    drop(sequences);

    if let Some(timestamp) = recovered.maximum_timestamp {
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryBeforeClockFloor);
        mako_local::advance_mako_timestamp_past(timestamp)?;
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryAfterClockFloor);
    }

    let table = local.open_table(DEFAULT_TABLE_NAME, DEFAULT_TABLE_ID)?;
    let mut loaded = 0usize;
    // Every native transaction is bounded to one current row. This table is
    // private until the whole startup succeeds, so no reader sees partial load.
    visit(backend, |key, value| {
        let BackendKey::Data { key: raw, .. } = classify_backend_key(key) else {
            return Ok(());
        };
        let row = covered_row(value, &recovered.metadata)?;
        let Some(value) = row.value else {
            return Ok(());
        };
        let mut txn = local.transaction()?;
        txn.put(&table, raw, value)?;
        let report = txn.commit_report();
        match report.disposition {
            CommitDisposition::Committed => report.cleanup.map_err(Error::Native)?,
            CommitDisposition::Aborted(error) | CommitDisposition::Unknown(error) => {
                return Err(Error::Native(error))
            }
        }
        loaded += 1;
        #[cfg(test)]
        {
            crate::record_replayed_sequence(row.sequence);
            if loaded == live_rows.div_ceil(2) {
                crate::failpoint::hit(crate::failpoint::Point::RecoveryMidReplay);
            }
        }
        Ok(())
    })?;
    if loaded != live_rows {
        return Err(Error::RecoveryDiverged);
    }
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::RecoveryReplayComplete);
    Ok(recovered)
}
