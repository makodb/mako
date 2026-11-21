#![cfg(feature = "test-install-observer")]

use std::{
    cell::RefCell,
    sync::{Arc, Barrier},
};
use sto_core::{CommitOutcome, Conflict, Runtime, RuntimeConfig, TxnArray, TxnCell, TxnCounter};

thread_local! {
    static RENDEZVOUS: RefCell<Option<Arc<Barrier>>> = const { RefCell::new(None) };
}

fn after_two_installs(installed: usize) {
    if installed == 2 {
        RENDEZVOUS.with(|slot| {
            let barrier = slot.borrow();
            let barrier = barrier.as_ref().unwrap();
            barrier.wait();
            barrier.wait();
        });
    }
}

#[test]
fn cell_and_counter_publish_during_install_while_the_later_array_stays_locked() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1_i64).unwrap();
    let counter = TxnCounter::new(&runtime, 10).unwrap();
    let array = TxnArray::new(&runtime, [100_i64]).unwrap();
    let rendezvous = Arc::new(Barrier::new(2));
    let mut reader_worker = runtime.attach().unwrap();
    std::thread::scope(|scope| {
        let writer = scope.spawn(|| {
            RENDEZVOUS.with(|slot| *slot.borrow_mut() = Some(Arc::clone(&rendezvous)));
            let mut worker = runtime.attach().unwrap();
            let mut txn = worker.begin().unwrap();
            cell.set(&mut txn, 2).unwrap();
            counter.increment(&mut txn, 5).unwrap();
            array.set(&mut txn, 0, 200).unwrap().unwrap();
            let result = sto_core::test_install::with_observer(after_two_installs, || txn.commit());
            let _ = RENDEZVOUS.with(|slot| slot.borrow_mut().take());
            result
        });
        rendezvous.wait();
        let mut read = reader_worker.begin().unwrap();
        let cell_value = cell.get(&mut read);
        let counter_value = counter.get(&mut read);
        let array_value = array.get(&mut read, 0);
        read.abort();
        // Release the writer before any assertion can unwind the rendezvous.
        rendezvous.wait();
        assert_eq!(cell_value.unwrap(), 2);
        assert_eq!(counter_value.unwrap(), 15);
        assert_eq!(array_value, Err(Conflict::LockBusy.into()));
        assert!(matches!(
            writer.join().unwrap().unwrap(),
            CommitOutcome::Committed(_)
        ));
    });
    let mut read = reader_worker.begin().unwrap();
    assert_eq!(array.get(&mut read, 0).unwrap().unwrap(), 200);
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}
