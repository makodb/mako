//! One bounded forward lookahead for callers that must cross long tombstone runs.
use super::*;

/// Result of one bounded forward lookahead.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum VersionedLookahead {
    /// The first live selected row in the bounds. Every entry walked before it
    /// selected a tombstone.
    Row(VersionedScanRecord),
    /// The directory holds no further entry in the bounds.
    End,
    /// `max_physical` entries were walked and every one selected a tombstone.
    /// The key is the last entry walked; resume with `ScanBound::Excluded`.
    Resume(Box<[u8]>),
}

impl Table {
    /// Walks at most `max_physical` directory entries forward from `lower`
    /// and stops at the first live selected row.
    ///
    /// Every walked entry is an ordinary read item of `txn`, so the caller can
    /// certify the step and finish the transaction before it resumes. The read
    /// dependency of the walked entries is not reported: use this only in a
    /// transaction that stages no write.
    ///
    /// The walk also reads the table's directory generation, which advances
    /// whenever any thread first touches a key the table has never held. A
    /// caller that resumes over a long tombstone run next to ordinary workers
    /// may therefore fail certification often; a lookahead that does not
    /// depend on the generation may be needed then. None is built here.
    pub fn lookahead_versioned(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        lower: ScanBound<'_>,
        upper: ScanBound<'_>,
        visibility: ReadVisibility,
        max_physical: usize,
    ) -> Result<VersionedLookahead, AccessError> {
        self.lookahead_versioned_inner(txn, Some(worker), lower, upper, visibility, max_physical)
    }

    fn lookahead_versioned_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        lower: ScanBound<'_>,
        upper: ScanBound<'_>,
        visibility: ReadVisibility,
        max_physical: usize,
    ) -> Result<VersionedLookahead, AccessError> {
        if max_physical == 0 {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }
        let adapter = self.record_resource.adapter();
        // The visitor below counts no entry towards the limit until it stops
        // the walk, so the limit only sizes the directory chunks.
        let request = ScanRequest::new(ScanDirection::Forward, max_physical)
            .with_lower(lower)
            .with_upper(upper);
        let mut walked = 0_usize;
        let mut outcome = VersionedLookahead::End;
        let mut scratch = ScanScratch::default();
        self.visit_scan_records(
            txn,
            worker,
            request,
            &mut scratch,
            &mut |key, entry, access| {
                let selected = adapter.selected_version(access, entry, visibility)?;
                walked += 1;
                if let Some(value) = selected.value {
                    outcome = VersionedLookahead::Row(VersionedScanRecord {
                        key: Arc::from(key),
                        value,
                        metadata: selected.metadata,
                        resolved: self.mint_resolved(access.record_id),
                    });
                    return Ok(Some(ScanControl::Stop));
                }
                if walked == max_physical {
                    outcome = VersionedLookahead::Resume(Box::from(key));
                    return Ok(Some(ScanControl::Stop));
                }
                Ok(None)
            },
        )?;
        Ok(outcome)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use sto_core::{AbortReason, CommitOutcome, RuntimeConfig, WorkerContext};

    const UNBOUNDED: ScanBound<'static> = ScanBound::Unbounded;

    fn decision(base: u32) -> DistributedTimeTerm {
        DistributedTimeTerm::new(
            DistributedTimestamp::new(base).unwrap(),
            DistributedTerm::new(0).unwrap(),
        )
    }

    fn latest() -> ReadVisibility {
        ReadVisibility::latest(DistributedTerm::new(0).unwrap())
    }

    /// The table of the neighbouring distributed tests, with a chosen chunk
    /// size: a small one makes a short walk cross several directory chunks.
    fn table(direct: bool, chunk_records: usize) -> (Arc<Runtime>, Table) {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let config = TableConfig::default()
            .with_record_local_versions(true)
            .with_distributed_history(true)
            .with_scan_chunk_records(chunk_records);
        let table = if direct {
            Table::new_memory_direct(&runtime, config)
        } else {
            Table::new_memory(&runtime, config)
        };
        (runtime, table)
    }

    /// The record-token modes and chunk sizes every test runs on.
    fn tables() -> impl Iterator<Item = (Arc<Runtime>, Table)> {
        [(false, 128), (true, 128), (false, 3), (true, 3), (false, 1)]
            .into_iter()
            .map(|(direct, chunk_records)| table(direct, chunk_records))
    }

    fn commit(txn: Transaction<'_, Active>) {
        assert!(matches!(txn.commit(), Ok(CommitOutcome::Committed(_))));
    }

    /// Commits one decided version of `key`: a value, or a tombstone for `None`.
    fn replay(
        table: &Table,
        worker: &mut WorkerContext,
        key: &[u8],
        value: Option<&[u8]>,
        base: u32,
    ) {
        let mut txn = worker.begin().unwrap();
        assert!(table
            .stage_replay_inner(&mut txn, None, key, value.map(Value::from), decision(base))
            .unwrap());
        commit(txn);
    }

    /// A point read that misses: the key becomes a tombstone without history.
    fn miss(table: &Table, worker: &mut WorkerContext, key: &[u8]) {
        let mut txn = worker.begin().unwrap();
        assert!(table.get_inner(&mut txn, None, key).unwrap().is_none());
        commit(txn);
    }

    /// Six tombstones `t1` to `t6`, three of each kind, then the live row `u`.
    fn six_tombstones_then_a_row(table: &Table, worker: &mut WorkerContext) {
        for (index, key) in [b"t1", b"t2", b"t3", b"t4", b"t5", b"t6"]
            .into_iter()
            .enumerate()
        {
            if index % 2 == 0 {
                let base = 10 + 2 * index as u32;
                replay(table, worker, key, Some(b"gone"), base);
                replay(table, worker, key, None, base + 1);
            } else {
                miss(table, worker, key);
            }
        }
        replay(table, worker, b"u", Some(b"live"), 40);
    }

    /// Runs `f` with a second worker of `runtime`, on a thread of its own.
    fn on_second_worker<R: Send>(
        runtime: &Arc<Runtime>,
        f: impl FnOnce(&mut WorkerContext) -> R + Send,
    ) -> R {
        std::thread::scope(|scope| {
            let second = scope.spawn(|| {
                let mut worker = runtime.attach().unwrap();
                f(&mut worker)
            });
            match second.join() {
                Ok(result) => result,
                Err(panic) => std::panic::resume_unwind(panic),
            }
        })
    }

    fn look(
        table: &Table,
        txn: &mut Transaction<'_, Active>,
        lower: ScanBound<'_>,
        upper: ScanBound<'_>,
        max_physical: usize,
    ) -> VersionedLookahead {
        table
            .lookahead_versioned_inner(txn, None, lower, upper, latest(), max_physical)
            .unwrap()
    }

    /// One lookahead in a transaction of its own, which must commit.
    fn step(
        table: &Table,
        worker: &mut WorkerContext,
        lower: ScanBound<'_>,
        upper: ScanBound<'_>,
        max_physical: usize,
    ) -> VersionedLookahead {
        let mut txn = worker.begin().unwrap();
        let found = look(table, &mut txn, lower, upper, max_physical);
        commit(txn);
        found
    }

    fn resume(key: &[u8]) -> VersionedLookahead {
        VersionedLookahead::Resume(Box::from(key))
    }

    /// Key, value and decision of a returned row.
    fn row(found: VersionedLookahead) -> (Vec<u8>, Vec<u8>, Option<DistributedTimeTerm>) {
        match found {
            VersionedLookahead::Row(row) => {
                (row.key.to_vec(), row.value.as_ref().to_vec(), row.metadata)
            }
            other => panic!("expected a row, found {other:?}"),
        }
    }

    fn live(
        key: &[u8],
        value: &[u8],
        base: Option<u32>,
    ) -> (Vec<u8>, Vec<u8>, Option<DistributedTimeTerm>) {
        (key.to_vec(), value.to_vec(), base.map(decision))
    }

    #[test]
    fn lookahead_returns_the_first_live_row_and_skips_tombstones() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            // `a`: deleted, with a value left in its history.
            replay(&table, &mut worker, b"a", Some(b"old-a"), 10);
            replay(&table, &mut worker, b"a", None, 20);
            // `b`: never written.
            miss(&table, &mut worker, b"b");
            // `c`: two committed versions.
            replay(&table, &mut worker, b"c", Some(b"old-c"), 11);
            replay(&table, &mut worker, b"c", Some(b"new-c"), 21);
            // `d`: written without a decision.
            let mut txn = worker.begin().unwrap();
            table
                .put_presence_inner(&mut txn, None, b"d", Value::from(&b"plain-d"[..]))
                .unwrap();
            commit(txn);

            let found = step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 16);
            let VersionedLookahead::Row(record) = &found else {
                panic!("expected a row, found {found:?}");
            };
            let id = table.shared().lookup(None, b"c").unwrap().unwrap();
            assert_eq!(record.resolved, table.mint_resolved(id));
            assert_eq!(row(found), live(b"c", b"new-c", Some(21)));
            // The budget counts the row itself: two tombstones and the row.
            assert_eq!(
                row(step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 3)),
                live(b"c", b"new-c", Some(21))
            );
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"c"),
                    UNBOUNDED,
                    16
                )),
                live(b"d", b"plain-d", None)
            );
        }
    }

    #[test]
    fn lookahead_reports_the_last_walked_key_when_the_budget_ends_on_a_tombstone() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            six_tombstones_then_a_row(&table, &mut worker);

            assert_eq!(
                step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 4),
                resume(b"t4")
            );
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"t4"),
                    UNBOUNDED,
                    4
                )),
                live(b"u", b"live", Some(40))
            );
            // A budget of one advances one entry at a time.
            assert_eq!(
                step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 1),
                resume(b"t1")
            );
            assert_eq!(
                step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"t1"),
                    UNBOUNDED,
                    1
                ),
                resume(b"t2")
            );
            // The budget ends on the last tombstone: the row behind it is not
            // looked at, and one more entry of budget reaches it.
            assert_eq!(
                step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 6),
                resume(b"t6")
            );
            assert_eq!(
                row(step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 7)),
                live(b"u", b"live", Some(40))
            );
            // An upper bound inside the run ends the walk before the budget.
            assert_eq!(
                step(
                    &table,
                    &mut worker,
                    UNBOUNDED,
                    ScanBound::Excluded(b"t3"),
                    4
                ),
                VersionedLookahead::End
            );
            assert_eq!(
                step(
                    &table,
                    &mut worker,
                    UNBOUNDED,
                    ScanBound::Excluded(b"t3"),
                    2
                ),
                resume(b"t2")
            );
        }
    }

    #[test]
    fn lookahead_reports_end_for_an_exhausted_or_empty_range() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            assert_eq!(
                step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 4),
                VersionedLookahead::End
            );

            six_tombstones_then_a_row(&table, &mut worker);
            // Behind the last key.
            assert_eq!(
                step(&table, &mut worker, ScanBound::Excluded(b"u"), UNBOUNDED, 4),
                VersionedLookahead::End
            );
            // Fewer tombstones than the budget and no row behind them.
            assert_eq!(
                step(
                    &table,
                    &mut worker,
                    ScanBound::Included(b"t4"),
                    ScanBound::Excluded(b"u"),
                    4
                ),
                VersionedLookahead::End
            );
            // Bounds that hold no key at all.
            for (lower, upper) in [
                (
                    ScanBound::Included(&b"u"[..]),
                    ScanBound::Excluded(&b"u"[..]),
                ),
                (
                    ScanBound::Excluded(&b"u"[..]),
                    ScanBound::Included(&b"u"[..]),
                ),
                (
                    ScanBound::Included(&b"v"[..]),
                    ScanBound::Included(&b"t1"[..]),
                ),
            ] {
                assert_eq!(
                    step(&table, &mut worker, lower, upper, 4),
                    VersionedLookahead::End
                );
            }
        }
    }

    #[test]
    fn lookahead_refuses_a_budget_of_zero() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            six_tombstones_then_a_row(&table, &mut worker);
            let mut txn = worker.begin().unwrap();
            assert_eq!(
                table.lookahead_versioned_inner(&mut txn, None, UNBOUNDED, UNBOUNDED, latest(), 0),
                Err(AccessError::Capacity(CapacityError::BufferLimit))
            );
            // The refusal dooms the transaction like every failed scan.
            assert_eq!(
                txn.commit(),
                Ok(CommitOutcome::Aborted(AbortReason::Doomed))
            );
        }
    }

    #[test]
    fn lookahead_orders_empty_nul_and_prefix_keys_and_honours_both_bounds() {
        let keys: [&[u8]; 5] = [b"", b"a", b"a\0", b"a\0\0", b"b"];
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            // Written out of key order; `a\0` has the empty value.
            for index in [3, 0, 4, 2, 1] {
                let value: &[u8] = if index == 2 { b"" } else { keys[index] };
                replay(
                    &table,
                    &mut worker,
                    keys[index],
                    Some(value),
                    10 + index as u32,
                );
            }
            let expected = |index: usize| {
                let value: &[u8] = if index == 2 { b"" } else { keys[index] };
                live(keys[index], value, Some(10 + index as u32))
            };

            // A full walk that resumes after the previous row.
            assert_eq!(
                row(step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 8)),
                expected(0)
            );
            for index in 1..keys.len() {
                assert_eq!(
                    row(step(
                        &table,
                        &mut worker,
                        ScanBound::Excluded(keys[index - 1]),
                        UNBOUNDED,
                        8
                    )),
                    expected(index)
                );
            }
            assert_eq!(
                step(&table, &mut worker, ScanBound::Excluded(b"b"), UNBOUNDED, 8),
                VersionedLookahead::End
            );

            // The empty key is a real bound on either side.
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Included(b""),
                    UNBOUNDED,
                    8
                )),
                expected(0)
            );
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    UNBOUNDED,
                    ScanBound::Included(b""),
                    8
                )),
                expected(0)
            );
            assert_eq!(
                step(&table, &mut worker, UNBOUNDED, ScanBound::Excluded(b""), 8),
                VersionedLookahead::End
            );

            // A key and the same key followed by NUL are different rows.
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Included(b"a\0"),
                    UNBOUNDED,
                    8
                )),
                expected(2)
            );
            assert_eq!(
                step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"a"),
                    ScanBound::Excluded(b"a\0"),
                    8
                ),
                VersionedLookahead::End
            );
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"a"),
                    ScanBound::Included(b"a\0"),
                    8
                )),
                expected(2)
            );

            // The upper bound, excluded and included.
            assert_eq!(
                step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"a\0\0"),
                    ScanBound::Excluded(b"b"),
                    8
                ),
                VersionedLookahead::End
            );
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"a\0\0"),
                    ScanBound::Included(b"b"),
                    8
                )),
                expected(4)
            );
        }
    }

    #[test]
    fn lookahead_step_validates_and_finishes_as_a_read_only_transaction() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            six_tombstones_then_a_row(&table, &mut worker);

            // A step that ends on the budget, then one that ends on the row,
            // each as a participant that validates and finishes committed
            // without an install.
            for (lower, expected) in [
                (UNBOUNDED, resume(b"t4")),
                (
                    ScanBound::Excluded(&b"t4"[..]),
                    VersionedLookahead::Row(VersionedScanRecord {
                        key: Arc::from(&b"u"[..]),
                        value: Value::from(&b"live"[..]),
                        metadata: Some(decision(40)),
                        resolved: table
                            .mint_resolved(table.shared().lookup(None, b"u").unwrap().unwrap()),
                    }),
                ),
            ] {
                let mut participant = worker.begin().unwrap().into_participant();
                let found = participant
                    .with_transaction(|txn| {
                        table.lookahead_versioned_inner(txn, None, lower, UNBOUNDED, latest(), 4)
                    })
                    .unwrap();
                assert_eq!(found, expected);
                assert!(!participant.is_empty());
                assert!(!participant.has_writes());
                participant.validate().unwrap();
                assert!(matches!(
                    participant.finish(true),
                    Ok(CommitOutcome::Committed(_))
                ));
            }

            // The same steps in a transaction that already holds items of the
            // table, each of them an entry one of the walks visits again.
            let mut txn = worker.begin().unwrap();
            for key in [&b"t2"[..], b"u"] {
                table
                    .get_versioned_inner(&mut txn, None, key, latest())
                    .unwrap();
            }
            assert_eq!(
                look(&table, &mut txn, UNBOUNDED, UNBOUNDED, 4),
                resume(b"t4")
            );
            assert_eq!(
                row(look(
                    &table,
                    &mut txn,
                    ScanBound::Excluded(b"t4"),
                    UNBOUNDED,
                    4
                )),
                live(b"u", b"live", Some(40))
            );
            commit(txn);
        }
    }

    #[test]
    fn lookahead_read_set_fails_certification_after_a_new_key_is_interned() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            six_tombstones_then_a_row(&table, &mut worker);

            // Control: a foreign commit to a key the table already holds,
            // outside the walked stretch, leaves the step valid.
            let mut txn = worker.begin().unwrap();
            assert_eq!(
                look(&table, &mut txn, UNBOUNDED, UNBOUNDED, 3),
                resume(b"t3")
            );
            on_second_worker(&runtime, |second| {
                replay(&table, second, b"u", Some(b"again"), 50);
            });
            commit(txn);

            // A key the table has never seen, far behind the walked stretch.
            let mut txn = worker.begin().unwrap();
            assert_eq!(
                look(&table, &mut txn, UNBOUNDED, UNBOUNDED, 3),
                resume(b"t3")
            );
            on_second_worker(&runtime, |second| {
                replay(&table, second, b"zz", Some(b"new"), 60);
            });
            assert_eq!(
                txn.commit(),
                Ok(CommitOutcome::Aborted(AbortReason::Conflict(
                    Conflict::ReadValidation
                )))
            );

            // A foreign point read that misses interns its key as well, and
            // fails the step in the same way without writing anything.
            let mut txn = worker.begin().unwrap();
            assert_eq!(
                look(&table, &mut txn, UNBOUNDED, UNBOUNDED, 3),
                resume(b"t3")
            );
            on_second_worker(&runtime, |second| miss(&table, second, b"zy"));
            assert_eq!(
                txn.commit(),
                Ok(CommitOutcome::Aborted(AbortReason::Conflict(
                    Conflict::ReadValidation
                )))
            );
        }
    }

    #[test]
    fn lookahead_read_set_fails_certification_after_a_walked_tombstone_becomes_live() {
        for (runtime, table) in tables() {
            let mut worker = runtime.attach().unwrap();
            six_tombstones_then_a_row(&table, &mut worker);

            // `t2` was a point miss and `t3` has history. Each is walked by a
            // step that ends on the budget, and comes back before the step
            // is certified.
            for (lower, walked_to, key, base) in [
                (UNBOUNDED, &b"t2"[..], &b"t2"[..], 70),
                (ScanBound::Excluded(&b"t2"[..]), &b"t4"[..], &b"t3"[..], 71),
            ] {
                let mut txn = worker.begin().unwrap();
                assert_eq!(
                    look(&table, &mut txn, lower, UNBOUNDED, 2),
                    resume(walked_to)
                );
                on_second_worker(&runtime, |second| {
                    replay(&table, second, key, Some(b"back"), base);
                });
                assert_eq!(
                    txn.commit(),
                    Ok(CommitOutcome::Aborted(AbortReason::Conflict(
                        Conflict::ReadValidation
                    )))
                );
            }
            // What the failed steps passed over is there for the next ones.
            assert_eq!(
                row(step(&table, &mut worker, UNBOUNDED, UNBOUNDED, 2)),
                live(b"t2", b"back", Some(70))
            );
            assert_eq!(
                row(step(
                    &table,
                    &mut worker,
                    ScanBound::Excluded(b"t2"),
                    UNBOUNDED,
                    2
                )),
                live(b"t3", b"back", Some(71))
            );
        }
    }
}
