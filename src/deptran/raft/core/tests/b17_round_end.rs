// bugs-found B17: the round end (PHASE 3) advances the commit index without
// checking that this server still leads. This drives one server's core --
// server A of docs/verus/bugs-found.md's scenario -- through the inputs that
// server sees, in the order the race delivers them: its heartbeat round's
// replies, then a newer leader's RequestVote and append (which the RPC
// handler takes while the heartbeat driver waits on mtx_), then the round
// end. No timing is involved at this level: the core takes each input as a
// whole call, so the race shows as an order of calls.
//
// It asserts the correct outcome, so it fails today; it is ignored so the
// suite stays green. Reproduce with:
//   cargo test -p raft-core --test b17_round_end -- --ignored

use raft_core::*;

#[derive(Clone, Debug, PartialEq)]
struct Cmd(u64);

fn out() -> CoreOutput {
    CoreOutput::new()
}

// An append's payload: its entries' terms and commands, decoded as the
// shell's WireBatch decodes them (every term at least 1, F4).
struct Batch(Vec<(i64, Cmd)>);

impl InboundBatch<Cmd> for Batch {
    fn decode_terms(&self, _prev: u64, terms: &mut Vec<i64>) -> bool {
        terms.clear();
        let mut ok = true;
        for (term, _) in &self.0 {
            ok = ok && *term >= 1;
            terms.push(*term);
        }
        ok
    }

    fn entry_at(&self, k: u64) -> RaftEntry<Cmd> {
        let (term, cmd) = &self.0[k as usize];
        RaftEntry::new(*term, cmd.clone(), true, true, 0, 8)
    }
}

// Sites: A = 0 (this core), B = 1, C = 2, D = 3, E = 4.
const A: u16 = 0;
const B: u16 = 1;
const D: u16 = 3;

fn append(core: &mut RaftCore<Cmd>, from: u16, term: u64, prev: u64, prev_term: u64,
          entries: Vec<(i64, Cmd)>, commit: u64) -> u64 {
    let wire = Batch(entries);
    let has_cmd = !wire.0.is_empty();
    let (_report, ok, _term, _last) = core.step_checked(
        Event::RecvAppendEntries {
            wire: &wire, stopped: false, sender_is_current_voter: true, has_cmd,
            leader_current_term: term, leader_site_id: from, leader_prev_log_index: prev,
            leader_prev_log_term: prev_term, leader_commit_index: commit, failover: true,
        },
        &mut out()).unwrap().into_append();
    ok
}

fn request_vote(core: &mut RaftCore<Cmd>, from: u16, term: i64, last_index: u64, last_term: i64) {
    core.step_checked::<Batch>(
        Event::RecvRequestVote {
            stopped: false, candidate_is_current_voter: true, lst_log_idx: last_index,
            lst_log_term: last_term, can_id: from, can_term: term, failover: true,
            election_debug: false,
        },
        &mut out()).unwrap().into_vote();
}

// A campaign won with B's and C's votes (with A's own, 3 of 5).
fn win_election(core: &mut RaftCore<Cmd>) {
    let c = core.step::<Batch>(
        Event::StartElection { timer_guarded: false, expected_generation: 0, now: 0, stopped: false },
        &mut out()).into_campaign();
    assert!(c.started_);
    let won = core.step::<Batch>(
        Event::SettleElection {
            term: c.term_, loc_id: 0, voters: &[1, 2], granted: &[true, true],
            reply_terms: &[c.term_ as i64, c.term_ as i64], n_total: 5, timed_out: false,
            stopped: false, looping: true, failover: true, election_debug: false,
        },
        &mut out()).into_settled();
    assert!(won && core.is_leader_, "A wins term {}", c.term_);
}

fn term_at(core: &RaftCore<Cmd>, index: u64) -> i64 {
    core.raft_log_.get(index).expect("an entry").term()
}

#[test]
#[ignore = "bugs-found B17: fails until the round end advances the commit index only while leading"]
fn round_end_after_losing_leadership_does_not_commit() {
    let mut core: RaftCore<Cmd> = RaftCore::new();
    core.step::<Batch>(Event::SetIdentity { loc_id: 0, site_id: A, partition_id: 0 }, &mut out())
        .into_done();
    core.step::<Batch>(Event::Configure { members: &[0, 1, 2, 3, 4] }, &mut out()).into_done();
    assert!(core.step::<Batch>(Event::EnterGates { snapshots_enabled: false, failover: true },
                               &mut out()).into_gates());

    // Entries 1..8 everywhere, committed (here: from B, leader at term 1).
    let first: Vec<(i64, Cmd)> = (1..=8).map(|i| (1, Cmd(i))).collect();
    assert_eq!(append(&mut core, B, 1, 0, 0, first, 8), 1);
    assert_eq!(core.commit_index_, 8);

    // A leads term 2 and appends 9 and 10, replicating neither.
    win_election(&mut core);
    assert_eq!(core.current_term_, 2);
    for v in [9u64, 10] {
        core.step::<Batch>(
            Event::Propose { cmd: Cmd(v), has_value: true, is_tpc_commit: true, kind: 0, payload_bytes: 8 },
            &mut out()).into_proposed();
    }
    assert_eq!(core.raft_log_.last_index(), 10);

    // D campaigns at term 4 with its log 1..8; A steps down and refuses (its
    // log, ending at term 2, is ahead of D's, ending at term 1).
    request_vote(&mut core, D, 4, 8, 1);
    assert!(!core.is_leader_);
    assert_eq!(core.current_term_, 4);

    // A wins term 5 and opens a heartbeat round; B and C acknowledge 10.
    win_election(&mut core);
    assert_eq!(core.current_term_, 5);
    let tick = core.step::<Batch>(
        Event::TickHeartbeat {
            is_leader: true, snapshot_configured: false, batching: false,
            max_batch_entries: 256, max_batch_bytes: 16 << 20,
        },
        &mut out()).into_tick();
    assert!(!tick.declined_);
    assert_eq!(tick.sends_.len(), 4);
    for ord in [0usize, 1] {  // B and C
        core.step_checked::<Batch>(
            Event::RecvAppendReply {
                ord, status: true, term: 5, last_log_index: 10, is_leader: true,
                stopped: false, failover: true,
            },
            &mut out()).unwrap().into_append_reply();
    }
    // Entry 10 is of term 2, not 5: the leader rightly commits nothing.
    assert_eq!(core.commit_index_, 8);

    // The heartbeat driver now waits on mtx_ for the round end, while the
    // RPC handler takes D's messages. D wins term 7 (its last term, 4,
    // beats B's and C's): its RequestVote steps A down ...
    request_vote(&mut core, D, 7, 9, 4);
    assert!(!core.is_leader_);
    assert_eq!(core.current_term_, 7);
    // ... and its append replaces A's 9 and 10 with D's 9 (term 4) and its
    // no-op at 10 (term 7). D's commit index is 8.
    assert_eq!(append(&mut core, D, 7, 8, 1, vec![(4, Cmd(90)), (7, Cmd(100))], 8), 1);
    assert_eq!((term_at(&core, 9), term_at(&core, 10)), (4, 7));
    assert_eq!(core.commit_index_, 8);

    // The round end runs at last, no longer leading. Index 10 is held by A
    // and D only; it must not be committed.
    let advanced = core.step::<Batch>(Event::RoundEnd { is_leader: false }, &mut out())
        .into_round_end();
    assert!(!advanced && core.commit_index_ == 8,
            "B17: a server no longer leading committed index {} (advanced = {}), counting \
             term 5's match indices against entry 10 of term 7, which only A and D hold",
            core.commit_index_, advanced);
}
