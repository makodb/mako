//! Forced concurrent histories checked by `mako-history`'s strict
//! serializability (and, when advertised, opacity) oracle.
//!
//! The three scenarios stage an anti-dependency, a write-write race, and a
//! phantom across two workers, force the losing order with channels, and
//! feed the recorded history plus the observed final state to the checker.
//! They are generic over the engine so both the C ABI facade and the native
//! Rust STO engine run the same oracle.

use std::sync::{mpsc, Arc};
use std::time::Duration;

use mako_engine_api::{EngineDb, EngineTransaction, Error, Features};
use mako_history::{
    check_opacity, check_strict_serializability, state_insert, CheckOptions, History, Interval,
    LogicalClock, Observation, Operation, Row, ScanDirection, State, StateKey, TerminalCall,
    TerminalOutcome, TimedOperation, Transaction as HistoryTransaction,
};

const CHANNEL_TIMEOUT: Duration = Duration::from_secs(10);

fn begin_recorded<'db, D: EngineDb>(
    db: &'db D,
    clock: &LogicalClock,
    id: u32,
) -> (D::Transaction<'db>, HistoryTransaction) {
    let invocation = clock.next();
    let transaction = db.transaction().expect("begin engine transaction");
    let response = clock.next();
    (
        transaction,
        HistoryTransaction::new(id, Interval::completed(invocation, response)),
    )
}

fn record_get<'db, T: EngineTransaction<'db>>(
    transaction: &mut T,
    history: &mut HistoryTransaction,
    table: &T::Table,
    table_id: u64,
    key: &[u8],
    clock: &LogicalClock,
) -> Option<Vec<u8>> {
    let invocation = clock.next();
    let value = transaction.get(table, key).expect("engine get");
    let response = clock.next();
    history.push(TimedOperation::completed(
        invocation,
        response,
        Operation::get(table_id, key),
        Observation::Get(value.clone()),
    ));
    value
}

fn record_put<'db, T: EngineTransaction<'db>>(
    transaction: &mut T,
    history: &mut HistoryTransaction,
    table: &T::Table,
    table_id: u64,
    key: &[u8],
    value: &[u8],
    clock: &LogicalClock,
) -> bool {
    let invocation = clock.next();
    let created = transaction.put(table, key, value).expect("engine put");
    let response = clock.next();
    history.push(TimedOperation::completed(
        invocation,
        response,
        Operation::put(table_id, key, value),
        Observation::Put { created },
    ));
    created
}

fn record_scan<'db, T: EngineTransaction<'db>>(
    transaction: &mut T,
    history: &mut HistoryTransaction,
    table: &T::Table,
    table_id: u64,
    lower: &[u8],
    upper: Option<&[u8]>,
    clock: &LogicalClock,
) -> Vec<(Vec<u8>, Vec<u8>)> {
    let invocation = clock.next();
    let rows = transaction
        .scan(table, lower, upper)
        .expect("engine scan")
        .collect::<Result<Vec<_>, _>>()
        .expect("engine scan rows");
    let response = clock.next();
    history.push(TimedOperation::completed(
        invocation,
        response,
        Operation::scan(
            table_id,
            lower,
            upper.map(<[u8]>::to_vec),
            ScanDirection::Forward,
        ),
        Observation::Scan(
            rows.iter()
                .map(|(key, value)| Row::new(key.clone(), value.clone()))
                .collect(),
        ),
    ));
    rows
}

fn record_commit<'db, T: EngineTransaction<'db>>(
    transaction: T,
    mut history: HistoryTransaction,
    clock: &LogicalClock,
) -> (HistoryTransaction, Result<(), Error>) {
    let invocation = clock.next();
    let result = transaction.commit();
    let response = clock.next();
    let outcome = match result {
        Ok(()) => TerminalOutcome::Committed,
        Err(Error::Conflict) => TerminalOutcome::Conflict,
        Err(error) => panic!("unexpected engine commit error: {error}"),
    };
    history.finish(TerminalCall::commit(invocation, response, outcome));
    (history, result)
}

fn seed<D: EngineDb>(db: &D, name: &[u8], table_id: u64, rows: &[(&[u8], &[u8])]) {
    let table = db.open_table(name, table_id).expect("seed table");
    let mut transaction = db.transaction().expect("seed transaction");
    for (key, value) in rows {
        transaction.put(&table, key, value).expect("seed put");
    }
    transaction.commit().expect("seed commit");
}

fn snapshot<D: EngineDb>(db: &D, name: &[u8], table_id: u64) -> State {
    let table = db.open_table(name, table_id).expect("snapshot table");
    let mut transaction = db.transaction().expect("snapshot transaction");
    let rows = transaction
        .scan(&table, b"", None)
        .expect("snapshot scan")
        .collect::<Result<Vec<_>, _>>()
        .expect("snapshot rows");
    transaction.commit().expect("snapshot commit");
    let mut state = State::new();
    for (key, value) in rows {
        state_insert(&mut state, table_id, key, value);
    }
    state
}

fn check_required_history(history: &History, opacity: bool) {
    check_strict_serializability(history, CheckOptions::default())
        .unwrap_or_else(|error| panic!("strict-serializability failure:\n{error}"));
    if opacity {
        check_opacity(history, CheckOptions::default())
            .unwrap_or_else(|error| panic!("opacity failure:\n{error}"));
    }
}

fn forced_rw_history<D: EngineDb + 'static>(opacity: bool) {
    const TABLE_ID: u64 = 24_100;
    const NAME: &[u8] = b"history/rw";
    let db = Arc::new(D::open().expect("open RW database"));
    seed(&*db, NAME, TABLE_ID, &[(b"x", b"0")]);
    let mut initial = State::new();
    state_insert(&mut initial, TABLE_ID, b"x", b"0");

    let clock = Arc::new(LogicalClock::default());
    let (staged_tx, staged_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);
    let reader_db = Arc::clone(&db);
    let reader_clock = Arc::clone(&clock);
    let reader = std::thread::spawn(move || {
        let table = reader_db.open_table(NAME, TABLE_ID).expect("reader table");
        let (mut transaction, mut history) = begin_recorded(&*reader_db, &reader_clock, 1);
        assert_eq!(
            record_get(
                &mut transaction,
                &mut history,
                &table,
                TABLE_ID,
                b"x",
                &reader_clock
            )
            .as_deref(),
            Some(&b"0"[..])
        );
        assert!(record_put(
            &mut transaction,
            &mut history,
            &table,
            TABLE_ID,
            b"reader-side",
            b"must-abort",
            &reader_clock,
        ));
        staged_tx.send(()).expect("announce staged reader");
        release_rx
            .recv_timeout(CHANNEL_TIMEOUT)
            .expect("release reader");
        record_commit(transaction, history, &reader_clock)
    });

    staged_rx
        .recv_timeout(CHANNEL_TIMEOUT)
        .expect("reader staged");
    let table = db.open_table(NAME, TABLE_ID).expect("writer table");
    let (mut writer, mut writer_history) = begin_recorded(&*db, &clock, 2);
    assert!(!record_put(
        &mut writer,
        &mut writer_history,
        &table,
        TABLE_ID,
        b"x",
        b"1",
        &clock,
    ));
    let (writer_history, writer_result) = record_commit(writer, writer_history, &clock);
    assert_eq!(writer_result, Ok(()));
    release_tx.send(()).expect("release reader commit");
    let (reader_history, reader_result) = reader.join().expect("join reader");
    assert_eq!(reader_result, Err(Error::Conflict));

    let mut history = History::new(initial);
    history
        .set_observed_final_state(snapshot(&*db, NAME, TABLE_ID))
        .push(reader_history)
        .push(writer_history);
    assert_eq!(
        history
            .observed_final_state
            .as_ref()
            .expect("final state")
            .get(&StateKey::new(TABLE_ID, b"reader-side")),
        None
    );
    check_required_history(&history, opacity);
}

fn forced_ww_history<D: EngineDb + 'static>(opacity: bool) {
    const TABLE_ID: u64 = 24_101;
    const NAME: &[u8] = b"history/ww";
    let db = Arc::new(D::open().expect("open WW database"));
    seed(&*db, NAME, TABLE_ID, &[(b"x", b"base")]);
    let mut initial = State::new();
    state_insert(&mut initial, TABLE_ID, b"x", b"base");

    let clock = Arc::new(LogicalClock::default());
    let (staged_tx, staged_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);
    let loser_db = Arc::clone(&db);
    let loser_clock = Arc::clone(&clock);
    let loser = std::thread::spawn(move || {
        let table = loser_db.open_table(NAME, TABLE_ID).expect("loser table");
        let (mut transaction, mut history) = begin_recorded(&*loser_db, &loser_clock, 2);
        assert!(!record_put(
            &mut transaction,
            &mut history,
            &table,
            TABLE_ID,
            b"x",
            b"loser",
            &loser_clock,
        ));
        staged_tx.send(()).expect("announce staged loser");
        release_rx
            .recv_timeout(CHANNEL_TIMEOUT)
            .expect("release loser");
        record_commit(transaction, history, &loser_clock)
    });

    staged_rx
        .recv_timeout(CHANNEL_TIMEOUT)
        .expect("loser staged");
    let table = db.open_table(NAME, TABLE_ID).expect("winner table");
    let (mut winner, mut winner_history) = begin_recorded(&*db, &clock, 1);
    assert!(!record_put(
        &mut winner,
        &mut winner_history,
        &table,
        TABLE_ID,
        b"x",
        b"winner",
        &clock,
    ));
    let (winner_history, winner_result) = record_commit(winner, winner_history, &clock);
    assert_eq!(winner_result, Ok(()));
    release_tx.send(()).expect("release losing commit");
    let (loser_history, loser_result) = loser.join().expect("join loser");
    assert_eq!(loser_result, Err(Error::Conflict));

    let mut history = History::new(initial);
    history
        .set_observed_final_state(snapshot(&*db, NAME, TABLE_ID))
        .push(winner_history)
        .push(loser_history);
    let final_state = history.observed_final_state.as_ref().expect("final state");
    assert_eq!(
        final_state
            .get(&StateKey::new(TABLE_ID, b"x"))
            .map(Vec::as_slice),
        Some(&b"winner"[..])
    );
    check_required_history(&history, opacity);
}

fn forced_phantom_history<D: EngineDb + 'static>(opacity: bool) {
    const TABLE_ID: u64 = 24_102;
    const NAME: &[u8] = b"history/phantom";
    let db = Arc::new(D::open().expect("open phantom database"));
    let _ = db.open_table(NAME, TABLE_ID).expect("open phantom table");
    let initial = State::new();

    let clock = Arc::new(LogicalClock::default());
    let (staged_tx, staged_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);
    let scanner_db = Arc::clone(&db);
    let scanner_clock = Arc::clone(&clock);
    let scanner = std::thread::spawn(move || {
        let table = scanner_db
            .open_table(NAME, TABLE_ID)
            .expect("scanner table");
        let (mut transaction, mut history) = begin_recorded(&*scanner_db, &scanner_clock, 1);
        assert!(record_put(
            &mut transaction,
            &mut history,
            &table,
            TABLE_ID,
            b"phantom/own",
            b"ryw",
            &scanner_clock,
        ));
        assert_eq!(
            record_scan(
                &mut transaction,
                &mut history,
                &table,
                TABLE_ID,
                b"phantom/",
                Some(b"phantom0"),
                &scanner_clock,
            ),
            vec![(b"phantom/own".to_vec(), b"ryw".to_vec())]
        );
        assert!(record_put(
            &mut transaction,
            &mut history,
            &table,
            TABLE_ID,
            b"scanner-side",
            b"must-abort",
            &scanner_clock,
        ));
        staged_tx.send(()).expect("announce staged scanner");
        release_rx
            .recv_timeout(CHANNEL_TIMEOUT)
            .expect("release scanner");
        record_commit(transaction, history, &scanner_clock)
    });

    staged_rx
        .recv_timeout(CHANNEL_TIMEOUT)
        .expect("scanner staged");
    let table = db.open_table(NAME, TABLE_ID).expect("inserter table");
    let (mut inserter, mut inserter_history) = begin_recorded(&*db, &clock, 2);
    assert!(record_put(
        &mut inserter,
        &mut inserter_history,
        &table,
        TABLE_ID,
        b"phantom/new",
        b"visible",
        &clock,
    ));
    let (inserter_history, inserter_result) = record_commit(inserter, inserter_history, &clock);
    assert_eq!(inserter_result, Ok(()));
    release_tx.send(()).expect("release scanner commit");
    let (scanner_history, scanner_result) = scanner.join().expect("join scanner");
    assert_eq!(scanner_result, Err(Error::Conflict));

    let mut history = History::new(initial);
    history
        .set_observed_final_state(snapshot(&*db, NAME, TABLE_ID))
        .push(scanner_history)
        .push(inserter_history);
    let final_state = history.observed_final_state.as_ref().expect("final state");
    assert!(final_state
        .get(&StateKey::new(TABLE_ID, b"scanner-side"))
        .is_none());
    assert!(final_state
        .get(&StateKey::new(TABLE_ID, b"phantom/own"))
        .is_none());
    assert_eq!(
        final_state
            .get(&StateKey::new(TABLE_ID, b"phantom/new"))
            .map(Vec::as_slice),
        Some(&b"visible"[..])
    );
    check_required_history(&history, opacity);
}

/// Run the forced anti-dependency, write-write, and phantom histories on
/// engine `D` and check them against the oracle for the advertised profile.
pub fn forced_histories_pass_the_advertised_profile<D: EngineDb + 'static>(capabilities: Features) {
    assert!(
        capabilities.point_transactions()
            && capabilities.read_my_writes()
            && capabilities.transactional_scans()
            && capabilities.scan_read_my_writes(),
        "the history oracle requires point, RYW, and transactional scan features; found 0x{:016x}",
        capabilities.bits()
    );
    forced_rw_history::<D>(capabilities.opacity());
    forced_ww_history::<D>(capabilities.opacity());
    forced_phantom_history::<D>(capabilities.opacity());
}
