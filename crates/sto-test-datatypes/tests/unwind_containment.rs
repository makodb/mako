use std::cmp::Ordering;
use std::hash::{Hash, Hasher};
use std::num::NonZeroUsize;
use std::panic::{catch_unwind, AssertUnwindSafe};

use sto_core::{AbortReason, CommitOutcome, Runtime, RuntimeConfig, TxnCounter};
use sto_test_datatypes::{TxnHashMap, TxnVec};

fn assert_committed(outcome: CommitOutcome) {
    assert!(matches!(outcome, CommitOutcome::Committed(_)));
}

#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct PanicHash(u64);

impl Hash for PanicHash {
    fn hash<H: Hasher>(&self, state: &mut H) {
        assert_ne!(self.0, u64::MAX, "intentional Hash panic");
        self.0.hash(state);
    }
}

#[test]
fn a_caught_key_hash_panic_dooms_prior_staged_work() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map = TxnHashMap::<PanicHash, u64>::new(&runtime).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    map.insert(&mut transaction, PanicHash(1), 10).unwrap();

    let panic = catch_unwind(AssertUnwindSafe(|| {
        let _ = map.get(&mut transaction, &PanicHash(u64::MAX));
    }));
    assert!(panic.is_err());
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Doomed)
    );

    let mut verify = worker.begin().unwrap();
    assert_eq!(map.get(&mut verify, &PanicHash(1)).unwrap(), None);
    assert_committed(verify.commit().unwrap());
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct PanicOrd(u64);

impl Ord for PanicOrd {
    fn cmp(&self, other: &Self) -> Ordering {
        assert_eq!(self.0, other.0, "intentional Ord panic");
        Ordering::Equal
    }
}

impl PartialOrd for PanicOrd {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

#[test]
fn a_caught_full_map_comparison_panic_dooms_prior_staged_work() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map =
        TxnHashMap::<PanicOrd, u64>::with_bucket_count(&runtime, NonZeroUsize::new(16).unwrap())
            .unwrap();
    let counter = TxnCounter::new(&runtime, 0).unwrap();
    let first = PanicOrd(0);
    let second = (1..10_000)
        .map(PanicOrd)
        .find(|candidate| map.bucket_index(candidate) != map.bucket_index(&first))
        .expect("sixteen buckets must separate at least one sampled key");
    let mut worker = runtime.attach().unwrap();

    let mut setup = worker.begin().unwrap();
    map.insert(&mut setup, first.clone(), 10).unwrap();
    map.insert(&mut setup, second.clone(), 20).unwrap();
    assert_committed(setup.commit().unwrap());

    let mut transaction = worker.begin().unwrap();
    counter.increment(&mut transaction, 1).unwrap();
    let panic = catch_unwind(AssertUnwindSafe(|| {
        let _ = map.to_btree_map(&mut transaction);
    }));
    assert!(panic.is_err());
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Doomed)
    );

    let mut verify = worker.begin().unwrap();
    assert_eq!(counter.get(&mut verify).unwrap(), 0);
    assert_eq!(map.get(&mut verify, &first).unwrap(), Some(10));
    assert_eq!(map.get(&mut verify, &second).unwrap(), Some(20));
    assert_committed(verify.commit().unwrap());
}

#[derive(Clone, Debug)]
struct DropBomb {
    panic_on_drop: bool,
}

impl Drop for DropBomb {
    fn drop(&mut self) {
        assert!(!self.panic_on_drop, "intentional Drop panic");
    }
}

#[test]
fn a_caught_rejected_vector_value_drop_panic_dooms_prior_staged_work() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let vector = TxnVec::from_iter(
        &runtime,
        [DropBomb {
            panic_on_drop: false,
        }],
    )
    .unwrap();
    let counter = TxnCounter::new(&runtime, 0).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    counter.increment(&mut transaction, 1).unwrap();

    let panic = catch_unwind(AssertUnwindSafe(|| {
        let _ = vector.set(
            &mut transaction,
            1,
            DropBomb {
                panic_on_drop: true,
            },
        );
    }));
    assert!(panic.is_err());
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Doomed)
    );

    let mut verify = worker.begin().unwrap();
    assert_eq!(counter.get(&mut verify).unwrap(), 0);
    assert_eq!(vector.len(&mut verify).unwrap(), 1);
    assert_committed(verify.commit().unwrap());
}
