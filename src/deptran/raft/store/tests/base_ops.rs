//! `ops_for`'s range deletes (B32's `top`) against the in-memory base, so
//! they are checked without the `rocksdb` feature: an append past the highest
//! index written deletes nothing; a cut at or below it deletes the cut keys,
//! with or without new entries; the first record after a restart (`top`
//! unknown) deletes conservatively; snapshots drop the prefix, and an install
//! the rest. After each record the base loads as the fold of the records.

use raft_store::base::{self, ops_for, Base, MemBase, Op};
use raft_store::{Hard, Identity, Record, SavedState, SnapRef};

const ID: Identity = Identity { site: 1, partition: 0, fingerprint: 5, format: 1 };

fn rec(from: u64, n: u64, term: u64) -> Record<Vec<u8>> {
    Record { replace_from: Some(from), entries: (0..n).map(|i| (term, vec![(from + i) as u8])).collect(),
             ..Default::default() }
}

#[test]
fn deletes_follow_top() {
    let base = MemBase::new();
    let mut b = base.clone();
    b.write(&[Op::Put(base::KEY_ID.to_vec(), base::identity_bytes(&ID)),
              Op::Put(base::KEY_C.to_vec(), 0u64.to_le_bytes().to_vec())]).unwrap();
    let snap = |index, keep| Record::<Vec<u8>> {
        snapshot: Some(SnapRef { index, term: 2, image: None, keep }), ..Default::default()
    };
    // (record, range deletes it must emit, top after it)
    let steps: Vec<(Record<Vec<u8>>, usize, u64)> = vec![
        (Record { hard: Some(Hard { term: 1, vote: 2, commit: 0 }), ..rec(1, 5, 1) }, 1, 5), // top unknown
        (rec(6, 2, 1), 0, 7),   // an append past top
        (rec(4, 1, 2), 1, 4),   // a cut that shortens the log, with an entry
        (rec(4, 0, 2), 1, 3),   // a pure cut
        (rec(4, 1, 2), 0, 4),   // past top again: nothing to delete
        (snap(2, true), 1, 4),  // compaction: the prefix goes, the suffix stays
        (rec(5, 3, 3), 0, 7),
        (snap(9, false), 2, 9), // an install: everything goes
        (rec(10, 2, 4), 0, 11),
    ];
    let (mut top, mut state) = (u64::MAX, SavedState::new(u16::MAX));
    for (k, (r, ranges, want_top)) in steps.into_iter().enumerate() {
        let seq = k as u64 + 1;
        let mut ops = Vec::new();
        ops_for(&r, seq, &mut top, &mut ops);
        let got = ops.iter().filter(|o| matches!(o, Op::DeleteRange(..))).count();
        assert_eq!((got, top), (ranges, want_top), "record {seq}: range deletes and top");
        b.write(&ops).unwrap();
        state.apply(r).unwrap();
        assert_eq!(base::load(&base, &ID, u16::MAX).unwrap(), (seq, state.clone()), "record {seq}: the base");
    }
}
