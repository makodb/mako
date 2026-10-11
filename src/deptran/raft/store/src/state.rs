//! The saved state a replay of records folds into (design §4's `apply`:
//! `SavedState::apply`, shared by recovery, the shutdown verify and the
//! applier).

use crate::record::{Hard, Record};

/// Term, vote, commit, the snapshot boundary and the log after it.
#[derive(Clone, Debug, PartialEq)]
pub struct SavedState<P> {
    pub hard: Hard,
    pub snap_index: u64,
    pub snap_term: u64,
    pub image: Option<String>,
    /// `entries[k]` is the entry at index `snap_index + 1 + k`: (term, payload).
    pub entries: Vec<(u64, P)>,
}

impl<P> SavedState<P> {
    /// A fresh server's state: term 0, no vote, an empty log from index 1.
    pub fn new(no_vote: u16) -> Self {
        SavedState {
            hard: Hard { term: 0, vote: no_vote, commit: 0 },
            snap_index: 0,
            snap_term: 0,
            image: None,
            entries: Vec::new(),
        }
    }

    pub fn last(&self) -> u64 {
        self.snap_index + self.entries.len() as u64
    }

    /// Applies one record whole. A record that cannot follow this state (a
    /// replace that leaves a gap or reaches into the snapshot, a snapshot
    /// behind the current one) is an error: the store is not one this
    /// server wrote.
    pub fn apply(&mut self, rec: Record<P>) -> Result<(), String> {
        if let Some(h) = rec.hard {
            self.hard = h;
        }
        if let Some(s) = rec.snapshot {
            if s.index < self.snap_index {
                return Err(format!("snapshot at {} behind the current one at {}", s.index, self.snap_index));
            }
            let drop = (s.index - self.snap_index) as usize;
            if s.keep {
                if drop > self.entries.len() {
                    return Err(format!("snapshot at {} keeps a log that ends at {}", s.index, self.last()));
                }
                self.entries.drain(..drop);
            } else {
                self.entries.clear();
            }
            self.snap_index = s.index;
            self.snap_term = s.term;
            self.image = s.image;
        }
        if let Some(from) = rec.replace_from {
            if from <= self.snap_index || from > self.last() + 1 {
                return Err(format!(
                    "replace from {from} outside {}..={} (snapshot {}, last {})",
                    self.snap_index + 1,
                    self.last() + 1,
                    self.snap_index,
                    self.last()
                ));
            }
            self.entries.truncate((from - self.snap_index - 1) as usize);
            self.entries.extend(rec.entries);
        } else if !rec.entries.is_empty() {
            return Err("entries without a replace index".into());
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::record::SnapRef;

    type R = Record<u32>;

    #[test]
    fn the_design_s_follower_step() {
        // design §2: at term 4 a follower holds 1-10, 6-10 of term 2; the
        // leader of term 5 sends prev 5, 6'-8' and commit 8.
        let mut s = SavedState::new(u16::MAX);
        s.apply(R { hard: Some(Hard { term: 4, vote: 1, commit: 5 }), replace_from: Some(1), entries: (1..=10).map(|i| (if i <= 5 { 1 } else { 2 }, i)).collect(), ..Default::default() }).unwrap();
        s.apply(R { hard: Some(Hard { term: 5, vote: u16::MAX, commit: 8 }), replace_from: Some(6), entries: vec![(5, 60), (5, 70), (5, 80)], ..Default::default() }).unwrap();
        assert_eq!(s.last(), 8);
        assert_eq!(s.hard, Hard { term: 5, vote: u16::MAX, commit: 8 });
        assert_eq!(s.entries[5], (5, 60));
    }

    #[test]
    fn snapshots_and_refusals() {
        let mut s = SavedState::new(0);
        s.apply(R { replace_from: Some(1), entries: (1..=10).map(|i| (1, i)).collect(), ..Default::default() }).unwrap();
        s.apply(R { snapshot: Some(SnapRef { index: 4, term: 1, image: None, keep: true }), ..Default::default() }).unwrap();
        assert_eq!((s.snap_index, s.last(), s.entries[0].1), (4, 10, 5));
        assert!(s.clone().apply(R { replace_from: Some(4), ..Default::default() }).is_err());
        assert!(s.clone().apply(R { replace_from: Some(12), ..Default::default() }).is_err());
        assert!(s.clone().apply(R { snapshot: Some(SnapRef { index: 3, term: 1, image: None, keep: true }), ..Default::default() }).is_err());
        assert!(s.clone().apply(R { snapshot: Some(SnapRef { index: 11, term: 1, image: None, keep: true }), ..Default::default() }).is_err());
        s.apply(R { snapshot: Some(SnapRef { index: 20, term: 3, image: Some("20-3.img".into()), keep: false }), ..Default::default() }).unwrap();
        assert_eq!((s.snap_index, s.last()), (20, 20));
        s.apply(R { replace_from: Some(21), entries: vec![(3, 21)], ..Default::default() }).unwrap();
        assert_eq!(s.last(), 21);
    }
}
