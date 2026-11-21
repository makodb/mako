//! Real native poison, only in fresh exact subprocesses of the proof build.
//! The dedicated runner links the macro-guarded test boundary. Ordinary test,
//! sanitizer and release builds do not compile this module or that helper.

use super::*;
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use mako_engine_api::WorkerHealth;

const TEST: &str = "db::native_health_tests::native_poison_is_diagnostic_before_point_mapping";
const ROLE: &str = "MAKO_NATIVE_RUNTIME_HEALTH_PROOF_ROLE";

unsafe extern "C" {
    fn mako_test_native_runtime_poison() -> i32;
}

fn child(scenario: &str) {
    assert!(matches!(scenario, "abort" | "drop"));
    let origin = Arc::new(LocalDb::open().unwrap());
    let target = LocalDb::open().unwrap();
    let table = target.open_table("native-health", 1).unwrap();
    let mut transaction = target.transaction().unwrap();
    transaction.put(&table, b"pending", &[0x37; 128]).unwrap();
    assert!(origin.runtime_is_healthy() && target.runtime_is_healthy());
    let quarantined = worker::quarantined_worker_count();
    assert_eq!(worker::health(), WorkerHealth::Healthy);

    let (poisoned_tx, poisoned_rx) = mpsc::channel();
    let (retire_tx, retire_rx) = mpsc::channel();
    let source = Arc::clone(&origin);
    let poisoner = std::thread::spawn(move || {
        source.attach().unwrap();
        // SAFETY: this exact proof executable links the guarded native TU.
        // It performs the production Release poison transition only.
        assert_eq!(unsafe { mako_test_native_runtime_poison() }, 0);
        poisoned_tx.send(()).unwrap();
        let _ = retire_rx.recv_timeout(Duration::from_secs(15));
    });
    poisoned_rx.recv_timeout(Duration::from_secs(10)).unwrap();
    assert_eq!(
        target.directory.health().unwrap(),
        masstree::RuntimeHealth::Poisoned
    );
    assert_eq!(origin.sto.health(), sto_core::RuntimeHealth::Healthy);
    assert_eq!(target.sto.health(), sto_core::RuntimeHealth::Healthy);
    assert_eq!(
        table.entry.table.health(),
        sto_masstree::TableHealth::Healthy
    );

    assert!(!origin.runtime_is_healthy());
    assert!(!target.runtime_is_healthy());
    assert_eq!(worker::health(), WorkerHealth::Healthy);
    assert_eq!(worker::quarantined_worker_count(), quarantined);
    // A diagnostic from an unattached thread must not attach or quarantine it.
    let observer = Arc::clone(&origin);
    std::thread::spawn(move || {
        assert_eq!(worker::health(), WorkerHealth::NotAttached);
        assert!(!observer.runtime_is_healthy());
        assert_eq!(worker::health(), WorkerHealth::NotAttached);
    })
    .join()
    .unwrap();
    println!("NATIVE_POISON_DIAGNOSTIC_WITH_HEALTHY_CORES {scenario}");

    match scenario {
        "abort" => assert_eq!(transaction.abort(), Ok(())),
        "drop" => drop(transaction),
        _ => unreachable!(),
    }
    assert_eq!(target.check_abort_cleanup(), Ok(()));
    assert_eq!(target.sto.health(), sto_core::RuntimeHealth::Healthy);
    assert_eq!(worker::health(), WorkerHealth::Healthy);
    assert_eq!(worker::quarantined_worker_count(), quarantined);
    // This fix is diagnostic-only. Existing core begin/clean abort behavior
    // is unchanged; point admission still maps native poison separately.
    assert_eq!(target.transaction().unwrap().abort(), Ok(()));
    assert!(!target.runtime_is_healthy());
    println!("NATIVE_POISON_KNOWN_CORE_CLEANUP {scenario}");

    let mut failed = target.transaction().unwrap();
    assert_eq!(
        failed.put(&table, b"mapped", b"no"),
        Err(Error::WorkerPoisoned)
    );
    assert_eq!(target.sto.health(), sto_core::RuntimeHealth::Poisoned);
    assert_eq!(origin.sto.health(), sto_core::RuntimeHealth::Healthy);
    assert_eq!(worker::health(), WorkerHealth::Poisoned);
    assert_eq!(worker::quarantined_worker_count(), quarantined + 1);
    retire_tx.send(()).unwrap();
    poisoner.join().unwrap();
    println!("NATIVE_POISON_POINT_MAPPING_REJECTED {scenario}");
    // No reset or healthy-fixture teardown after native poison. Process exit
    // owns the intentionally retained native runtime and worker attachments.
}

#[test]
fn native_poison_is_diagnostic_before_point_mapping() {
    if let Ok(scenario) = std::env::var(ROLE) {
        child(&scenario);
        return;
    }
    for scenario in ["abort", "drop"] {
        let mut process = Command::new(std::env::current_exe().unwrap())
            .args([TEST, "--exact", "--nocapture", "--test-threads=1"])
            .env(ROLE, scenario)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            if process.try_wait().unwrap().is_some() {
                break;
            }
            if Instant::now() >= deadline {
                let _ = process.kill();
                let output = process.wait_with_output().unwrap();
                panic!("native health child timed out: {scenario}: {output:?}");
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let output = process.wait_with_output().unwrap();
        assert!(output.status.success(), "{scenario}: {output:?}");
        let stdout = String::from_utf8(output.stdout).unwrap();
        for marker in [
            "NATIVE_POISON_DIAGNOSTIC_WITH_HEALTHY_CORES",
            "NATIVE_POISON_KNOWN_CORE_CLEANUP",
            "NATIVE_POISON_POINT_MAPPING_REJECTED",
        ] {
            assert!(stdout.contains(&format!("{marker} {scenario}")), "{stdout}");
        }
        print!("{stdout}");
    }
    let runtime =
        masstree::Runtime::new(masstree::RuntimeConfig::new().with_max_threads(MAX_WORKERS as u32))
            .unwrap();
    assert_eq!(runtime.health().unwrap(), masstree::RuntimeHealth::Healthy);
    assert_eq!(worker::health(), WorkerHealth::NotAttached);
}
