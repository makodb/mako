//! A recording with the disk events (Restore, ObserveTerm) and the persist
//! notes' `P` lines replays cleanly, and the shadow catches a `P` line that
//! was tampered with or dropped: the replay is the evidence the host
//! contract cites for every note being exact.

use raft_core::*;
use raft_replay::*;

struct NoEntries;
impl InboundBatch<ReplayCmd> for NoEntries {
    fn decode_terms(&self, _p: u64, t: &mut Vec<i64>) -> bool {
        t.clear();
        true
    }
    fn entry_at(&self, _k: u64) -> RaftEntry<ReplayCmd> {
        unreachable!()
    }
}

fn digest(c: &ReplayCmd) -> u64 {
    c.0
}

/// Steps the core and records it as the shell's step wrapper does: the
/// record line, then the step's persist note as a `P` line.
fn rec(core: &mut RaftCore<ReplayCmd>, ev: Event<'_, ReplayCmd, NoEntries>, text: &mut String) -> Reply<ReplayCmd> {
    let mut e = String::new();
    write_event(&mut e, &ev, &digest);
    let mut out = CoreOutput::new();
    let r = core.step(ev, &mut out);
    let res = result_text(&out, 0, 0, &r, &digest);
    text.push_str(&record_line(&e, out.log_level(), &res));
    text.push('\n');
    if let Some(n) = out.persist() {
        text.push_str(&persist_line(&n));
        text.push('\n');
    }
    r
}

#[test]
fn restore_observe_term_and_notes_replay() {
    let mut core: RaftCore<ReplayCmd> = RaftCore::new();
    let mut t = String::new();
    rec(&mut core, Event::SetIdentity { loc_id: 0, site_id: 1, partition_id: 0 }, &mut t);
    rec(&mut core, Event::Configure { members: &[1, 2, 3] }, &mut t);
    let mut rev = vec![RaftEntry::new(1, ReplayCmd(11), true, false, 0, 8),
                       RaftEntry::new(2, ReplayCmd(12), true, false, 0, 8)];
    rev.reverse();
    assert!(rec(&mut core, Event::Restore { term: 3, vote: 2, commit: 1, entries_rev: rev }, &mut t).into_restored());
    rec(&mut core, Event::EnterGates { snapshots_enabled: false, failover: true }, &mut t);
    rec(&mut core, Event::ObserveTerm { term: 5, stopped: false, failover: true }, &mut t);
    let c = rec(&mut core, Event::StartElection { timer_guarded: false, expected_generation: 0, now: 1,
                                                  stopped: false }, &mut t).into_campaign();
    rec(&mut core, Event::SettleElection { term: c.term_, loc_id: 0, voters: &[2], granted: &[true],
                                          reply_terms: &[c.term_ as i64], n_total: 3, timed_out: false,
                                          stopped: false, looping: true, failover: true,
                                          election_debug: false }, &mut t);
    rec(&mut core, Event::Propose { cmd: ReplayCmd(77), has_value: true, is_tpc_commit: false, kind: 0,
                                    payload_bytes: 8 }, &mut t);
    assert!(t.contains("\nP "), "the recording holds persist notes");
    replay(&t).unwrap_or_else(|m| panic!("line {}: {} | {} | {}", m.line, m.event, m.recorded, m.replayed));

    let i = t.find("\nP ").unwrap() + 1;
    let end = t[i..].find('\n').unwrap() + i;
    let mut tampered = t.clone();
    tampered.replace_range(i..end, "P 1 9 9 9 9");
    assert!(replay(&tampered).is_err(), "a tampered P line replayed");
    let mut dropped = t.clone();
    dropped.replace_range(i..end + 1, "");
    assert!(replay(&dropped).is_err(), "a dropped P line replayed");
}
