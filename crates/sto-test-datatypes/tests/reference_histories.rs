use std::collections::{BTreeMap, VecDeque};
use std::num::NonZeroUsize;

use sto_core::{AbortReason, CommitOutcome, Runtime, RuntimeConfig};
use sto_test_datatypes::{TxnHashMap, TxnQueue, TxnVec};

fn assert_committed(outcome: CommitOutcome) {
    assert!(matches!(outcome, CommitOutcome::Committed(_)));
}

#[test]
fn deterministic_commit_and_abort_history_matches_standard_collections() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map =
        TxnHashMap::<u64, i64>::with_bucket_count(&runtime, NonZeroUsize::new(8).unwrap()).unwrap();
    let vector = TxnVec::<i64>::new(&runtime).unwrap();
    let queue = TxnQueue::<i64>::new(&runtime).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut committed_map = BTreeMap::new();
    let mut committed_vector = Vec::new();
    let mut committed_queue = VecDeque::new();
    let mut state = 0x4d59_5df4_d0f3_3173_u64;

    for step in 0..96_u64 {
        state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        let key = state % 13;
        let value = (state >> 17) as i64;
        let mut expected_map = committed_map.clone();
        let mut expected_vector = committed_vector.clone();
        let mut expected_queue = committed_queue.clone();
        let mut transaction = worker.begin().unwrap();

        match state % 3 {
            0 => assert_eq!(
                map.insert(&mut transaction, key, value).unwrap(),
                expected_map.insert(key, value)
            ),
            1 => assert_eq!(
                map.remove(&mut transaction, &key).unwrap(),
                expected_map.remove(&key)
            ),
            _ => assert_eq!(
                map.get(&mut transaction, &key).unwrap(),
                expected_map.get(&key).copied()
            ),
        }

        match (state >> 3) % 4 {
            0 => {
                vector.push(&mut transaction, value).unwrap();
                expected_vector.push(value);
            }
            1 => assert_eq!(vector.pop(&mut transaction).unwrap(), expected_vector.pop()),
            2 if !expected_vector.is_empty() => {
                let index = key as usize % expected_vector.len();
                let expected_old = std::mem::replace(&mut expected_vector[index], value);
                assert_eq!(
                    vector.set(&mut transaction, index, value).unwrap().unwrap(),
                    expected_old
                );
            }
            _ => {
                vector.insert(&mut transaction, 0, value).unwrap().unwrap();
                expected_vector.insert(0, value);
            }
        }

        if (state >> 7) & 1 == 0 {
            queue.push_back(&mut transaction, value).unwrap();
            expected_queue.push_back(value);
        } else {
            assert_eq!(
                queue.pop_front(&mut transaction).unwrap(),
                expected_queue.pop_front()
            );
        }

        assert_eq!(map.to_btree_map(&mut transaction).unwrap(), expected_map);
        assert_eq!(vector.to_vec(&mut transaction).unwrap(), expected_vector);
        assert_eq!(
            queue.to_vec(&mut transaction).unwrap(),
            expected_queue.iter().copied().collect::<Vec<_>>()
        );

        if step % 6 == 0 {
            assert_eq!(*transaction.abort().reason(), AbortReason::Explicit);
        } else {
            assert_committed(transaction.commit().unwrap());
            committed_map = expected_map;
            committed_vector = expected_vector;
            committed_queue = expected_queue;
        }
    }

    let mut verify = worker.begin().unwrap();
    assert_eq!(map.to_btree_map(&mut verify).unwrap(), committed_map);
    assert_eq!(vector.to_vec(&mut verify).unwrap(), committed_vector);
    assert_eq!(
        queue.to_vec(&mut verify).unwrap(),
        committed_queue.into_iter().collect::<Vec<_>>()
    );
    assert_committed(verify.commit().unwrap());
}
