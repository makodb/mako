// Shared by the disk-persistence tests (persist_note.rs, observe_term.rs,
// restore.rs; docs/verus/disk-persistence-plan.md P1): a core driven through
// Setup's events, an inbound batch with entries, and the exactness check of
// a step's persist note.
#![allow(dead_code)]

use raft_core::*;

#[derive(Clone, Debug, PartialEq)]
pub struct Cmd(pub u64);

/// An inbound AppendEntries payload: the entries' terms and commands.
pub struct Batch {
    pub terms: Vec<i64>,
    pub cmds: Vec<u64>,
}

impl InboundBatch<Cmd> for Batch {
    fn decode_terms(&self, _prev: u64, terms: &mut Vec<i64>) -> bool {
        terms.clear();
        terms.extend_from_slice(&self.terms);
        true
    }

    fn entry_at(&self, k: u64) -> RaftEntry<Cmd> {
        RaftEntry::new(self.terms[k as usize], Cmd(self.cmds[k as usize]), true, false, 0, 8)
    }
}

pub fn batch(entries: &[(i64, u64)]) -> Batch {
    Batch { terms: entries.iter().map(|e| e.0).collect(), cmds: entries.iter().map(|e| e.1).collect() }
}

pub fn entry(term: i64, cmd: u64) -> RaftEntry<Cmd> {
    RaftEntry::new(term, Cmd(cmd), true, false, 0, 8)
}

/// Site 1 of {1, 2, 3}: identity and configuration, not yet gated.
pub fn configured_ungated() -> RaftCore<Cmd> {
    let mut core: RaftCore<Cmd> = RaftCore::new();
    core.step::<Batch>(Event::SetIdentity { loc_id: 0, site_id: 1, partition_id: 0 }, &mut CoreOutput::new())
        .into_done();
    core.step::<Batch>(Event::Configure { members: &[1, 2, 3] }, &mut CoreOutput::new()).into_done();
    core
}

pub fn configured() -> RaftCore<Cmd> {
    let mut core = configured_ungated();
    assert!(core.step::<Batch>(Event::EnterGates { snapshots_enabled: false, failover: true },
                               &mut CoreOutput::new()).into_gates());
    core
}

/// The saved state: term, vote, commit, and (term, cmd) per index from the
/// log's base.
#[derive(Clone, Debug, PartialEq)]
pub struct Saved {
    pub term: u64,
    pub vote: u16,
    pub commit: u64,
    pub base: u64,
    pub log: Vec<(i64, u64)>,
}

pub fn saved(core: &RaftCore<Cmd>) -> Saved {
    let base = core.raft_log_.base();
    let last = core.raft_log_.last_index();
    Saved {
        term: core.current_term_,
        vote: core.vote_for_,
        commit: core.commit_index_,
        base,
        log: (base..=last).map(|i| {
            let e = core.raft_log_.get(i).unwrap();
            (e.term(), e.cmd().0)
        }).collect(),
    }
}

/// `before` with the note applied: what the WAL record would make of it.
pub fn applied(before: &Saved, note: &Option<PersistNote>, after: &Saved) -> Saved {
    let mut s = before.clone();
    if let Some(n) = note {
        if n.hard_ {
            s.term = n.term_;
            s.vote = n.vote_;
            s.commit = n.commit_;
        }
        if n.log_from_ != 0 {
            assert!(n.log_from_ >= s.base && n.log_from_ <= s.base + s.log.len() as u64,
                    "note writes from {} outside the log {}..", n.log_from_, s.base);
            s.log.truncate((n.log_from_ - s.base) as usize);
            let from = (n.log_from_ - after.base) as usize;
            s.log.extend_from_slice(&after.log[from..]);
        }
    }
    s
}

/// One step, checked: the note is exact (applying it to the state before
/// gives the state after), and absent when nothing saved changed.
pub fn step<'a, W: InboundBatch<Cmd>>(core: &mut RaftCore<Cmd>, ev: Event<'a, Cmd, W>)
    -> (Reply<Cmd>, Option<PersistNote>) {
    let before = saved(core);
    let mut out = CoreOutput::new();
    let reply = core.step(ev, &mut out);
    let note = out.take_persist();
    let after = saved(core);
    assert_eq!(applied(&before, &note, &after), after, "the persist note is not exact");
    if before == after {
        assert!(note.is_none(), "a step that saved nothing left a note");
    }
    (reply, note)
}

pub fn append(core: &mut RaftCore<Cmd>, term: u64, leader: u16, prev: (u64, u64), entries: &[(i64, u64)],
              commit: u64) -> (Reply<Cmd>, Option<PersistNote>) {
    let wire = batch(entries);
    step(core, Event::RecvAppendEntries {
        wire: &wire, stopped: false, sender_is_current_voter: true, has_cmd: !entries.is_empty(),
        leader_current_term: term, leader_site_id: leader, leader_prev_log_index: prev.0,
        leader_prev_log_term: prev.1, leader_commit_index: commit, failover: true,
    })
}
