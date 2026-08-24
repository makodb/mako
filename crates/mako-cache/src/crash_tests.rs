//! Deterministic process-crash checks for the Rust/native/Rocks commit seam.

use std::env;
use std::fs;
use std::io::ErrorKind;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use mrx_core::Blobs;
use mrx_rocks::{RocksBlobs, WriteBatchHookPoint};

use crate::failpoint::{self, Point};
use crate::record::{classify_backend_key, BackendKey, CommitRecord};
use crate::{Db, Durability, Options};

const ROLE_ENV: &str = "MAKO_CACHE_CRASH_TEST_ROLE";
const DB_PATH_ENV: &str = "MAKO_CACHE_CRASH_TEST_DB";
const MARKER_PATH_ENV: &str = "MAKO_CACHE_CRASH_TEST_MARKER";
const POINT_ENV: &str = "MAKO_CACHE_CRASH_TEST_POINT";
const EXPECTED_ENV: &str = "MAKO_CACHE_CRASH_TEST_EXPECTED";
const REQUIRE_NATIVE_HOOKS_ENV: &str = "MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS";

const REPLACE_KEY: &[u8] = b"crash/replace";
const DELETE_KEY: &[u8] = b"crash/delete";
const NEW_KEY: &[u8] = b"crash/new";
const OLD_VALUE: &[u8] = b"old";
const NEW_VALUE: &[u8] = b"new";
const INSERTED_VALUE: &[u8] = b"inserted";

const RECOVERY_A: &[u8] = b"recovery/a";
const RECOVERY_B: &[u8] = b"recovery/b";
const RECOVERY_C: &[u8] = b"recovery/c";
const RECOVERY_D: &[u8] = b"recovery/d";
const RECOVERY_EMPTY: &[u8] = b"recovery/empty";
const RECOVERY_POST_RESTART: &[u8] = b"recovery/post-restart";
const RECOVERY_TOMBSTONE: &[u8] = b"recovery/tombstone";
const RECOVERY_RECORDS: u64 = 4;
const RECOVERY_TIMESTAMP_FLOOR: u32 = 1 << 24;

const CHILD_TIMEOUT: Duration = Duration::from_secs(30);
const CHILD_WATCHDOG: Duration = Duration::from_secs(60);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Expected {
    Old,
    New,
}

impl Expected {
    const fn name(self) -> &'static str {
        match self {
            Self::Old => "old",
            Self::New => "new",
        }
    }

    fn from_name(name: &str) -> Option<Self> {
        match name {
            "old" => Some(Self::Old),
            "new" => Some(Self::New),
            _ => None,
        }
    }
}

const CASES: &[(Point, Expected)] = &[
    (Point::DetachedPrepared, Expected::Old),
    (Point::NativeWritesetLocked, Expected::Old),
    (Point::NativeTimestampAllocated, Expected::Old),
    (Point::NativeValidationComplete, Expected::Old),
    (Point::PreinstallBound, Expected::Old),
    (Point::NativePreinstallAccepted, Expected::Old),
    (Point::NativeFirstWriteInstalled, Expected::Old),
    (Point::NativeAllWritesInstalled, Expected::Old),
    (Point::NativeCommittedBeforeReady, Expected::Old),
    (Point::ReadyBeforeBackend, Expected::Old),
    (Point::RocksBatchConstructed, Expected::Old),
    (Point::RocksBeforeWrite, Expected::Old),
    (Point::RocksAfterWrite, Expected::New),
    (Point::BackendWrittenBeforeDurable, Expected::New),
    (Point::DurableAdvanced, Expected::New),
];

const RECOVERY_CASES: &[Point] = &[
    Point::RecoveryKeysEnumerated,
    Point::RecoveryFirstRecordValidated,
    Point::RecoveryLastRecordValidated,
    Point::RecoveryMaterializedValidated,
    Point::RecoveryBeforeClockFloor,
    Point::RecoveryAfterClockFloor,
    Point::RecoveryMidReplay,
    Point::RecoveryReplayComplete,
];

struct Scratch {
    root: PathBuf,
    db: PathBuf,
    marker: PathBuf,
}

impl Scratch {
    fn new(point: Point) -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);

        let mut root = env::temp_dir();
        root.push(format!(
            "mako-cache-crash-{}-{}-{}",
            point.name(),
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create crash-test directory");

        Self {
            db: root.join("rocks"),
            marker: root.join("phase-reached"),
            root,
        }
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

struct ChildGuard(Option<Child>);

impl ChildGuard {
    fn new(child: Child) -> Self {
        Self(Some(child))
    }

    fn child_mut(&mut self) -> &mut Child {
        self.0.as_mut().expect("child has not been reaped")
    }

    fn kill_and_wait(mut self) -> ExitStatus {
        let mut child = self.0.take().expect("child has not been reaped");
        child.kill().expect("send SIGKILL to crash writer");
        child.wait().expect("reap crash writer")
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        if let Some(mut child) = self.0.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn sync_options() -> Options {
    Options {
        durability: Durability::Sync,
        ..Options::default()
    }
}

fn open_observed_writer(path: &Path) -> Db {
    let options = sync_options();
    let mut backend =
        RocksBlobs::open(path, options.durability).expect("open observed RocksDB backend");
    backend.set_write_batch_observer(|phase| {
        let point = match phase {
            WriteBatchHookPoint::BatchConstructed => Point::RocksBatchConstructed,
            WriteBatchHookPoint::BeforeWrite => Point::RocksBeforeWrite,
            WriteBatchHookPoint::AfterWrite => Point::RocksAfterWrite,
        };
        failpoint::hit(point);
    });
    Db::from_backend(backend, options.cache).expect("open observed writer cache")
}

fn observe_native_commit(phase: mako_local::TestCommitPhase, _mako_timestamp: u32) {
    let point = match phase {
        mako_local::TestCommitPhase::WritesetLocked => Point::NativeWritesetLocked,
        mako_local::TestCommitPhase::MakoTimestampAllocated => Point::NativeTimestampAllocated,
        mako_local::TestCommitPhase::LocalValidationComplete => Point::NativeValidationComplete,
        mako_local::TestCommitPhase::PreinstallAccepted => Point::NativePreinstallAccepted,
        mako_local::TestCommitPhase::FirstWriteInstalled => Point::NativeFirstWriteInstalled,
        mako_local::TestCommitPhase::AllWritesInstalled => Point::NativeAllWritesInstalled,
    };
    // This callback intentionally rendezvous-parks while native write locks
    // are held. The parent always SIGKILLs the fresh helper process; it never
    // resumes into the critical section.
    failpoint::hit(point);
}

fn install_native_observer(point: Point) {
    if !point.requires_native_observer() {
        return;
    }
    assert!(
        mako_local::features()
            .expect("read native crash-hook capability")
            .test_commit_observer(),
        "native crash case requires MAKO_LOCAL_TEST_HOOKS"
    );
    mako_local::install_test_commit_observer(observe_native_commit)
        .expect("install native commit observer");
}

fn clear_native_observer(point: Point) {
    if !point.requires_native_observer() {
        return;
    }
    mako_local::clear_test_commit_observer().expect("clear native commit observer");
}

fn required_path(variable: &str) -> PathBuf {
    env::var_os(variable)
        .map(PathBuf::from)
        .unwrap_or_else(|| panic!("missing {variable}"))
}

fn writer_role() {
    let db_path = required_path(DB_PATH_ENV);
    let marker = required_path(MARKER_PATH_ENV);
    let point_name = env::var(POINT_ENV).expect("missing crash point");
    let point = Point::from_name(&point_name).expect("unknown crash point");

    let cache = open_observed_writer(&db_path);
    let mut baseline = cache.transaction().expect("begin baseline transaction");
    assert!(baseline
        .put(REPLACE_KEY, OLD_VALUE)
        .expect("seed replacement key"));
    assert!(baseline
        .put(DELETE_KEY, OLD_VALUE)
        .expect("seed deletion key"));
    baseline.commit().expect("commit baseline transaction");
    cache.flush().expect("flush baseline transaction");

    let mut transaction = cache.transaction().expect("begin crash transaction");
    assert!(!transaction
        .put(REPLACE_KEY, NEW_VALUE)
        .expect("stage replacement"));
    assert!(transaction.remove(DELETE_KEY).expect("stage deletion"));
    assert!(transaction
        .put(NEW_KEY, INSERTED_VALUE)
        .expect("stage insertion"));

    failpoint::arm(point, marker);
    install_native_observer(point);
    transaction.commit().expect("commit crash transaction");
    clear_native_observer(point);

    // Foreground points park inside commit. Writeback points park the consumer;
    // a synchronous flush drives that consumer and keeps this process alive
    // until its parent kills it.
    cache.flush().expect("drive crash transaction to failpoint");
    panic!("armed crash failpoint was not reached: {}", point.name());
}

fn verifier_role() {
    let db_path = required_path(DB_PATH_ENV);
    let expected_name = env::var(EXPECTED_ENV).expect("missing expected recovery state");
    let expected = Expected::from_name(&expected_name).expect("unknown expected recovery state");
    let cache = Db::open(&db_path, sync_options()).expect("reopen killed writer cache");

    let recovered = (
        cache.get(REPLACE_KEY).expect("recover replacement key"),
        cache.get(DELETE_KEY).expect("recover deletion key"),
        cache.get(NEW_KEY).expect("recover inserted key"),
    );
    let wanted = match expected {
        Expected::Old => (Some(OLD_VALUE.to_vec()), Some(OLD_VALUE.to_vec()), None),
        Expected::New => (
            Some(NEW_VALUE.to_vec()),
            None,
            Some(INSERTED_VALUE.to_vec()),
        ),
    };
    assert_eq!(
        recovered, wanted,
        "a recovered transaction must contain either all old values or all new values"
    );
    cache.close().expect("close recovered cache");
}

fn recovery_seed_role() {
    let db_path = required_path(DB_PATH_ENV);
    let cache = Db::open(&db_path, sync_options()).expect("open recovery seed cache");
    mako_local::advance_mako_timestamp_past(
        mako_local::MakoTimestamp::new(RECOVERY_TIMESTAMP_FLOOR)
            .expect("recovery test timestamp floor is valid"),
    )
    .expect("raise seed process Mako timestamp");

    let mut first = cache
        .transaction()
        .expect("begin first durable transaction");
    assert!(first.put(RECOVERY_A, b"a-v1").expect("seed recovery a"));
    assert!(first.put(RECOVERY_B, b"b-v1").expect("seed recovery b"));
    assert!(first
        .put(RECOVERY_TOMBSTONE, b"doomed")
        .expect("seed recovery tombstone"));
    first.commit().expect("commit first durable transaction");

    let mut second = cache
        .transaction()
        .expect("begin second durable transaction");
    assert!(!second.put(RECOVERY_A, b"a-v2").expect("replace recovery a"));
    assert!(second.put(RECOVERY_C, b"c-v1").expect("seed recovery c"));
    assert!(second
        .remove(RECOVERY_TOMBSTONE)
        .expect("delete recovery tombstone"));
    second.commit().expect("commit second durable transaction");

    let mut third = cache
        .transaction()
        .expect("begin third durable transaction");
    assert!(!third
        .put(RECOVERY_B, b"b-final")
        .expect("replace recovery b"));
    assert!(!third.put(RECOVERY_C, b"c-v2").expect("replace recovery c"));
    assert!(third
        .put(RECOVERY_D, b"\0durable\xff")
        .expect("seed binary recovery value"));
    third.commit().expect("commit third durable transaction");

    let mut fourth = cache
        .transaction()
        .expect("begin fourth durable transaction");
    assert!(!fourth
        .put(RECOVERY_A, b"a-final")
        .expect("finalize recovery a"));
    assert!(fourth.remove(RECOVERY_C).expect("delete recovery c"));
    assert!(fourth
        .put(RECOVERY_EMPTY, b"")
        .expect("seed empty recovery value"));
    fourth.commit().expect("commit fourth durable transaction");

    assert_eq!(
        cache.flush().expect("flush recovery seed history"),
        RECOVERY_RECORDS
    );
    assert_eq!(
        cache.close().expect("close recovery seed cache"),
        RECOVERY_RECORDS
    );
}

fn recovery_crash_role() {
    let db_path = required_path(DB_PATH_ENV);
    let marker = required_path(MARKER_PATH_ENV);
    let point_name = env::var(POINT_ENV).expect("missing recovery crash point");
    let point = Point::from_name(&point_name).expect("unknown recovery crash point");

    failpoint::arm(point, marker);
    let _cache = Db::open(&db_path, sync_options()).expect("run production recovery path");
    panic!("armed recovery failpoint was not reached: {}", point.name());
}

fn recovery_verifier_role() {
    let db_path = required_path(DB_PATH_ENV);
    let cache = Db::open(&db_path, sync_options()).expect("reopen interrupted recovery");
    assert_eq!(cache.durable_sequence(), RECOVERY_RECORDS);
    assert_eq!(cache.highest_acknowledged_sequence(), RECOVERY_RECORDS);

    let recovered = (
        cache.get(RECOVERY_A).expect("recover final a"),
        cache.get(RECOVERY_B).expect("recover final b"),
        cache.get(RECOVERY_C).expect("recover deleted c"),
        cache.get(RECOVERY_D).expect("recover binary d"),
        cache.get(RECOVERY_EMPTY).expect("recover empty value"),
        cache
            .get(RECOVERY_TOMBSTONE)
            .expect("recover deleted tombstone"),
    );
    assert_eq!(
        recovered,
        (
            Some(b"a-final".to_vec()),
            Some(b"b-final".to_vec()),
            None,
            Some(b"\0durable\xff".to_vec()),
            Some(Vec::new()),
            None,
        ),
        "restarting interrupted recovery must converge to the complete durable history"
    );

    let mut seed_log_keys = Vec::new();
    cache
        .backend()
        .for_each_key(&mut |key| {
            if matches!(classify_backend_key(key), BackendKey::Log(_)) {
                seed_log_keys.push(key.to_vec());
            }
        })
        .expect("enumerate recovered commit records");
    assert_eq!(seed_log_keys.len(), RECOVERY_RECORDS as usize);
    let max_record_bytes = sync_options().cache.writeback.max_record_bytes;
    let recovered_max_timestamp = seed_log_keys
        .iter()
        .map(|key| {
            let encoded = cache
                .backend()
                .get(key)
                .expect("read recovered commit record")
                .expect("recovered commit record exists");
            CommitRecord::decode(key, &encoded, max_record_bytes)
                .expect("decode recovered commit record")
                .mako_timestamp()
        })
        .max()
        .expect("seed history has a timestamp");
    assert!(
        recovered_max_timestamp.get() > RECOVERY_TIMESTAMP_FLOOR,
        "seed history must exercise recovery from a deliberately high Mako timestamp"
    );

    cache
        .put(RECOVERY_POST_RESTART, b"clock-floored")
        .expect("commit after interrupted recovery");
    assert_eq!(
        cache.flush().expect("flush post-recovery transaction"),
        RECOVERY_RECORDS + 1
    );
    let mut post_recovery_log_key = None;
    cache
        .backend()
        .for_each_key(&mut |key| {
            if matches!(
                classify_backend_key(key),
                BackendKey::Log(sequence) if sequence.get() == RECOVERY_RECORDS + 1
            ) {
                post_recovery_log_key = Some(key.to_vec());
            }
        })
        .expect("find post-recovery commit record");
    let post_recovery_log_key = post_recovery_log_key.expect("post-recovery log key exists");
    let post_recovery_encoded = cache
        .backend()
        .get(&post_recovery_log_key)
        .expect("read post-recovery commit record")
        .expect("post-recovery commit record exists");
    let post_recovery_record = CommitRecord::decode(
        &post_recovery_log_key,
        &post_recovery_encoded,
        max_record_bytes,
    )
    .expect("decode post-recovery commit record");
    assert!(
        post_recovery_record.mako_timestamp() > recovered_max_timestamp,
        "reopen must floor Mako's clock beyond every durable timestamp"
    );
    assert_eq!(
        cache
            .get(RECOVERY_POST_RESTART)
            .expect("read post-recovery value")
            .as_deref(),
        Some(&b"clock-floored"[..])
    );
    assert_eq!(
        cache.close().expect("close recovery verifier"),
        RECOVERY_RECORDS + 1
    );
}

/// One helper entry point is self-executed for both writer and verifier roles.
#[test]
fn crash_role() {
    let Some(role) = env::var_os(ROLE_ENV) else {
        return;
    };
    // Every subprocess must eventually return control to its parent, including
    // seed and verifier roles that use blocking `Command::output()` there.
    // Crash-point roles are normally killed much earlier by the controller.
    let _watchdog = std::thread::Builder::new()
        .name("mako-crash-watchdog".to_owned())
        .spawn(|| {
            std::thread::sleep(CHILD_WATCHDOG);
            std::process::abort();
        })
        .expect("start crash-child watchdog");
    match role.to_str().expect("crash role must be UTF-8") {
        "writer" => writer_role(),
        "verifier" => verifier_role(),
        "recovery-seed" => recovery_seed_role(),
        "recovery-crash" => recovery_crash_role(),
        "recovery-verifier" => recovery_verifier_role(),
        other => panic!("unknown crash-test role: {other}"),
    }
}

fn role_command(role: &str, scratch: &Scratch, point: Point, expected: Expected) -> Command {
    let mut command = Command::new(env::current_exe().expect("locate current test executable"));
    command
        .arg("--exact")
        .arg("crash_tests::crash_role")
        .arg("--test-threads=1")
        .arg("--nocapture")
        .env(ROLE_ENV, role)
        .env(DB_PATH_ENV, &scratch.db)
        .env(MARKER_PATH_ENV, &scratch.marker)
        .env(POINT_ENV, point.name())
        .env(EXPECTED_ENV, expected.name());
    command
}

fn wait_for_marker(child: &mut Child, marker: &Path, point: Point) {
    let deadline = Instant::now() + CHILD_TIMEOUT;
    loop {
        match fs::read_to_string(marker) {
            Ok(contents) if contents == point.name() => return,
            // `write_all` may use more than one syscall. A prefix is a marker
            // still being published, not evidence that the child hit the
            // wrong phase.
            Ok(contents) if point.name().starts_with(&contents) => {}
            Ok(contents) => panic!(
                "crash marker contained {contents:?}, expected {:?}",
                point.name()
            ),
            Err(error) if error.kind() == ErrorKind::NotFound => {}
            Err(error) => panic!("cannot read crash marker: {error}"),
        }

        if let Some(status) = child.try_wait().expect("inspect crash writer") {
            panic!("crash writer exited before its failpoint: {status}");
        }
        assert!(
            Instant::now() < deadline,
            "crash writer did not reach {} within {:?}",
            point.name(),
            CHILD_TIMEOUT
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn process_crash_matrix_recovers_only_whole_transactions() {
    let native_observer = mako_local::features()
        .expect("read native crash-hook capability")
        .test_commit_observer();
    let require_native_observer = env::var_os(REQUIRE_NATIVE_HOOKS_ENV).is_some();
    assert!(
        native_observer || !require_native_observer,
        "{REQUIRE_NATIVE_HOOKS_ENV} requires a native build configured with MAKO_LOCAL_TEST_HOOKS=ON"
    );
    if !native_observer {
        eprintln!(
            "native commit crash seams are disabled; this profile runs 9/15 write-path points"
        );
    }
    for &(point, expected) in CASES {
        // Ordinary builds keep the native commit path hook-free. The outer
        // Rust/Rocks points still run there; a dedicated hook-enabled native
        // profile runs the six lock-held cases as well.
        if point.requires_native_observer() && !native_observer {
            continue;
        }
        let scratch = Scratch::new(point);
        let writer = role_command("writer", &scratch, point, expected)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn crash writer");
        let mut writer = ChildGuard::new(writer);
        wait_for_marker(writer.child_mut(), &scratch.marker, point);
        let status = writer.kill_and_wait();
        assert_eq!(
            status.signal(),
            Some(9),
            "writer at {} did not die from SIGKILL: {status}",
            point.name()
        );

        let output = role_command("verifier", &scratch, point, expected)
            .output()
            .expect("spawn recovery verifier");
        assert!(
            output.status.success(),
            "recovery verifier failed at {} (expected {}):\nstdout:\n{}\nstderr:\n{}",
            point.name(),
            expected.name(),
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }
}

#[test]
fn recovery_process_crash_matrix_converges_after_restart() {
    for &point in RECOVERY_CASES {
        let scratch = Scratch::new(point);
        let seed = role_command("recovery-seed", &scratch, point, Expected::New)
            .output()
            .expect("spawn durable-history seeder");
        assert!(
            seed.status.success(),
            "durable-history seeder failed for {}:\nstdout:\n{}\nstderr:\n{}",
            point.name(),
            String::from_utf8_lossy(&seed.stdout),
            String::from_utf8_lossy(&seed.stderr)
        );

        // Repeat the same interrupted recovery twice against unchanged durable
        // state. This catches one-shot or restart-sensitive recovery logic;
        // neither partial in-memory replay may leak into the next process.
        for restart in 1..=2 {
            match fs::remove_file(&scratch.marker) {
                Ok(()) => {}
                Err(error) if error.kind() == ErrorKind::NotFound => {}
                Err(error) => panic!("remove stale recovery marker: {error}"),
            }
            let recovery = role_command("recovery-crash", &scratch, point, Expected::New)
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .expect("spawn crashable recovery process");
            let mut recovery = ChildGuard::new(recovery);
            wait_for_marker(recovery.child_mut(), &scratch.marker, point);
            let status = recovery.kill_and_wait();
            assert_eq!(
                status.signal(),
                Some(9),
                "recovery restart {restart} at {} did not die from SIGKILL: {status}",
                point.name()
            );
        }

        let verifier = role_command("recovery-verifier", &scratch, point, Expected::New)
            .output()
            .expect("spawn post-crash recovery verifier");
        assert!(
            verifier.status.success(),
            "recovery verifier failed at {}:\nstdout:\n{}\nstderr:\n{}",
            point.name(),
            String::from_utf8_lossy(&verifier.stdout),
            String::from_utf8_lossy(&verifier.stderr)
        );
    }
}
