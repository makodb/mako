//! Real native singleton poison proof, isolated to one exact test subprocess.
//! This module and the private native helper are absent from release builds.

use super::*;
use crate::resolved_cache::Probe;
use std::process::{Command, Stdio};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

const CHILD: &str = "MAKO_NATIVE_HEALTH_PROOF_CHILD";
const TEST: &str = "txn::native_health_proof::global_native_poison_rejects_cross_db_cached_put";

unsafe extern "C" {
    fn mako_native_health_proof_poison() -> i32;
}

fn child_case(scenario: &str) {
    assert!(matches!(scenario, "hit-active" | "hit-fresh" | "miss-active" | "miss-fresh"));
    let origin = Arc::new(LocalDb::open().unwrap());
    let target = LocalDb::open().unwrap();
    let origin_table = origin.open_table("origin", 1).unwrap();
    let target_table = target.open_table("target", 1).unwrap();
    for (db, table) in [(&*origin, &origin_table), (&target, &target_table)] {
        let mut seed = db.transaction().unwrap();
        seed.put(table, b"cached", b"before").unwrap();
        seed.commit().unwrap();
    }
    let attachment = target.attach().unwrap();
    assert!(matches!(attachment.slot.probe_resolved(&target_table.entry.table, b"cached"), Probe::Hit(_)));
    assert!(matches!(attachment.slot.probe_resolved(&target_table.entry.table, b"cold-miss"), Probe::Miss(_)));
    let native = masstree::Runtime::new(masstree::RuntimeConfig::new()
        .with_max_threads(mako_engine_api::MAX_WORKERS as u32)).unwrap();
    assert_eq!(native.health().unwrap(), masstree::RuntimeHealth::Healthy);
    let mut transaction = scenario.ends_with("active").then(|| target.transaction().unwrap());
    let (poisoned_tx, poisoned_rx) = mpsc::channel();
    let (retire_tx, retire_rx) = mpsc::channel();
    let origin_worker = Arc::clone(&origin);
    let producer = std::thread::spawn(move || {
        // Bind another real native worker to the first DB, then stop after
        // native poison and before any STO access-error mapping can run.
        origin_worker.attach().unwrap();
        // SAFETY: this exact subprocess links the macro-guarded proof TU.
        // Its helper performs the production atomic Release transition only.
        assert_eq!(unsafe { mako_native_health_proof_poison() }, 0);
        poisoned_tx.send(()).unwrap();
        let _ = retire_rx.recv_timeout(Duration::from_secs(15));
    });
    poisoned_rx.recv_timeout(Duration::from_secs(10)).unwrap();
    assert_eq!(native.health().unwrap(), masstree::RuntimeHealth::Poisoned);
    assert_eq!(origin.runtime_for_test().health(), sto_core::RuntimeHealth::Healthy);
    assert_eq!(target.runtime_for_test().health(), sto_core::RuntimeHealth::Healthy);
    assert_eq!(target_table.entry.table.health(), sto_masstree::TableHealth::Healthy);
    println!("NATIVE_HEALTH_REAL_POISON_BEFORE_RUST_MAPPING {scenario}");
    let mut transaction = transaction.take().unwrap_or_else(|| target.transaction().unwrap());
    let key: &[u8] = if scenario.starts_with("hit") { b"cached" } else { b"cold-miss" };
    let put = transaction.put(&target_table, key, b"must-not-commit");
    if put.is_ok() {
        let report = transaction.commit_report();
        if report.disposition == CommitDisposition::Committed && report.cleanup.is_ok() {
            println!("NATIVE_HEALTH_BYPASS_COMMITTED {scenario}");
        }
        panic!("poisoned native runtime accepted PUT: {report:?}");
    }
    assert_eq!(put, Err(Error::WorkerPoisoned));
    assert_ne!(transaction.commit_report().disposition, CommitDisposition::Committed);
    assert_eq!(target_table.entry.table.health(), sto_masstree::TableHealth::Poisoned);
    assert_eq!(target.runtime_for_test().health(), sto_core::RuntimeHealth::Poisoned);
    // The native fault is global, but Rust propagation is per core runtime.
    assert_eq!(origin.runtime_for_test().health(), sto_core::RuntimeHealth::Healthy);
    assert!(matches!(target.transaction(), Err(Error::WorkerPoisoned)));
    retire_tx.send(()).unwrap();
    producer.join().unwrap();
    println!("NATIVE_HEALTH_REJECTED {scenario}");
    // No reset and no healthy-fixture teardown. This child's poisoned native
    // singleton and intentional worker retention end only at process exit.
}

#[test]
fn global_native_poison_rejects_cross_db_cached_put() {
    if let Ok(scenario) = std::env::var(CHILD) {
        return child_case(&scenario);
    }
    for scenario in ["hit-active", "hit-fresh", "miss-active", "miss-fresh"] {
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args(["--exact", TEST, "--test-threads=1", "--nocapture"])
            .env(CHILD, scenario)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn().unwrap();
        let deadline = Instant::now() + Duration::from_secs(25);
        loop {
            if child.try_wait().unwrap().is_some() { break; }
            if Instant::now() >= deadline {
                let _ = child.kill();
                let output = child.wait_with_output().unwrap();
                panic!("native health child timed out: {scenario}: {output:?}");
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let output = child.wait_with_output().unwrap();
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(output.status.success(), "{scenario}: {stdout}\n{stderr}");
        assert!(stdout.contains(&format!("NATIVE_HEALTH_REJECTED {scenario}")), "{stdout}");
        println!("{stdout}");
    }
}
