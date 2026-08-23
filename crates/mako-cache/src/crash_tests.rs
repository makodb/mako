//! Deterministic process-crash checks for the Rust/native/Rocks commit seam.

use std::env;
use std::fs;
use std::io::ErrorKind;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use crate::failpoint::{self, Point};
use crate::{Db, Durability, Options};

const ROLE_ENV: &str = "MAKO_CACHE_CRASH_TEST_ROLE";
const DB_PATH_ENV: &str = "MAKO_CACHE_CRASH_TEST_DB";
const MARKER_PATH_ENV: &str = "MAKO_CACHE_CRASH_TEST_MARKER";
const POINT_ENV: &str = "MAKO_CACHE_CRASH_TEST_POINT";
const EXPECTED_ENV: &str = "MAKO_CACHE_CRASH_TEST_EXPECTED";

const REPLACE_KEY: &[u8] = b"crash/replace";
const DELETE_KEY: &[u8] = b"crash/delete";
const NEW_KEY: &[u8] = b"crash/new";
const OLD_VALUE: &[u8] = b"old";
const NEW_VALUE: &[u8] = b"new";
const INSERTED_VALUE: &[u8] = b"inserted";

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
    (Point::PreinstallBound, Expected::Old),
    (Point::NativeCommittedBeforeReady, Expected::Old),
    (Point::ReadyBeforeBackend, Expected::Old),
    (Point::BackendWrittenBeforeDurable, Expected::New),
    (Point::DurableAdvanced, Expected::New),
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

    let _watchdog = std::thread::Builder::new()
        .name("mako-crash-watchdog".to_owned())
        .spawn(|| {
            std::thread::sleep(CHILD_WATCHDOG);
            std::process::abort();
        })
        .expect("start crash-child watchdog");

    let cache = Db::open(&db_path, sync_options()).expect("open writer cache");
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
    transaction.commit().expect("commit crash transaction");

    // The first three failpoints stop the foreground commit. The final three
    // stop a consumer; a synchronous flush ensures that consumer reaches the
    // armed point and keeps this process alive until its parent kills it.
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

/// One helper entry point is self-executed for both writer and verifier roles.
#[test]
fn crash_role() {
    let Some(role) = env::var_os(ROLE_ENV) else {
        return;
    };
    match role.to_str().expect("crash role must be UTF-8") {
        "writer" => writer_role(),
        "verifier" => verifier_role(),
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
            Ok(contents) if contents.is_empty() => {}
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
    for &(point, expected) in CASES {
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
