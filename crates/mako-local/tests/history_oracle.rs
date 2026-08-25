#![cfg(have_mako)]

use std::sync::{mpsc, Arc};
use std::time::Duration;

use mako_history::{
    check_opacity, check_strict_serializability, state_insert, CheckOptions, History, Interval,
    LogicalClock, Observation, Operation, Row, ScanDirection, State, TerminalCall, TerminalOutcome,
    TimedOperation, Transaction as HistoryTransaction,
};
use mako_local::{features, Error, LocalDb, Table, Transaction as LocalTransaction};

const CHANNEL_TIMEOUT: Duration = Duration::from_secs(10);

fn begin_recorded<'db>(
    db: &'db LocalDb,
    clock: &LogicalClock,
    id: u32,
) -> (LocalTransaction<'db>, HistoryTransaction) {
    let invocation = clock.next();
    let transaction = db.transaction().expect("begin native transaction");
    let response = clock.next();
    (
        transaction,
        HistoryTransaction::new(id, Interval::completed(invocation, response)),
    )
}

fn record_get<'db>(
    transaction: &mut LocalTransaction<'db>,
    history: &mut HistoryTransaction,
    table: &Table<'db>,
    key: &[u8],
    clock: &LogicalClock,
) -> Option<Vec<u8>> {
    let invocation = clock.next();
    let value = transaction.get(table, key).expect("native get");
    let response = clock.next();
    history.push(TimedOperation::completed(
        invocation,
        response,
        Operation::get(table.id(), key),
        Observation::Get(value.clone()),
    ));
    value
}

fn record_put<'db>(
    transaction: &mut LocalTransaction<'db>,
    history: &mut HistoryTransaction,
    table: &Table<'db>,
    key: &[u8],
    value: &[u8],
    clock: &LogicalClock,
) -> bool {
    let invocation = clock.next();
    let created = transaction.put(table, key, value).expect("native put");
    let response = clock.next();
    history.push(TimedOperation::completed(
        invocation,
        response,
        Operation::put(table.id(), key, value),
        Observation::Put { created },
    ));
    created
}

fn record_scan<'db>(
    transaction: &mut LocalTransaction<'db>,
    history: &mut HistoryTransaction,
    table: &Table<'db>,
    lower: &[u8],
    upper: Option<&[u8]>,
    clock: &LogicalClock,
) -> Vec<(Vec<u8>, Vec<u8>)> {
    let invocation = clock.next();
    let rows = transaction
        .scan(table, lower, upper)
        .expect("native scan")
        .collect::<Result<Vec<_>, _>>()
        .expect("native scan rows");
    let response = clock.next();
    history.push(TimedOperation::completed(
        invocation,
        response,
        Operation::scan(
            table.id(),
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

fn record_commit(
    transaction: LocalTransaction<'_>,
    mut history: HistoryTransaction,
    clock: &LogicalClock,
) -> (HistoryTransaction, Result<(), Error>) {
    let invocation = clock.next();
    let result = transaction.commit();
    let response = clock.next();
    let outcome = match result {
        Ok(()) => TerminalOutcome::Committed,
        Err(Error::Conflict) => TerminalOutcome::Conflict,
        Err(error) => panic!("unexpected native commit error: {error}"),
    };
    history.finish(TerminalCall::commit(invocation, response, outcome));
    (history, result)
}

fn seed(table: &Table<'_>, db: &LocalDb, rows: &[(&[u8], &[u8])]) {
    let mut transaction = db.transaction().expect("seed transaction");
    for (key, value) in rows {
        transaction.put(table, key, value).expect("seed put");
    }
    transaction.commit().expect("seed commit");
}

fn snapshot(db: &LocalDb, table: &Table<'_>) -> State {
    let mut transaction = db.transaction().expect("snapshot transaction");
    let rows = transaction
        .scan(table, b"", None)
        .expect("snapshot scan")
        .collect::<Result<Vec<_>, _>>()
        .expect("snapshot rows");
    transaction.commit().expect("snapshot commit");
    let mut state = State::new();
    for (key, value) in rows {
        state_insert(&mut state, table.id(), key, value);
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

fn forced_rw_history(opacity: bool) {
    const TABLE_ID: u64 = 24_100;
    let db = Arc::new(LocalDb::open().expect("open RW database"));
    let table = db
        .open_table("history/rw", TABLE_ID)
        .expect("open RW table");
    seed(&table, &db, &[(b"x", b"0")]);
    let mut initial = State::new();
    state_insert(&mut initial, TABLE_ID, b"x", b"0");

    let clock = Arc::new(LogicalClock::default());
    let (staged_tx, staged_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);
    let reader_db = Arc::clone(&db);
    let reader_clock = Arc::clone(&clock);
    let reader = std::thread::spawn(move || {
        let table = reader_db
            .open_table("history/rw", TABLE_ID)
            .expect("reader table");
        let (mut transaction, mut history) = begin_recorded(&reader_db, &reader_clock, 1);
        assert_eq!(
            record_get(&mut transaction, &mut history, &table, b"x", &reader_clock,).as_deref(),
            Some(&b"0"[..])
        );
        assert!(record_put(
            &mut transaction,
            &mut history,
            &table,
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
    let (mut writer, mut writer_history) = begin_recorded(&db, &clock, 2);
    assert!(!record_put(
        &mut writer,
        &mut writer_history,
        &table,
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
        .set_observed_final_state(snapshot(&db, &table))
        .push(reader_history)
        .push(writer_history);
    assert_eq!(
        history
            .observed_final_state
            .as_ref()
            .expect("final state")
            .get(&mako_history::StateKey::new(TABLE_ID, b"reader-side")),
        None
    );
    check_required_history(&history, opacity);
}

fn forced_ww_history(opacity: bool) {
    const TABLE_ID: u64 = 24_101;
    let db = Arc::new(LocalDb::open().expect("open WW database"));
    let table = db
        .open_table("history/ww", TABLE_ID)
        .expect("open WW table");
    seed(&table, &db, &[(b"x", b"base")]);
    let mut initial = State::new();
    state_insert(&mut initial, TABLE_ID, b"x", b"base");

    let clock = Arc::new(LogicalClock::default());
    let (staged_tx, staged_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);
    let loser_db = Arc::clone(&db);
    let loser_clock = Arc::clone(&clock);
    let loser = std::thread::spawn(move || {
        let table = loser_db
            .open_table("history/ww", TABLE_ID)
            .expect("loser table");
        let (mut transaction, mut history) = begin_recorded(&loser_db, &loser_clock, 2);
        assert!(!record_put(
            &mut transaction,
            &mut history,
            &table,
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
    let (mut winner, mut winner_history) = begin_recorded(&db, &clock, 1);
    assert!(!record_put(
        &mut winner,
        &mut winner_history,
        &table,
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
        .set_observed_final_state(snapshot(&db, &table))
        .push(winner_history)
        .push(loser_history);
    let final_state = history.observed_final_state.as_ref().expect("final state");
    assert_eq!(
        final_state
            .get(&mako_history::StateKey::new(TABLE_ID, b"x"))
            .map(Vec::as_slice),
        Some(&b"winner"[..])
    );
    check_required_history(&history, opacity);
}

fn forced_phantom_history(opacity: bool) {
    const TABLE_ID: u64 = 24_102;
    let db = Arc::new(LocalDb::open().expect("open phantom database"));
    let table = db
        .open_table("history/phantom", TABLE_ID)
        .expect("open phantom table");
    let initial = State::new();

    let clock = Arc::new(LogicalClock::default());
    let (staged_tx, staged_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);
    let scanner_db = Arc::clone(&db);
    let scanner_clock = Arc::clone(&clock);
    let scanner = std::thread::spawn(move || {
        let table = scanner_db
            .open_table("history/phantom", TABLE_ID)
            .expect("scanner table");
        let (mut transaction, mut history) = begin_recorded(&scanner_db, &scanner_clock, 1);
        assert!(record_put(
            &mut transaction,
            &mut history,
            &table,
            b"phantom/own",
            b"ryw",
            &scanner_clock,
        ));
        assert_eq!(
            record_scan(
                &mut transaction,
                &mut history,
                &table,
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
    let (mut inserter, mut inserter_history) = begin_recorded(&db, &clock, 2);
    assert!(record_put(
        &mut inserter,
        &mut inserter_history,
        &table,
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
        .set_observed_final_state(snapshot(&db, &table))
        .push(scanner_history)
        .push(inserter_history);
    let final_state = history.observed_final_state.as_ref().expect("final state");
    assert!(final_state
        .get(&mako_history::StateKey::new(TABLE_ID, b"scanner-side"))
        .is_none());
    assert!(final_state
        .get(&mako_history::StateKey::new(TABLE_ID, b"phantom/own"))
        .is_none());
    assert_eq!(
        final_state
            .get(&mako_history::StateKey::new(TABLE_ID, b"phantom/new"))
            .map(Vec::as_slice),
        Some(&b"visible"[..])
    );
    check_required_history(&history, opacity);
}

#[test]
fn forced_rw_ww_and_phantom_histories_pass_the_advertised_profile() {
    let capabilities = features().expect("query native features");
    assert!(
        capabilities.point_transactions()
            && capabilities.read_my_writes()
            && capabilities.transactional_scans()
            && capabilities.scan_read_my_writes(),
        "Item 3 requires point, RYW, and transactional scan features; found 0x{:016x}",
        capabilities.bits()
    );
    forced_rw_history(capabilities.opacity());
    forced_ww_history(capabilities.opacity());
    forced_phantom_history(capabilities.opacity());
}
