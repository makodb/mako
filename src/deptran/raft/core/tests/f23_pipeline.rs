// F23 (docs/verus/modification-plan.md A.2; bugs-found B53): pipelined
// AppendEntries. A follower with batches in flight is sent another only once
// the newest of them has been in flight for `pipeline_after_us`, within
// PIPELINE_WINDOW and the byte cap, and only to carry entries it has not been
// sent. Each send with entries moves the follower's next index past its
// batch; a reject backs off from its own send's start, once, and a lost reply
// sends its batch again.

use raft_core::*;

#[derive(Clone, Debug, PartialEq)]
struct Cmd(u64);

fn out() -> CoreOutput {
    CoreOutput::new()
}

struct NoEntries;

impl InboundBatch<Cmd> for NoEntries {
    fn decode_terms(&self, _prev: u64, terms: &mut Vec<i64>) -> bool {
        terms.clear();
        true
    }

    fn entry_at(&self, _k: u64) -> RaftEntry<Cmd> {
        unreachable!("no entries")
    }
}

// Sites: A = 0 (this core, the leader), B = 1 (ordinal 0), C = 2 (ordinal 1).
const B: u16 = 1;
const AFTER: u64 = 5_000;

fn leader() -> RaftCore<Cmd> {
    let mut core: RaftCore<Cmd> = RaftCore::new();
    core.step::<NoEntries>(Event::SetIdentity { loc_id: 0, site_id: 0, partition_id: 0 }, &mut out())
        .into_done();
    core.step::<NoEntries>(Event::Configure { members: &[0, 1, 2] }, &mut out()).into_done();
    assert!(core.step::<NoEntries>(Event::EnterGates { unsaved_restart: false, failover: true },
                                   &mut out()).into_gates());
    let c = core.step::<NoEntries>(
        Event::StartElection { timer_guarded: false, expected_generation: 0, now: 0, stopped: false },
        &mut out()).into_campaign();
    assert!(c.started_);
    let won = core.step::<NoEntries>(
        Event::SettleElection {
            term: c.term_, loc_id: 0, voters: &[1, 2], granted: &[true, true],
            reply_terms: &[c.term_ as i64, c.term_ as i64], n_total: 3, timed_out: false,
            stopped: false, looping: true, failover: true, election_debug: false,
        },
        &mut out()).into_settled();
    assert!(won && core.is_leader_);
    core
}

fn propose(core: &mut RaftCore<Cmd>, from: u64, to: u64, bytes: u64) {
    for v in from..=to {
        core.step::<NoEntries>(
            Event::Propose { cmd: Cmd(v), has_value: true, is_tpc_commit: true, kind: 0,
                             payload_bytes: bytes },
            &mut out()).into_proposed();
    }
    assert_eq!(core.raft_log_.last_index(), to);
}

fn tick(core: &mut RaftCore<Cmd>, now_us: u64, pipeline_after_us: u64, batching: bool,
        max_batch_bytes: u64) -> HeartbeatTick<Cmd> {
    let t = core.step::<NoEntries>(
        Event::TickHeartbeat {
            is_leader: true, snapshot_configured: false, batching, max_batch_entries: 256,
            max_batch_bytes, now_us, pipeline_after_us,
        },
        &mut out()).into_tick();
    assert!(!t.declined_);
    t
}

fn tick_batched(core: &mut RaftCore<Cmd>, now_us: u64) -> HeartbeatTick<Cmd> {
    tick(core, now_us, AFTER, true, 16 << 20)
}

fn reply(core: &mut RaftCore<Cmd>, slot: usize, ok: bool, term: u64, last: u64) -> ReplyResult {
    core.step_checked::<NoEntries>(
        Event::RecvAppendReply { ord: slot, status: ok, term, last_log_index: last,
                                 is_leader: true, stopped: false, failover: true },
        &mut out()).expect("a reply the core takes").into_append_reply()
}

// The one send to `site` in a tick: (slot, prev, end).
fn send_to(t: &HeartbeatTick<Cmd>, site: u16) -> Option<(usize, u64, u64)> {
    let s: Vec<_> = t.sends_.iter().filter(|s| s.site_id_ == site).collect();
    assert!(s.len() <= 1, "one send per follower per tick");
    s.first().map(|s| (s.slot_, s.prev_log_index_, s.sent_end_index_))
}

fn in_flight(core: &RaftCore<Cmd>, site: u16) -> usize {
    core.pending_rpcs_.in_flight(site).count_
}

#[test]
fn a_second_batch_waits_for_the_interval_and_for_new_entries() {
    let mut core = leader();
    propose(&mut core, 1, 3, 8);
    let t1 = tick_batched(&mut core, 1_000);
    let (b1, prev, end) = send_to(&t1, B).expect("B is sent 1..3");
    assert_eq!((prev, end), (0, 3));
    assert_eq!(core.peers_.next_index(0), 4, "next moves past the batch at the send");

    // New entries, but B's batch has been in flight for less than the
    // interval: nothing is sent.
    propose(&mut core, 4, 5, 8);
    assert!(send_to(&tick_batched(&mut core, 1_000 + AFTER - 1), B).is_none());

    // Once it has, B gets 4..5 behind 1..3, in another slot.
    let t2 = tick_batched(&mut core, 1_000 + AFTER);
    let (b2, prev, end) = send_to(&t2, B).expect("B is sent 4..5 while 1..3 is in flight");
    assert_eq!((prev, end), (3, 5));
    assert_ne!(b1, b2);
    assert_eq!(in_flight(&core, B), 2);
    assert_eq!(core.peers_.next_index(0), 6);

    // Nothing new: no pipelined heartbeat.
    assert!(send_to(&tick_batched(&mut core, 1_000 + 3 * AFTER), B).is_none());

    // The replies, in order: each raises the match; next stays past both.
    let term = core.current_term_;
    reply(&mut core, b1, true, term, 3);
    assert_eq!((core.peers_.match_index(0), core.peers_.next_index(0)), (3, 6));
    reply(&mut core, b2, true, term, 5);
    assert_eq!((core.peers_.match_index(0), core.peers_.next_index(0)), (5, 6));
    assert_eq!(in_flight(&core, B), 0);
}

#[test]
fn pipelining_off_keeps_one_batch_in_flight() {
    let mut core = leader();
    propose(&mut core, 1, 3, 8);
    assert!(send_to(&tick(&mut core, 1_000, 0, true, 16 << 20), B).is_some());
    propose(&mut core, 4, 5, 8);
    for k in 1..4 {
        assert!(send_to(&tick(&mut core, 1_000 + k * AFTER, 0, true, 16 << 20), B).is_none());
    }
    assert_eq!(in_flight(&core, B), 1);
}

#[test]
fn the_window_bounds_the_batches_in_flight() {
    let mut core = leader();
    // One entry per send (batching off), a new one before every tick.
    let mut sent = 0;
    for k in 1..=(PIPELINE_WINDOW as u64 + 8) {
        propose(&mut core, k, k, 8);
        if send_to(&tick(&mut core, k * AFTER, AFTER, false, 16 << 20), B).is_some() {
            sent += 1;
        }
    }
    assert_eq!(sent, PIPELINE_WINDOW);
    assert_eq!(in_flight(&core, B), PIPELINE_WINDOW);
}

#[test]
fn the_byte_cap_bounds_the_bytes_in_flight() {
    let mut core = leader();
    // 8-byte entries, one per send, and a 20-byte cap: the first send always
    // goes, then more while fewer than 20 bytes are in flight (8, then 16).
    for k in 1..=10u64 {
        propose(&mut core, k, k, 8);
        tick(&mut core, k * AFTER, AFTER, false, 20);
    }
    assert_eq!(in_flight(&core, B), 3);
}

// B accepts 1..3, then loses 3 (a restart that kept 1..2) while 4..5 and
// 6..7 are in flight behind it, and rejects both reporting last index 2.
fn two_rejected_batches(oldest_first: bool) -> RaftCore<Cmd> {
    let mut core = leader();
    let term = core.current_term_;
    propose(&mut core, 1, 3, 8);
    let (b1, _, _) = send_to(&tick_batched(&mut core, 1_000), B).unwrap();
    propose(&mut core, 4, 5, 8);
    let (b2, _, _) = send_to(&tick_batched(&mut core, 1_000 + AFTER), B).unwrap();
    propose(&mut core, 6, 7, 8);
    let (b3, prev, _) = send_to(&tick_batched(&mut core, 1_000 + 2 * AFTER), B).unwrap();
    assert_eq!(prev, 5);
    assert_eq!(in_flight(&core, B), 3);
    reply(&mut core, b1, true, term, 3);
    assert_eq!((core.peers_.match_index(0), core.peers_.next_index(0)), (3, 8));

    let (first, second) = if oldest_first { (b2, b3) } else { (b3, b2) };
    reply(&mut core, first, false, term, 2);
    assert_eq!(core.peers_.next_index(0), 3,
               "FAST back-off to B's end (oldest first: {oldest_first})");
    reply(&mut core, second, false, term, 2);
    assert_eq!(core.peers_.next_index(0), 3,
               "the other reject backs off no further (oldest first: {oldest_first})");
    core
}

#[test]
fn a_reject_backs_off_once_whatever_the_reply_order() {
    for oldest_first in [true, false] {
        let mut core = two_rejected_batches(oldest_first);
        // The next tick sends B everything from 3.
        let (_, prev, end) = send_to(&tick_batched(&mut core, 1_000 + 3 * AFTER), B).unwrap();
        assert_eq!((prev, end), (2, 7));
    }
}

#[test]
fn a_stale_reject_does_not_move_next_back_up() {
    // Rejects that allow no FAST back-off (B reports last index 0): 4..5's
    // steps next down from its start, 4, to 3. 6..7's, sent before that,
    // must not then reset next to its own start, 6, and step to 5.
    let mut core = leader();
    let term = core.current_term_;
    propose(&mut core, 1, 3, 8);
    let (b1, _, _) = send_to(&tick_batched(&mut core, 1_000), B).unwrap();
    propose(&mut core, 4, 5, 8);
    let (b2, _, _) = send_to(&tick_batched(&mut core, 1_000 + AFTER), B).unwrap();
    propose(&mut core, 6, 7, 8);
    let (b3, _, _) = send_to(&tick_batched(&mut core, 1_000 + 2 * AFTER), B).unwrap();
    reply(&mut core, b1, true, term, 3);
    reply(&mut core, b2, false, term, 0);
    assert_eq!(core.peers_.next_index(0), 3);
    reply(&mut core, b3, false, term, 0);
    assert_eq!(core.peers_.next_index(0), 3);
}

#[test]
fn a_reject_backs_off_from_its_own_send_not_from_next() {
    // A reject whose report does not allow a FAST back-off: without the
    // reset to the send's start the ladder would step from the index the
    // later batch moved next to (6, then 5), not from the rejected batch's
    // start (1, the floor).
    let mut core = leader();
    let term = core.current_term_;
    propose(&mut core, 1, 3, 8);
    let (b1, _, _) = send_to(&tick_batched(&mut core, 1_000), B).unwrap();
    propose(&mut core, 4, 5, 8);
    send_to(&tick_batched(&mut core, 1_000 + AFTER), B).unwrap();
    assert_eq!(core.peers_.next_index(0), 6);
    reply(&mut core, b1, false, term, 9);
    assert_eq!(core.peers_.next_index(0), 1);
}

#[test]
fn a_lost_reply_sends_its_batch_again() {
    let mut core = leader();
    propose(&mut core, 1, 3, 8);
    let (b1, _, _) = send_to(&tick_batched(&mut core, 1_000), B).unwrap();
    assert_eq!(core.peers_.next_index(0), 4);
    // An unavailable reply (the shell reads a failed RPC as 0/0/0).
    reply(&mut core, b1, false, 0, 0);
    assert_eq!(core.peers_.next_index(0), 1);
    let (_, prev, end) = send_to(&tick_batched(&mut core, 1_000 + AFTER), B).unwrap();
    assert_eq!((prev, end), (0, 3));
}
