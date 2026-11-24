//! Explicit distributed history for the ordinary transactional table.
//!
//! Histories share the record's existing immutable ArcSwap payload slot, so
//! local-only tables retain their inline layout and allocation-free small rows.
//! OCC certification protects the complete selected payload/metadata pair;
//! history selection never substitutes a historical OCC generation.
use super::*;
use mako_clock::distributed::{DistributedTerm, DistributedTimeTerm, DistributedTimestamp};

mod lookahead;
pub use lookahead::{PinnedLookahead, VersionedLookahead};

/// Visibility supplied by the replication/failure authority, in BASE units.
/// An absent per-term watermark means that term's watermark is not known yet
/// (the FAIL_NEW_VERSION policy); it does not mean a zero allocated timestamp.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReadVisibility {
    pub current_term: DistributedTerm,
    pub stable_through: [Option<u32>; 10],
    /// When set, reject a selected dependency beyond this failed-shard BASE
    /// bound. Zero is meaningful and rejects every allocated timestamp.
    pub failed_shard_through: Option<u32>,
}

impl ReadVisibility {
    pub const fn latest(current_term: DistributedTerm) -> Self {
        Self {
            current_term,
            stable_through: [None; 10],
            failed_shard_through: None,
        }
    }

    fn visible(self, metadata: Option<DistributedTimeTerm>) -> bool {
        let Some(metadata) = metadata else {
            return true;
        };
        metadata.term() == self.current_term
            || self.stable_through[usize::from(metadata.term().get())]
                .is_none_or(|watermark| metadata.timestamp().get() <= watermark)
    }

    fn certify(self, metadata: Option<DistributedTimeTerm>) -> Result<(), AccessError> {
        if let (Some(metadata), Some(watermark)) = (metadata, self.failed_shard_through) {
            if metadata.timestamp().get() > watermark {
                return Err(Conflict::ReadValidation.into());
            }
        }
        Ok(())
    }
}

/// The selected committed version, including metadata for a selected tombstone.
/// `metadata == None` means bootstrap data or a not-yet-decided staged write.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedRead {
    pub value: Option<Value>,
    pub metadata: Option<DistributedTimeTerm>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedScanRecord {
    pub key: Arc<[u8]>,
    pub value: Value,
    pub metadata: Option<DistributedTimeTerm>,
    pub resolved: ResolvedRecord,
}

/// Dependency includes selected tombstones traversed before the logical limit.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedScan {
    pub rows: Vec<VersionedScanRecord>,
    pub dependency: Option<DistributedTimestamp>,
}

#[derive(Debug, Eq, PartialEq)]
pub(super) struct DistributedVersion {
    pub(super) state: RecordState,
    pub(super) metadata: Option<DistributedTimeTerm>,
    pub(super) previous: Option<Arc<SharedValue>>,
    // The previous chain was already filtered at this floor. Avoid rescanning
    // it when an unchanged authority floor is supplied for the next install.
    pruned_through: u32,
}

impl Table {
    /// Advances the authoritative shard reclamation floor, in BASE units.
    ///
    /// Call only when reclamation is authorized, using the stable shard
    /// watermark (not a read dependency or a per-term visibility watermark).
    /// Zero/unknown authority requires no call. The floor cannot move back:
    /// discarded versions cannot be recreated. Subsequent installs retain the
    /// new head and the prior prefix at or above the floor, cutting the first
    /// older node and its tail. Existing owned snapshots remain unchanged.
    pub fn advance_history_retention_floor(&self, floor: DistributedTimestamp) {
        self.shared()
            .history_retention_floor
            .fetch_max(floor.get(), Ordering::AcqRel);
    }

    /// Reads a selected historical value and its own dependency metadata.
    pub fn get_versioned(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visibility: ReadVisibility,
    ) -> Result<VersionedRead, AccessError> {
        self.get_versioned_inner(txn, Some(worker), key, visibility)
    }

    /// The value borrow is scoped to the callback, never to a native history pointer.
    pub fn visit_get_versioned<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visibility: ReadVisibility,
        visit: impl for<'value> FnOnce(Option<&'value Value>, Option<DistributedTimeTerm>) -> R,
    ) -> Result<R, AccessError> {
        let selected = self.get_versioned(txn, worker, key, visibility)?;
        Ok(visit(selected.value.as_ref(), selected.metadata))
    }

    fn get_versioned_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visibility: ReadVisibility,
    ) -> Result<VersionedRead, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(
            txn,
            worker,
            key,
            || self.shared().lookup(worker, key),
            |entry, access| adapter.selected_version(access, entry, visibility),
        )
    }

    /// Scans physical records through the normal directory/predicate machinery,
    /// selecting history BEFORE tombstone filtering and logical-limit counting.
    pub fn scan_versioned(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        visibility: ReadVisibility,
    ) -> Result<VersionedScan, AccessError> {
        self.scan_versioned_inner(txn, Some(worker), request, visibility)
    }

    fn scan_versioned_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        visibility: ReadVisibility,
    ) -> Result<VersionedScan, AccessError> {
        let adapter = self.record_resource.adapter();
        let mut result = VersionedScan {
            rows: Vec::new(),
            dependency: None,
        };
        let mut scratch = ScanScratch::default();
        self.visit_scan_records(
            txn,
            worker,
            request,
            &mut scratch,
            &mut |key, entry, access| {
                let selected = adapter.selected_version(access, entry, visibility)?;
                if let Some(metadata) = selected.metadata {
                    result.dependency = Some(
                        result
                            .dependency
                            .map_or(metadata.timestamp(), |old| old.max(metadata.timestamp())),
                    );
                }
                let Some(value) = selected.value else {
                    return Ok(None);
                };
                result
                    .rows
                    .try_reserve(1)
                    .map_err(|_| AccessError::from(CapacityError::BufferLimit))?;
                result.rows.push(VersionedScanRecord {
                    key: Arc::from(key),
                    value,
                    metadata: selected.metadata,
                    resolved: self.mint_resolved(access.record_id),
                });
                Ok(Some(ScanControl::Continue))
            },
        )?;
        Ok(result)
    }

    /// Compare-and-stage a replay mutation using its ORIGINAL source decision.
    /// Returns false for a duplicate/older record. The caller must still commit
    /// the transaction: OCC certifies the comparison, and abort publishes nothing.
    /// A higher term OR higher base wins, matching the legacy replay ordering.
    /// The replay authority must observe the recovered base before foreground
    /// admission, even when this comparison returns false. This method never
    /// allocates or substitutes a source timestamp.
    pub fn stage_replay(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: Option<Value>,
        source: DistributedTimeTerm,
    ) -> Result<bool, AccessError> {
        self.stage_replay_inner(txn, Some(worker), key, value, source)
    }

    fn stage_replay_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: Option<Value>,
        source: DistributedTimeTerm,
    ) -> Result<bool, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(
            txn,
            worker,
            key,
            || self.shared().lookup(worker, key),
            |entry, access| {
                if !adapter.table.registry.config.distributed_history {
                    return Err(table_fault("replay requires distributed history mode"));
                }
                let selected = adapter.selected_version(
                    access,
                    entry,
                    ReadVisibility::latest(source.term()),
                )?;
                if let Some(old) = selected.metadata {
                    if source.term() <= old.term() && source.timestamp() <= old.timestamp() {
                        return Ok(false);
                    }
                }
                let replacement = value.map_or(RecordState::Tombstone, RecordState::Live);
                match entry.intent_mut() {
                    Some(TableIntent::Replay(replay)) => {
                        replay.replacement = replacement;
                        replay.source = source;
                    }
                    Some(intent) => {
                        *intent = TableIntent::Replay(Box::new(ReplayIntent {
                            replacement,
                            source,
                        }));
                    }
                    None => entry.add_write(TableIntent::Replay(Box::new(ReplayIntent {
                        replacement,
                        source,
                    })))?,
                }
                Ok(true)
            },
        )
    }
}

impl TableAdapter {
    fn selected_version(
        &self,
        access: RecordAccess<'_>,
        entry: &mut TransProxy<'_, Self>,
        visibility: ReadVisibility,
    ) -> Result<VersionedRead, AccessError> {
        // Establish the normal OCC observation without cloning the head
        // payload: the selected value may belong to an older history node.
        // Validation below covers both descriptor and history pointer loads.
        self.prepare_resolved_presence_access(access, entry)?;
        if let Some(intent) = entry.intent() {
            let source = intent.source();
            visibility.certify(source)?;
            return Ok(VersionedRead {
                value: intent.record_state().value().cloned(),
                metadata: source,
            });
        }
        let head = if self.table.registry.config.distributed_history {
            access.record.state.shared.load_full()
        } else {
            None
        };
        let observed = match entry.observation() {
            ObservationRef::Read(TableObservation::Record(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
                observation.version
            }
            _ => return Err(table_fault("versioned read has no OCC observation")),
        };
        if !access.record.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        let mut cursor = head.as_deref();
        while let Some(SharedValue::Distributed(version)) = cursor {
            if visibility.visible(version.metadata) {
                visibility.certify(version.metadata)?;
                return Ok(VersionedRead {
                    value: version.state.value().cloned(),
                    metadata: version.metadata,
                });
            }
            cursor = version.previous.as_deref();
        }
        if matches!(head.as_deref(), Some(SharedValue::Distributed(_))) {
            // There was committed history but no stable version. Never expose
            // the unstable head merely because the history chain ended.
            return Err(Conflict::ReadValidation.into());
        }
        let (state, _) = self.snapshot_state(access, observed)?;
        let value = match state {
            RecordState::Live(value) => Some(value),
            RecordState::Tombstone => None,
        };
        Ok(VersionedRead {
            value,
            metadata: None,
        })
    }
}

/// Called only for explicitly enabled tables, with their OCC lock held.
/// Returns false for ordinary bootstrap writes, preserving local fast paths.
pub(super) fn publish_distributed_if_needed(
    access: RecordAccess<'_>,
    table: &TableShared,
    replacement: &RecordState,
    metadata: Option<DistributedTimeTerm>,
    owner: OwnerId,
    commit_id: Option<OccCommitId>,
) -> bool {
    let old = access.record.state.shared.load_full();
    let has_history = matches!(old.as_deref(), Some(SharedValue::Distributed(_)));
    if metadata.is_none() && !has_history {
        return false;
    }
    assert!(
        metadata.is_some(),
        "a distributed record overwrite requires an accepted decision"
    );
    let retention_floor = table.history_retention_floor.load(Ordering::Acquire);
    let previous = if has_history {
        prune_history(old, retention_floor)
    } else if retention_floor != 0 {
        // Bootstrap is below every allocated authority floor. Do not allocate
        // a bootstrap node only to discard it in the same publication.
        None
    } else {
        // Capture the real bootstrap state before marking publication in
        // progress, including absence. Zero is NOT an allocated timestamp.
        let state = match access.record.state.load(access.stable) {
            CommittedStateLoad::Complete { state, .. } => state,
            CommittedStateLoad::Incomplete(message) => panic!("{message}"),
        };
        Some(Arc::new(SharedValue::Distributed(DistributedVersion {
            state,
            metadata: None,
            previous: None,
            pruned_through: 0,
        })))
    };
    // A legacy get may return a Value owning the whole history node. Retain
    // just its immutable payload rather than nesting another history in it.
    let state = match replacement {
        RecordState::Live(Value {
            repr: ValueRepr::Shared(value),
        }) => match value.as_ref() {
            SharedValue::Distributed(version) => version.state.clone(),
            _ => replacement.clone(),
        },
        _ => replacement.clone(),
    };
    let descriptor = if state.is_live() {
        RECORD_STATE_SHARED
    } else {
        RECORD_STATE_DISTRIBUTED_TOMBSTONE
    };
    let version = Arc::new(SharedValue::Distributed(DistributedVersion {
        state,
        metadata,
        previous,
        pruned_through: retention_floor,
    }));
    let mut publication = access.record.state.begin_publication(table, access.stable);
    publication
        .table
        .advance_scan_generation_for_commit(owner, commit_id);
    publication.state.shared.store(Some(version));
    publication
        .state
        .tail_and_descriptor
        .store(pack_record_state_word(descriptor, 0), Ordering::Release);
    publication.completed = true;
    true
}

/// Copy-on-write equivalent of cow_prune_value_chain: equality is retained,
/// and the first older node cuts the entire tail even for nonmonotone replay.
fn prune_history(previous: Option<Arc<SharedValue>>, floor: u32) -> Option<Arc<SharedValue>> {
    if floor == 0 {
        return previous;
    }
    let Some(SharedValue::Distributed(head)) = previous.as_deref() else {
        return previous;
    };
    let head_base = head
        .metadata
        .map_or(0, |metadata| metadata.timestamp().get());
    if head_base < floor {
        return None;
    }
    if head.pruned_through >= floor {
        return previous;
    }

    // First determine whether a cutoff exists without allocating scratch or
    // cloning any payload. With no cutoff the old immutable tail is reused.
    let mut count = 0;
    let mut cursor = previous.as_deref();
    let mut cutoff = false;
    while let Some(SharedValue::Distributed(version)) = cursor {
        if version
            .metadata
            .map_or(0, |metadata| metadata.timestamp().get())
            < floor
        {
            cutoff = true;
            break;
        }
        count += 1;
        cursor = version.previous.as_deref();
    }
    if !cutoff {
        return previous;
    }
    let mut retained = Vec::with_capacity(count);
    cursor = previous.as_deref();
    for _ in 0..count {
        let Some(SharedValue::Distributed(version)) = cursor else {
            unreachable!("retained prefix was counted above");
        };
        retained.push(version);
        cursor = version.previous.as_deref();
    }
    let mut cloned = None;
    for version in retained.into_iter().rev() {
        cloned = Some(Arc::new(SharedValue::Distributed(DistributedVersion {
            state: version.state.clone(),
            metadata: version.metadata,
            previous: cloned,
            pruned_through: floor,
        })));
    }
    cloned
}

impl Drop for DistributedVersion {
    fn drop(&mut self) {
        // Iterative destruction avoids overflowing the stack on a long-lived
        // row. Shared tails remain owned by outstanding snapshots/readers.
        let mut previous = self.previous.take();
        while let Some(node) = previous {
            match Arc::try_unwrap(node) {
                Ok(SharedValue::Distributed(mut version)) => {
                    previous = version.previous.take();
                }
                _ => break,
            }
        }
    }
}

#[cfg(test)]
mod tests;
