// [fix, F9] step_checked against a core driven through step, as the shell
// drives it: the messages it drops leave the core untouched, and a success
// reply claiming more than the leader's log teaches the leader nothing. The
// lab (case 15) shows the drops end to end; a reply cannot be injected into
// a live round there, so its case is here (as F1's tally tests are in rt).

use raft_core::*;

#[derive(Clone, Debug, PartialEq)]
struct Cmd(u64);

// A payload-free append: no entries, every term valid.
struct NoEntries;

impl InboundBatch<Cmd> for NoEntries {
    fn decode_terms(&self, _prev: u64, terms: &mut Vec<i64>) -> bool {
        terms.clear();
        true
    }

    fn entry_at(&self, _k: u64) -> RaftEntry<Cmd> {
        unreachable!("an entry-less append has no entries")
    }
}

fn out() -> CoreOutput {
    CoreOutput::new()
}

// Site 1 of {1, 2, 3}, through Setup's three events.
fn configured() -> RaftCore<Cmd> {
    let mut core: RaftCore<Cmd> = RaftCore::new();
    core.step::<NoEntries>(Event::SetIdentity { loc_id: 0, site_id: 1, partition_id: 0 }, &mut out())
        .into_done();
    core.step::<NoEntries>(Event::Configure { members: &[1, 2, 3] }, &mut out()).into_done();
    assert!(core.step::<NoEntries>(Event::EnterGates { snapshots_enabled: false, failover: true },
                                   &mut out()).into_gates());
    core
}

// The same, elected by site 2's vote, with one proposal sent to both
// followers: next 1, prev 0, the entry at index 1.
fn leading() -> RaftCore<Cmd> {
    let mut core = configured();
    let c = core.step::<NoEntries>(
        Event::StartElection { timer_guarded: false, expected_generation: 0, now: 0, stopped: false },
        &mut out()).into_campaign();
    assert!(c.started_);
    let won = core.step::<NoEntries>(
        Event::SettleElection {
            term: c.term_, loc_id: 0, voters: &[2], granted: &[true],
            reply_terms: &[c.term_ as i64], n_total: 3, timed_out: false, stopped: false,
            looping: true, failover: true, election_debug: false,
        },
        &mut out()).into_settled();
    assert!(won && core.is_leader_);
    core.step::<NoEntries>(
        Event::Propose { cmd: Cmd(7), has_value: true, is_tpc_commit: false, kind: 0, payload_bytes: 8 },
        &mut out()).into_proposed();
    let tick = core.step::<NoEntries>(
        Event::TickHeartbeat {
            is_leader: true, snapshot_configured: false, batching: false,
            max_batch_entries: 256, max_batch_bytes: 16 << 20,
        },
        &mut out()).into_tick();
    assert!(!tick.declined_);
    assert_eq!(tick.sends_.len(), 2);
    assert_eq!(tick.sends_[0].sent_end_index_, 1);
    core
}

fn reply(core: &mut RaftCore<Cmd>, ord: usize, last_log_index: u64) -> ReplyResult {
    let term = core.current_term_;
    core.step_checked::<NoEntries>(
        Event::RecvAppendReply {
            ord, status: true, term, last_log_index, is_leader: true, stopped: false,
            failover: true,
        },
        &mut out()).unwrap().into_append_reply()
}

#[test]
fn a_success_beyond_the_leaders_log_is_no_reply() {
    let mut core = leading();
    // The leader's log ends at 1; this follower claims 5.
    reply(&mut core, 0, 5);
    assert_eq!(core.peers_.match_index(0), 0, "the claim moved the follower's match index");
    assert!(!core.pending_rpcs_.occupied(0), "the slot was not released");
    // The genuine reply for the other follower still counts.
    reply(&mut core, 1, 1);
    assert_eq!(core.peers_.match_index(1), 1);
}

fn append(core: &mut RaftCore<Cmd>, term: u64, site: u16, prev: u64, prev_term: u64)
    -> Option<Reply<Cmd>> {
    core.step_checked(
        Event::RecvAppendEntries {
            wire: &NoEntries, stopped: false, sender_is_current_voter: true, has_cmd: false,
            leader_current_term: term, leader_site_id: site, leader_prev_log_index: prev,
            leader_prev_log_term: prev_term, leader_commit_index: 0, failover: true,
        },
        &mut out())
}

fn vote(core: &mut RaftCore<Cmd>, site: u16, term: i64) -> Option<Reply<Cmd>> {
    core.step_checked::<NoEntries>(
        Event::RecvRequestVote {
            stopped: false, candidate_is_current_voter: true, lst_log_idx: 0, lst_log_term: 0,
            can_id: site, can_term: term, failover: true, election_debug: false,
        },
        &mut out())
}

#[test]
fn unadmitted_messages_are_dropped_untouched() {
    let mut core = configured();
    assert!(append(&mut core, 1, 9, 0, 0).is_none(), "a stranger's append was taken");
    assert!(append(&mut core, 1, 1, 0, 0).is_none(), "an append from this server was taken");
    assert!(append(&mut core, 0, 2, 0, 0).is_none(), "a term-0 append was taken");
    assert!(append(&mut core, 1, 2, 0, 3).is_none(), "prev 0 with a prev term was taken");
    assert!(append(&mut core, 1, 2, 4, 0).is_none(), "prev 4 with prev term 0 was taken");
    assert!(vote(&mut core, 9, 3).is_none(), "a stranger's vote request was taken");
    assert!(vote(&mut core, 2, 0).is_none(), "a term-0 vote request was taken");
    // None of them moved the term, the vote, the leader hint or the log.
    assert_eq!(core.current_term_, 0);
    assert_eq!(core.vote_for_, u16::MAX);
    assert_eq!(core.current_leader_id_, u16::MAX);
    assert_eq!(core.raft_log_.last_index(), 0);
    // The admitted ones are taken.
    assert!(matches!(append(&mut core, 1, 2, 0, 0), Some(Reply::Append { ok: 1, .. })));
    assert_eq!(core.current_term_, 1);
    assert!(matches!(vote(&mut core, 3, 2), Some(Reply::Vote { granted: 1, .. })));
}
