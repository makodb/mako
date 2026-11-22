//! End-to-end checks of the Rust STO engine against the native Masstree.
//!
//! These tests need the CMake archives: run them with
//! `MAKO_MTREE_NATIVE_INTEGRATION=1`, `MAKO_MTREE_NATIVE_LIB_DIRS`, and
//! `MAKO_MTREE_NATIVE_LIBS` exactly as the `rust_sto_integration` CMake target
//! sets them. The cache-order namespace is process-global, so every test
//! serializes on one mutex and opens its own database.

use std::cell::UnsafeCell;
use std::num::{NonZeroU32, NonZeroU64};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, MutexGuard};

use mako_engine_sto::{
    record_format, CacheOrderMode, CommitDisposition, CommitRecordChecksum, CommitRecordTarget,
    Error, LocalDb, MakoTimestamp, TrustedFusedOnePutHolderAttempt,
    TrustedNativeOrderedArenaControl, TrustedNativeOrderedHolderControl, TrustedOnePutHolderPool,
    TrustedSpscOnePutHolderControl,
};

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn parse_header(record: &[u8]) -> (u16, u64, MakoTimestamp, u32) {
    let version = u16::from_be_bytes(record[8..10].try_into().unwrap());
    let sequence = u64::from_be_bytes(record[10..18].try_into().unwrap());
    let physical_us = u64::from_be_bytes(record[18..26].try_into().unwrap());
    let logical = u32::from_be_bytes(record[26..30].try_into().unwrap());
    let origin = u32::from_be_bytes(record[30..34].try_into().unwrap());
    let op_count = u32::from_be_bytes(record[34..38].try_into().unwrap());
    (
        version,
        sequence,
        MakoTimestamp::new(physical_us, logical, origin).unwrap(),
        op_count,
    )
}

fn parse_ops(record: &[u8]) -> Vec<(u8, u64, Vec<u8>, Vec<u8>)> {
    let op_count = u32::from_be_bytes(record[34..38].try_into().unwrap());
    let mut cursor = record_format::HEADER_BYTES;
    let mut ops = Vec::new();
    for _ in 0..op_count {
        let tag = record[cursor];
        let table_id = u64::from_be_bytes(record[cursor + 1..cursor + 9].try_into().unwrap());
        let key_len =
            u32::from_be_bytes(record[cursor + 9..cursor + 13].try_into().unwrap()) as usize;
        let value_len =
            u32::from_be_bytes(record[cursor + 13..cursor + 17].try_into().unwrap()) as usize;
        cursor += record_format::OP_HEADER_BYTES;
        let key = record[cursor..cursor + key_len].to_vec();
        cursor += key_len;
        let value = record[cursor..cursor + value_len].to_vec();
        cursor += value_len;
        ops.push((tag, table_id, key, value));
    }
    ops
}

#[test]
fn point_operations_compose_with_read_your_writes() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    let table = db.open_table("points", 1).unwrap();

    let mut tx = db.transaction().unwrap();
    assert!(tx.put(&table, b"a", b"1").unwrap());
    assert_eq!(tx.get(&table, b"a").unwrap(), Some(b"1".to_vec()));
    assert!(!tx.insert(&table, b"a", b"2").unwrap());
    assert!(tx.insert(&table, b"b", b"2").unwrap());
    assert!(tx.remove(&table, b"b").unwrap());
    assert_eq!(tx.get(&table, b"b").unwrap(), None);
    assert!(!tx.remove(&table, b"missing").unwrap());
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"a").unwrap(), Some(b"1".to_vec()));
    assert_eq!(tx.get(&table, b"b").unwrap(), None);
    assert!(!tx.put(&table, b"a", b"3").unwrap());
    assert!(tx.remove(&table, b"a").unwrap());
    assert!(tx.put(&table, b"a", b"4").unwrap());
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"a").unwrap(), Some(b"4".to_vec()));
    tx.abort().unwrap();
}

#[test]
fn scans_honor_bounds_direction_and_staged_writes() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    let table = db.open_table("scan", 1).unwrap();
    let mut seed = db.transaction().unwrap();
    for key in [b"a", b"b", b"c", b"d"] {
        seed.put(&table, key, key).unwrap();
    }
    seed.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"bb", b"staged").unwrap();
    tx.remove(&table, b"c").unwrap();
    let rows: Vec<_> = tx
        .scan(&table, b"b", Some(b"d"))
        .unwrap()
        .map(|row| row.unwrap())
        .collect();
    assert_eq!(
        rows,
        vec![
            (b"b".to_vec(), b"b".to_vec()),
            (b"bb".to_vec(), b"staged".to_vec())
        ]
    );
    let reverse: Vec<_> = tx
        .rscan(&table, b"", None)
        .unwrap()
        .map(|row| row.unwrap().0)
        .collect();
    assert_eq!(
        reverse,
        vec![b"d".to_vec(), b"bb".to_vec(), b"b".to_vec(), b"a".to_vec()]
    );
    let empty: Vec<_> = tx.scan(&table, b"x", Some(b"a")).unwrap().collect();
    assert!(empty.is_empty());
    tx.commit().unwrap();
}

#[test]
fn optimistic_conflicts_abort_the_loser() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    let table = db.open_table("occ", 1).unwrap();
    let mut seed = db.transaction().unwrap();
    seed.put(&table, b"k", b"0").unwrap();
    seed.commit().unwrap();

    let mut reader = db.transaction().unwrap();
    assert_eq!(reader.get(&table, b"k").unwrap(), Some(b"0".to_vec()));
    let mut reader_writes = Vec::new();
    std::thread::scope(|scope| {
        scope.spawn(|| {
            let mut writer = db.transaction().unwrap();
            writer.put(&table, b"k", b"1").unwrap();
            writer.commit().unwrap();
            reader_writes.push(());
        });
    });
    reader.put(&table, b"k", b"2").unwrap();
    assert_eq!(reader.commit(), Err(Error::Conflict));

    let mut check = db.transaction().unwrap();
    assert_eq!(check.get(&table, b"k").unwrap(), Some(b"1".to_vec()));
    check.commit().unwrap();
}

#[test]
fn table_identity_is_stable_per_database() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    let other = LocalDb::open().unwrap();
    let table = db.open_table("t", 1).unwrap();
    assert_eq!(table.id(), 1);
    assert_eq!(db.open_table("t", 1).unwrap().id(), 1);
    assert_eq!(
        db.open_table("t", 2).err(),
        Some(Error::WrongDatabaseOrTable)
    );
    assert_eq!(
        db.open_table("u", 1).err(),
        Some(Error::WrongDatabaseOrTable)
    );
    assert_eq!(
        db.open_table(vec![b'x'; 1025], 3).err(),
        Some(Error::ValueTooLarge)
    );
    let foreign = other.open_table("t", 1).unwrap();
    let mut tx = db.transaction().unwrap();
    assert_eq!(
        tx.get(&foreign, b"k").err(),
        Some(Error::WrongDatabaseOrTable)
    );
    assert_eq!(
        tx.put(&table, &[0u8; 1025], b"v").err(),
        Some(Error::ValueTooLarge)
    );
    tx.commit().unwrap();
}

#[test]
fn general_record_terminal_serializes_the_canonical_write_set() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    db.bind_trusted_table("records", 9).unwrap();
    let table = db.open_table("records", 9).unwrap();
    let mut seed = db.transaction().unwrap();
    seed.put(&table, b"old", b"o").unwrap();
    seed.commit().unwrap();
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();

    assert_eq!(tx.get(&table, b"zzz").unwrap(), None);
    tx.put(&table, b"b", b"2").unwrap();
    tx.put(&table, b"a", b"1").unwrap();
    tx.remove(&table, b"old").unwrap();
    tx.insert(&table, b"gone", b"x").unwrap();
    tx.remove(&table, b"gone").unwrap();
    tx.put(&table, b"b", b"22").unwrap();
    assert!(tx.unchecked_one_put_record_candidate().is_none());
    let preflight = tx
        .commit_record_preflight_with_checksum(1 << 20, CommitRecordChecksum::Crc32c)
        .unwrap();
    assert_eq!(preflight.op_count(), 3);
    let expected = record_format::HEADER_BYTES
        + record_format::CRC_BYTES
        + 3 * record_format::OP_HEADER_BYTES
        + (1 + 2)
        + (1 + 1)
        + 3;
    assert_eq!(preflight.exact_record_bytes(), expected);
    assert_eq!(tx.put(&table, b"late", b"x").err(), Some(Error::Busy));

    let mut buffer = vec![0u8; preflight.exact_record_bytes()];
    let mut seen = None;
    let report = unsafe {
        tx.commit_report_with_record_target(|timestamp, plan| {
            assert_eq!(plan, preflight);
            seen = Some(timestamp);
            Some(CommitRecordTarget::from_raw_parts(
                NonZeroU64::new(7).unwrap(),
                NonNull::new(buffer.as_mut_ptr()).unwrap(),
                buffer.len(),
            ))
        })
    };
    assert!(report.completion_contract_valid);
    assert!(report.record_bound);
    assert!(report.record_written);
    assert_eq!(report.commit.disposition, CommitDisposition::Committed);
    assert_eq!(report.commit.cleanup, Ok(()));
    let (version, sequence, timestamp, op_count) = parse_header(&buffer);
    assert_eq!(&buffer[..8], b"MAKOCMT\0");
    assert_eq!(version, 5);
    assert_eq!(sequence, 7);
    assert_eq!(Some(timestamp), seen);
    assert_eq!(op_count, 3);
    let ops = parse_ops(&buffer);
    assert_eq!(
        ops,
        vec![
            (record_format::TAG_PUT, 9, b"b".to_vec(), b"22".to_vec()),
            (record_format::TAG_PUT, 9, b"a".to_vec(), b"1".to_vec()),
            (record_format::TAG_DELETE, 9, b"old".to_vec(), Vec::new()),
        ]
    );

    let mut check = db.transaction().unwrap();
    assert_eq!(check.get(&table, b"b").unwrap(), Some(b"22".to_vec()));
    assert_eq!(check.get(&table, b"old").unwrap(), None);
    assert_eq!(check.get(&table, b"gone").unwrap(), None);
    check.commit().unwrap();
}

#[test]
fn one_put_candidate_tracks_the_direct_witness() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    db.bind_trusted_table("one", 3).unwrap();
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    assert!(tx.unchecked_one_put_record_candidate().is_none());
    tx.put(&table, b"key", b"value").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    assert_eq!(
        candidate.exact_record_bytes(),
        record_format::UNCHECKED_ONE_PUT_OVERHEAD_BYTES + 3 + 5
    );
    assert_eq!(candidate.op_count(), 1);
    assert_eq!(candidate.checksum(), CommitRecordChecksum::None);
    // A later read retires the direct witness.
    tx.get(&table, b"key").unwrap();
    assert!(tx.unchecked_one_put_record_candidate().is_none());
    let preflight = tx
        .commit_record_preflight_with_checksum(1 << 20, CommitRecordChecksum::None)
        .unwrap();
    assert_eq!(preflight.op_count(), 1);
    assert_eq!(
        preflight.exact_record_bytes(),
        candidate.exact_record_bytes()
    );
    tx.abort().unwrap();

    // A candidate taken before a read is stale: the trusted one-put terminal
    // refuses it before acquiring storage instead of serializing a one-put
    // record for a transaction that also read.
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"key", b"value").unwrap();
    let stale = tx.unchecked_one_put_record_candidate().unwrap();
    tx.get(&table, b"key").unwrap();
    let mut acquired = false;
    let report = unsafe {
        tx.commit_report_with_unchecked_one_put_record_target(stale, |_timestamp, _plan| {
            acquired = true;
            None
        })
    };
    assert!(!acquired, "a stale one-put candidate reached acquire");
    assert!(report.completion_contract_valid);
    assert!(!report.record_bound && !report.record_written);
    assert_ne!(report.commit.disposition, CommitDisposition::Committed);

    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"key", b"value").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    let mut buffer = vec![0u8; candidate.exact_record_bytes()];
    let report = unsafe {
        tx.commit_report_with_unchecked_one_put_record_target(candidate, |_timestamp, plan| {
            assert_eq!(plan, candidate);
            Some(CommitRecordTarget::from_raw_parts(
                NonZeroU64::new(11).unwrap(),
                NonNull::new(buffer.as_mut_ptr()).unwrap(),
                buffer.len(),
            ))
        })
    };
    assert!(report.completion_contract_valid && report.record_written);
    assert_eq!(report.commit.disposition, CommitDisposition::Committed);
    assert_eq!(&buffer[..8], b"MAKONOC\0");
    let (version, sequence, _, op_count) = parse_header(&buffer);
    assert_eq!((version, sequence, op_count), (6, 11, 1));
    assert_eq!(
        parse_ops(&buffer),
        vec![(
            record_format::TAG_PUT,
            3,
            b"key".to_vec(),
            b"value".to_vec()
        )]
    );

    // A stale candidate is a definite pre-acceptance abort.
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"key", b"other").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    tx.put(&table, b"second", b"write").unwrap();
    let report = unsafe {
        tx.commit_report_with_unchecked_one_put_record_target(candidate, |_, _| {
            panic!("a stale candidate must not bind")
        })
    };
    assert!(report.completion_contract_valid);
    assert!(!report.record_bound);
    assert_eq!(
        report.commit.disposition,
        CommitDisposition::Aborted(Error::InvalidArgument)
    );
}

#[test]
fn single_producer_preselected_record_and_holder_terminals() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    unsafe { db.claim_cache_order_namespace(CacheOrderMode::SingleProducer) }.unwrap();
    db.bind_trusted_table("spsc", 5).unwrap();

    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"k1", b"v1").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    let mut buffer = vec![0u8; candidate.exact_record_bytes()];
    let target = unsafe {
        CommitRecordTarget::from_raw_parts(
            NonZeroU64::new(1).unwrap(),
            NonNull::new(buffer.as_mut_ptr()).unwrap(),
            buffer.len(),
        )
    };
    let outcome = unsafe {
        tx.commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
            candidate, target,
        )
    };
    assert!(outcome.is_committed());
    let first = outcome.accepted_timestamp().unwrap();
    assert!(outcome.record_written());
    let report = outcome.into_report();
    assert!(report.completion_contract_valid && report.record_bound && report.record_written);
    let (_, sequence, timestamp, _) = parse_header(&buffer);
    assert_eq!(sequence, 1);
    assert_eq!(timestamp, first);

    let pool = TrustedOnePutHolderPool::new(8, 0, 0).unwrap();
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"k2", b"v2").unwrap();
    let exact = unsafe { tx.trusted_unchecked_one_put_record_bytes_candidate() }.unwrap();
    let outcome = unsafe {
        tx.commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
            exact,
            &pool,
            NonZeroU64::new(2).unwrap(),
        )
    };
    let second = outcome.committed_timestamp().unwrap();
    assert!(second > first);
    let view = unsafe { pool.view(NonZeroU64::new(2).unwrap()) }.unwrap();
    assert_eq!(view.table_id(), 5);
    assert_eq!(view.key(), b"k2");
    assert_eq!(view.value(), b"v2");
    assert_eq!(view.mako_timestamp(), second);
    assert!(matches!(
        unsafe { pool.view(NonZeroU64::new(3).unwrap()) },
        Err(Error::Busy)
    ));
    unsafe { view.release_after_backend() }.unwrap();
    assert!(matches!(
        unsafe { pool.view(NonZeroU64::new(2).unwrap()) },
        Err(Error::Busy)
    ));

    let mut check = db.transaction().unwrap();
    assert_eq!(check.get(&table, b"k1").unwrap(), Some(b"v1".to_vec()));
    assert_eq!(check.get(&table, b"k2").unwrap(), Some(b"v2".to_vec()));
    check.commit().unwrap();
}

#[test]
fn fused_single_producer_terminal_publishes_ack_itself() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    unsafe { db.claim_cache_order_namespace(CacheOrderMode::SingleProducer) }.unwrap();
    db.bind_trusted_table("fused", 6).unwrap();
    let pool = TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
    let acknowledged = AtomicU64::new(0);
    let unhealthy = AtomicBool::new(false);
    let control = unsafe {
        TrustedSpscOnePutHolderControl::new(
            &pool,
            acknowledged.as_ptr(),
            unhealthy.as_ptr().cast::<u8>(),
            NonZeroU64::new(4).unwrap(),
            NonZeroU32::new(1024).unwrap(),
        )
    };
    let producer_next = AtomicU64::new(0);
    let capacity_limit = AtomicU64::new(4);

    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"hot", b"path").unwrap();
    let mut cold = std::mem::MaybeUninit::uninit();
    let published = unsafe {
        tx.try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
            &control,
            producer_next.as_ptr(),
            capacity_limit.as_ptr().cast_const(),
            &mut cold,
        )
    };
    assert!(published);
    std::mem::forget(tx);
    assert_eq!(acknowledged.load(Ordering::Acquire), 1);
    let view = unsafe { pool.view(NonZeroU64::new(1).unwrap()) }.unwrap();
    assert_eq!(view.key(), b"hot");
    assert_eq!(view.value(), b"path");
    unsafe { view.release_after_backend() }.unwrap();

    // A read retires the candidate: the terminal leaves the transaction
    // active and asks for the general path.
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"hot", b"again").unwrap();
    tx.get(&table, b"hot").unwrap();
    let mut cold = std::mem::MaybeUninit::uninit();
    let published = unsafe {
        tx.try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
            &control,
            producer_next.as_ptr(),
            capacity_limit.as_ptr().cast_const(),
            &mut cold,
        )
    };
    assert!(!published);
    assert!(matches!(
        unsafe { cold.assume_init() },
        TrustedFusedOnePutHolderAttempt::UntouchedGeneral
    ));
    tx.commit().unwrap();

    // Capacity exhaustion is the slow untouched result.
    capacity_limit.store(1, Ordering::Relaxed);
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"hot", b"full").unwrap();
    let mut cold = std::mem::MaybeUninit::uninit();
    let published = unsafe {
        tx.try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
            &control,
            producer_next.as_ptr(),
            capacity_limit.as_ptr().cast_const(),
            &mut cold,
        )
    };
    assert!(!published);
    assert!(matches!(
        unsafe { cold.assume_init() },
        TrustedFusedOnePutHolderAttempt::UntouchedSlow { .. }
    ));
    tx.abort().unwrap();
}

#[repr(C, align(64))]
struct Cell {
    turn: AtomicU64,
    timestamp_stamp: UnsafeCell<u64>,
    record_bytes: UnsafeCell<usize>,
    _padding: [u8; 40],
}

#[repr(C, align(64))]
struct Block(UnsafeCell<[u8; 256]>);

#[test]
fn concurrent_arena_and_holder_terminals_drive_the_publication_ring() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    unsafe { db.claim_cache_order_namespace(CacheOrderMode::Concurrent) }.unwrap();
    unsafe { db.reseed_cache_order_namespace(0) }.unwrap();
    db.bind_trusted_table("ring", 8).unwrap();
    const CAPACITY: usize = 4;
    const SHIFT: u32 = 2;
    let cells: Vec<Cell> = (0..CAPACITY)
        .map(|_| Cell {
            turn: AtomicU64::new(0),
            timestamp_stamp: UnsafeCell::new(0),
            record_bytes: UnsafeCell::new(0),
            _padding: [0; 40],
        })
        .collect();
    let blocks: Vec<Block> = (0..CAPACITY)
        .map(|_| Block(UnsafeCell::new([0; 256])))
        .collect();
    let next_bound = AtomicU64::new(0);
    let unhealthy = AtomicBool::new(false);
    let pool = TrustedOnePutHolderPool::new(CAPACITY, 0, 0).unwrap();

    let arena_control = unsafe {
        TrustedNativeOrderedArenaControl::from_raw_parts(
            next_bound.as_ptr(),
            unhealthy.as_ptr().cast::<u8>(),
            cells.as_ptr().cast_mut().cast::<u8>(),
            blocks.as_ptr().cast_mut().cast::<u8>(),
            CAPACITY - 1,
            SHIFT,
            64,
            256,
            256,
        )
    };
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"ka", b"va").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    let outcome = unsafe {
        tx.commit_trusted_native_ordered_unchecked_one_put_arena(candidate, &arena_control)
    };
    assert!(outcome.is_committed(), "{:?}", outcome.into_report());
    let (timestamp, sequence) = outcome.accepted_order().unwrap();
    assert_eq!(sequence.get(), 1);
    assert_eq!(
        cells[1].turn.load(Ordering::Acquire),
        1,
        "BOUND turn of sequence 1"
    );
    let record = unsafe { &*blocks[1].0.get() };
    let (version, header_sequence, header_timestamp, _) = parse_header(record);
    assert_eq!((version, header_sequence), (6, 1));
    assert_eq!(header_timestamp, timestamp);
    assert_eq!(
        parse_ops(&record[..candidate.exact_record_bytes()]),
        vec![(record_format::TAG_PUT, 8, b"ka".to_vec(), b"va".to_vec())]
    );

    let holder_control = unsafe {
        TrustedNativeOrderedHolderControl::new(
            &pool,
            unhealthy.as_ptr().cast::<u8>(),
            cells.as_ptr().cast_mut().cast::<u8>(),
            CAPACITY - 1,
            SHIFT,
            64,
            1024,
        )
    };
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"kb", b"vb").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    let outcome = unsafe {
        tx.commit_trusted_native_ordered_unchecked_one_put_holder(candidate, &holder_control)
    };
    assert!(outcome.is_committed(), "{:?}", outcome.into_report());
    assert!(outcome.native_holder_ready());
    let (holder_timestamp, sequence) = outcome.accepted_order().unwrap();
    assert!(holder_timestamp > timestamp);
    assert_eq!(sequence.get(), 2);
    assert_eq!(
        cells[2].turn.load(Ordering::Acquire),
        3,
        "READY turn of sequence 2"
    );
    assert_eq!(
        unsafe { *cells[2].record_bytes.get() },
        (1usize << (usize::BITS - 1)) | candidate.exact_record_bytes()
    );
    assert_eq!(
        unsafe { *cells[2].timestamp_stamp.get() },
        holder_timestamp.hot_stamp().unwrap()
    );
    let view = unsafe { pool.view(sequence) }.unwrap();
    assert_eq!(view.key(), b"kb");
    assert_eq!(view.value(), b"vb");
    assert_eq!(view.mako_timestamp(), holder_timestamp);
    unsafe { view.release_after_backend() }.unwrap();

    // An unhealthy queue rejects before any order is assigned.
    unhealthy.store(true, Ordering::Release);
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"kc", b"vc").unwrap();
    let candidate = tx.unchecked_one_put_record_candidate().unwrap();
    let outcome = unsafe {
        tx.commit_trusted_native_ordered_unchecked_one_put_arena(candidate, &arena_control)
    };
    assert!(!outcome.is_committed());
    assert!(outcome.accepted_order().is_none());
    let report = outcome.into_report();
    assert!(report.completion_contract_valid);
    assert_eq!(
        report.commit.disposition,
        CommitDisposition::Aborted(Error::CommitHookRejected)
    );
    assert_eq!(db.cache_order_snapshot(), 2);

    let mut check = db.transaction().unwrap();
    assert_eq!(check.get(&table, b"ka").unwrap(), Some(b"va".to_vec()));
    assert_eq!(check.get(&table, b"kb").unwrap(), Some(b"vb".to_vec()));
    assert_eq!(check.get(&table, b"kc").unwrap(), None);
    check.commit().unwrap();
}

#[test]
fn per_worker_holder_terminal_uses_the_restricted_and_general_profiles() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    unsafe { db.claim_cache_order_namespace(CacheOrderMode::PerWorker) }.unwrap();
    db.bind_trusted_table("lane", 4).unwrap();
    let pool = TrustedOnePutHolderPool::new(8, 0, 0).unwrap();
    let acknowledged = AtomicU64::new(0);
    let unhealthy = AtomicBool::new(false);
    let control = unsafe {
        TrustedSpscOnePutHolderControl::new(
            &pool,
            acknowledged.as_ptr(),
            unhealthy.as_ptr().cast::<u8>(),
            NonZeroU64::new(8).unwrap(),
            NonZeroU32::new(1024).unwrap(),
        )
    };

    // First write of the key: an insert, so the general (gated) profile.
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"k", b"first").unwrap();
    let exact = unsafe { tx.trusted_unchecked_one_put_record_bytes_candidate() }.unwrap();
    let outcome = unsafe {
        tx.commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
            exact,
            &control,
            NonZeroU64::new(1).unwrap(),
        )
    };
    let first = outcome.committed_timestamp().unwrap();

    // An update of an existing key takes the restricted profile.
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    assert!(!tx.put(&table, b"k", b"second").unwrap());
    let exact = unsafe { tx.trusted_unchecked_one_put_record_bytes_candidate() }.unwrap();
    let outcome = unsafe {
        tx.commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
            exact,
            &control,
            NonZeroU64::new(2).unwrap(),
        )
    };
    let second = outcome.committed_timestamp().unwrap();
    assert!(second > first);
    let view = unsafe { pool.view(NonZeroU64::new(2).unwrap()) }.unwrap();
    assert_eq!(view.value(), b"second");
    unsafe { view.release_after_backend() }.unwrap();
    let view = unsafe { pool.view(NonZeroU64::new(1).unwrap()) }.unwrap();
    assert_eq!(view.value(), b"first");
    unsafe { view.release_after_backend() }.unwrap();

    // A second claim in the process is refused while this one is live.
    let other = LocalDb::open().unwrap();
    assert_eq!(
        unsafe { other.claim_cache_order_namespace(CacheOrderMode::Concurrent) },
        Err(Error::Busy)
    );
    db.order_record_validation_prefix();
}

#[test]
fn item_budget_bounds_a_transaction() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    let table = db.open_table("budget", 12).unwrap();
    // A read costs one credit; the budget is 512 credits, exactly as in the
    // C++ engine. The transaction is aborted at the first excess operation.
    let mut tx = db.transaction().unwrap();
    for ordinal in 0..mako_engine_sto::TRANSACTION_ITEM_BUDGET {
        assert_eq!(
            tx.get(&table, format!("k{ordinal}").as_bytes()).unwrap(),
            None
        );
    }
    assert_eq!(
        tx.get(&table, b"one-too-many").err(),
        Some(Error::TransactionTooLarge)
    );
    assert_eq!(
        tx.get(&table, b"after").err(),
        Some(Error::TransactionFinished)
    );
    assert_eq!(
        tx.commit_report().disposition,
        CommitDisposition::Aborted(Error::TransactionFinished)
    );

    // A write costs four credits plus one per eight key bytes.
    let mut tx = db.transaction().unwrap();
    let key = [b'k'; 16];
    for ordinal in 0..(mako_engine_sto::TRANSACTION_ITEM_BUDGET / 6) {
        let mut key = key;
        key[..2].copy_from_slice(&(ordinal as u16).to_be_bytes());
        tx.put(&table, &key, b"v").unwrap();
    }
    assert_eq!(
        tx.put(&table, &key, b"v").err(),
        Some(Error::TransactionTooLarge)
    );
}

#[test]
fn commit_report_separates_visibility_from_a_sealed_plan() {
    let _serial = serial();
    let db = LocalDb::open().unwrap();
    db.bind_trusted_table("plan", 2).unwrap();
    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"k", b"v").unwrap();
    tx.commit_record_preflight_with_checksum(1 << 20, CommitRecordChecksum::Crc32c)
        .unwrap();
    let report = tx.commit_report();
    assert_eq!(
        report.disposition,
        CommitDisposition::Aborted(Error::InvalidArgument)
    );
    assert_eq!(report.cleanup, Ok(()));

    let (table, mut tx) = db.trusted_bound_transaction().unwrap();
    tx.put(&table, b"k", b"v").unwrap();
    assert_eq!(
        tx.commit_record_preflight_with_checksum(10, CommitRecordChecksum::Crc32c),
        Err(Error::ValueTooLarge)
    );
    assert_eq!(tx.get(&table, b"k").err(), Some(Error::TransactionFinished));
    let report = tx.commit_report();
    assert_eq!(
        report.disposition,
        CommitDisposition::Aborted(Error::TransactionFinished)
    );

    let mut check = db.transaction().unwrap();
    assert_eq!(check.get(&table, b"k").unwrap(), None);
    check.commit().unwrap();
}
