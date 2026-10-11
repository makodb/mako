//! The Raft core's history as text (docs/verus/modification-plan.md A.4).
//!
//! One record per `RaftCore::step`, one line each:
//!
//! ```text
//! E <event> | <log level> | <actions and log lines> R <reply>
//! ```
//!
//! The shell's recorder (env-gated, `MAKO_RAFT_REPLAY_DIR`) writes the event
//! as the shell built it -- an inbound batch as its decoded terms and
//! entries, a command as the digest of its wire bytes -- then what the core
//! produced: the actions and log lines the call appended to its CoreOutput,
//! and its Reply. [`replay`] feeds each recorded event to a fresh core over
//! [`ReplayCmd`]s and compares the text of what that core produces with the
//! recorded text, byte for byte.
//!
//! A `T <why>` line marks a write to the core that did not go through step
//! (the snapshot paths, outside the verified configuration). The history is
//! incomplete from there, so a replay stops at it.

use raft_core::*;

/// What a recording keeps of a command: a 64-bit digest of its wire bytes,
/// computed by the caller's function (the shell's over its command handle).
pub type Digest<'a, C> = &'a dyn Fn(&C) -> u64;

/// The replay's command: the recorded digest, which is all the core's
/// output can show of a command.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReplayCmd(pub u64);

fn replay_digest(cmd: &ReplayCmd) -> u64 {
    cmd.0
}

/// FNV-1a, 64 bits: the digest a recorder computes over a command's bytes.
pub struct Fnv64(pub u64);

impl Default for Fnv64 {
    fn default() -> Self {
        Fnv64(0xcbf2_9ce4_8422_2325)
    }
}

impl Fnv64 {
    pub fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.0 ^= byte as u64;
            self.0 = self.0.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

fn b(x: bool) -> &'static str {
    if x { "1" } else { "0" }
}

fn put(s: &mut String, token: &str) {
    s.push(' ');
    s.push_str(token);
}

fn put_u(s: &mut String, x: u64) {
    s.push(' ');
    s.push_str(&x.to_string());
}

fn put_i(s: &mut String, x: i64) {
    s.push(' ');
    s.push_str(&x.to_string());
}

/// A string as one token: space, '%', '|' and every byte outside printable
/// ASCII become %XX.
fn encode(s: &mut String, text: &str) {
    for &byte in text.as_bytes() {
        if byte == b' ' || byte == b'%' || byte == b'|' || !(0x21..0x7f).contains(&byte) {
            s.push_str(&format!("%{:02X}", byte));
        } else {
            s.push(byte as char);
        }
    }
}

#[cfg(test)]
fn decode(token: &str) -> String {
    let bytes = token.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap();
            out.push(u8::from_str_radix(hex, 16).unwrap());
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8(out).unwrap()
}

/// The event, as `E <name> <fields>...`. An inbound batch is decoded here,
/// exactly as the core will decode it, and its entries read: the recorder
/// pays that, a run without one does not.
pub fn write_event<C: Clone, W: InboundBatch<C>>(s: &mut String, ev: &Event<'_, C, W>,
                                                 digest: Digest<'_, C>) {
    s.push('E');
    match ev {
        Event::SetIdentity { loc_id, site_id, partition_id } => {
            put(s, "identity");
            put_u(s, *loc_id as u64);
            put_u(s, *site_id as u64);
            put_u(s, *partition_id as u64);
        }
        Event::Configure { members } => {
            put(s, "configure");
            put_u(s, members.len() as u64);
            for m in members.iter() {
                put_u(s, *m as u64);
            }
        }
        Event::EnterGates { snapshots_enabled, failover } => {
            put(s, "gates");
            put(s, b(*snapshots_enabled));
            put(s, b(*failover));
        }
        Event::RebuildPeers { next_index } => {
            put(s, "peers");
            put_u(s, *next_index);
        }
        Event::Propose { cmd, has_value, is_tpc_commit, kind, payload_bytes } => {
            put(s, "propose");
            put_u(s, digest(cmd));
            put(s, b(*has_value));
            put(s, b(*is_tpc_commit));
            put_i(s, *kind as i64);
            put_u(s, *payload_bytes);
        }
        Event::StartElection { timer_guarded, expected_generation, now, stopped } => {
            put(s, "campaign");
            put(s, b(*timer_guarded));
            put_u(s, *expected_generation);
            put_u(s, *now);
            put(s, b(*stopped));
        }
        Event::SettleElection {
            term, loc_id, voters, granted, reply_terms, n_total, timed_out, stopped,
            looping, failover, election_debug,
        } => {
            put(s, "settle");
            put_u(s, *term);
            put_u(s, *loc_id as u64);
            put_u(s, *n_total);
            put(s, b(*timed_out));
            put(s, b(*stopped));
            put(s, b(*looping));
            put(s, b(*failover));
            put(s, b(*election_debug));
            put_u(s, voters.len() as u64);
            for k in 0..voters.len() {
                put_u(s, voters[k] as u64);
                put(s, b(granted[k]));
                put_i(s, reply_terms[k]);
            }
        }
        Event::ResetElectionTimer { now, timeout_us } => {
            put(s, "timer");
            put_u(s, *now);
            put_u(s, *timeout_us);
        }
        Event::RecvRequestVote {
            stopped, candidate_is_current_voter, lst_log_idx, lst_log_term, can_id,
            can_term, failover, election_debug,
        } => {
            put(s, "vote");
            put(s, b(*stopped));
            put(s, b(*candidate_is_current_voter));
            put_u(s, *lst_log_idx);
            put_i(s, *lst_log_term);
            put_u(s, *can_id as u64);
            put_i(s, *can_term);
            put(s, b(*failover));
            put(s, b(*election_debug));
        }
        Event::RecvAppendEntries {
            wire, stopped, sender_is_current_voter, has_cmd, leader_current_term,
            leader_site_id, leader_prev_log_index, leader_prev_log_term,
            leader_commit_index, failover,
        } => {
            put(s, "append");
            put(s, b(*stopped));
            put(s, b(*sender_is_current_voter));
            put(s, b(*has_cmd));
            put_u(s, *leader_current_term);
            put_u(s, *leader_site_id as u64);
            put_u(s, *leader_prev_log_index);
            put_u(s, *leader_prev_log_term);
            put_u(s, *leader_commit_index);
            put(s, b(*failover));
            let mut terms: Vec<i64> = Vec::new();
            let valid = wire.decode_terms(*leader_prev_log_index, &mut terms);
            put(s, b(valid));
            put_u(s, terms.len() as u64);
            for t in terms.iter() {
                put_i(s, *t);
            }
            // The entries the core may materialize: only a valid payload's.
            let n = if valid { terms.len() } else { 0 };
            put_u(s, n as u64);
            for k in 0..n {
                let entry = wire.entry_at(k as u64);
                put(s, b(entry.has_value()));
                put(s, b(entry.is_tpc_commit()));
                put_i(s, entry.kind() as i64);
                put_u(s, entry.payload_bytes());
                put_u(s, digest(entry.cmd()));
            }
        }
        Event::TickHeartbeat {
            is_leader, snapshot_configured, batching, max_batch_entries, max_batch_bytes,
        } => {
            put(s, "tick");
            put(s, b(*is_leader));
            put(s, b(*snapshot_configured));
            put(s, b(*batching));
            put_u(s, *max_batch_entries);
            put_u(s, *max_batch_bytes);
        }
        Event::RecvAppendReply { ord, status, term, last_log_index, is_leader, stopped, failover } => {
            put(s, "reply");
            put_u(s, *ord as u64);
            put(s, b(*status));
            put_u(s, *term);
            put_u(s, *last_log_index);
            put(s, b(*is_leader));
            put(s, b(*stopped));
            put(s, b(*failover));
        }
        Event::AbandonRound => put(s, "abandon"),
        Event::RoundEnd { is_leader } => {
            put(s, "round_end");
            put(s, b(*is_leader));
        }
        Event::ResetRoundState => put(s, "reset_round"),
        Event::Applied { index, published } => {
            put(s, "applied");
            put_u(s, *index);
            put_u(s, *published);
        }
        Event::SetFollower { stopped, failover } => {
            put(s, "follower");
            put(s, b(*stopped));
            put(s, b(*failover));
        }
        Event::StepDown { stopped, failover } => {
            put(s, "step_down");
            put(s, b(*stopped));
            put(s, b(*failover));
        }
        Event::ObserveTerm { term, stopped, failover } => {
            put(s, "observe_term");
            put_u(s, *term);
            put(s, b(*stopped));
            put(s, b(*failover));
        }
        Event::Restore { term, vote, commit, entries_rev } => {
            put(s, "restore");
            put_u(s, *term);
            put_u(s, *vote as u64);
            put_u(s, *commit);
            put_u(s, entries_rev.len() as u64);
            for e in entries_rev.iter() {
                put_i(s, e.term());
                put(s, b(e.has_value()));
                put(s, b(e.is_tpc_commit()));
                put_i(s, e.kind() as i64);
                put_u(s, e.payload_bytes());
                put_u(s, digest(e.cmd()));
            }
        }
    }
}

/// A step's persist note ([fix, F21]): `P <hard> <term> <vote> <commit>
/// <log_from>`, on its own line after the step's record.
pub fn persist_line(note: &PersistNote) -> String {
    format!("P {} {} {} {} {}", b(note.hard_), note.term_, note.vote_, note.commit_, note.log_from_)
}

/// A message handed to `step_checked` ([fix, F9]): `C <name> <fields>...`.
pub fn write_checked_event<C: Clone, W: InboundBatch<C>>(s: &mut String,
                                                         ev: &Event<'_, C, W>,
                                                         digest: Digest<'_, C>) {
    write_event(s, ev, digest);
    s.replace_range(0..1, "C");
}

/// `step_checked`'s result: `R dropped` for a message the core refused,
/// else as [`result_text`].
pub fn checked_result_text<C>(out: &CoreOutput, actions_from: usize, logs_from: usize,
                              reply: &Option<Reply<C>>, digest: Digest<'_, C>) -> String {
    match reply {
        Some(r) => result_text(out, actions_from, logs_from, r, digest),
        None => {
            let mut text = String::new();
            let s = &mut text;
            write_outputs(s, out, actions_from, logs_from);
            put(s, "R");
            put(s, "dropped");
            text[1..].to_string()
        }
    }
}

/// One record: the event's text (from [`write_event`]), the log level the
/// call's CoreOutput kept (which decides its log lines), and the result's
/// text (from [`result_text`]).
pub fn record_line(event: &str, level: i32, result: &str) -> String {
    format!("{event} | {level} | {result}")
}

/// What the call appended to `out` (actions from `actions_from`, log lines
/// from `logs_from`), then its reply: `A ...` per action, `L ...` per log
/// line, `R <reply>`.
pub fn result_text<C>(out: &CoreOutput, actions_from: usize, logs_from: usize,
                      reply: &Reply<C>, digest: Digest<'_, C>) -> String {
    let mut text = String::new();
    let s = &mut text;
    write_outputs(s, out, actions_from, logs_from);
    put(s, "R");
    write_reply(s, reply, digest);
    // every token went in with a space before it
    text[1..].to_string()
}

fn write_outputs(s: &mut String, out: &CoreOutput, actions_from: usize, logs_from: usize) {
    for i in actions_from..out.len() {
        let a = out.at(i);
        put(s, "A");
        put_i(s, a.kind() as i64);
        put_u(s, a.from());
        put_u(s, a.to());
        put_i(s, a.reason() as i64);
        put_u(s, a.term());
        put(s, b(a.prev_is_leader()));
        put(s, b(a.is_leader()));
        put(s, b(a.became_leader()));
        put(s, b(a.became_follower()));
    }
    for i in logs_from..out.log_count() {
        let l = out.log_at(i);
        put(s, "L");
        put_i(s, l.level as i64);
        s.push(' ');
        encode(s, l.fmt);
        put_u(s, l.nargs as u64);
        for arg in l.args.iter().take(l.nargs) {
            s.push(' ');
            match arg {
                LogArg::U(x) => s.push_str(&format!("u{}", x)),
                LogArg::I(x) => s.push_str(&format!("i{}", x)),
                LogArg::B(x) => s.push_str(if *x { "b1" } else { "b0" }),
                LogArg::S(text) => {
                    s.push('s');
                    encode(s, text);
                }
            }
        }
    }
}

fn write_reply<C>(s: &mut String, reply: &Reply<C>, digest: Digest<'_, C>) {
    match reply {
        Reply::Done => put(s, "done"),
        Reply::Gates(ok) => {
            put(s, "gates");
            put(s, b(*ok));
        }
        Reply::Proposed(prev) => {
            put(s, "proposed");
            put_u(s, *prev);
        }
        Reply::Campaign(c) => {
            put(s, "campaign");
            put(s, b(c.started_));
            put_u(s, c.term_);
            put_u(s, c.prev_term_);
            put_u(s, c.prev_vote_for_ as u64);
            put_u(s, c.lst_idx_);
            put_i(s, c.lst_term_);
        }
        Reply::Settled(won) => {
            put(s, "settled");
            put(s, b(*won));
        }
        Reply::TimerReset(prev) => {
            put(s, "timer_reset");
            put_u(s, *prev);
        }
        Reply::Vote { term, granted } => {
            put(s, "vote");
            put_i(s, *term);
            put_i(s, *granted as i64);
        }
        Reply::Append { report, ok, term, last_log_index } => {
            put(s, "append");
            put(s, b(report.accepted()));
            put(s, b(report.term_ok()));
            put(s, b(report.index_ok()));
            put(s, b(report.prev_term_ok()));
            put(s, b(report.refused_committed_conflict()));
            put(s, b(report.unauthoritative()));
            put_u(s, report.conflict_index());
            put_u(s, report.local_prev_term());
            put_u(s, *ok);
            put_u(s, *term);
            put_u(s, *last_log_index);
        }
        Reply::Tick(t) => {
            put(s, "tick");
            put(s, b(t.declined_));
            put(s, b(t.slots_reset_));
            put_u(s, t.slot_count_ as u64);
            put_u(s, t.round_id_);
            put(s, b(t.has_authority_));
            put_u(s, t.snapshots_.len() as u64);
            for snap in t.snapshots_.iter() {
                put_u(s, snap.ord_ as u64);
                put_u(s, snap.site_id_ as u64);
                put_u(s, snap.term_);
            }
            put_u(s, t.sends_.len() as u64);
            for send in t.sends_.iter() {
                put_u(s, send.ord_ as u64);
                put_u(s, send.site_id_ as u64);
                put_i(s, send.payload_ as i64);
                put_u(s, send.term_);
                put_u(s, send.prev_log_index_);
                put_u(s, send.prev_log_term_);
                put_u(s, send.commit_index_);
                put_u(s, send.entry_term_);
                put_u(s, send.sent_end_index_);
                put_u(s, send.sent_round_);
                put_u(s, send.cmds_.len() as u64);
                for cmd in send.cmds_.iter() {
                    put_u(s, digest(cmd));
                }
                put_u(s, send.terms_.len() as u64);
                for term in send.terms_.iter() {
                    put_i(s, *term);
                }
            }
        }
        Reply::AppendReply(r) => {
            put(s, "append_reply");
            put(s, b(r.stepped_down_));
            put(s, b(r.completed_previous_round_));
            put(s, b(r.has_authority_));
        }
        Reply::RoundEnd(advanced) => {
            put(s, "round_end");
            put(s, b(*advanced));
        }
        Reply::Applied(recorded) => {
            put(s, "applied");
            put(s, b(*recorded));
        }
        Reply::Restored(ok) => {
            put(s, "restored");
            put(s, b(*ok));
        }
    }
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/// A recorded inbound batch: what the decoder returned, and the entries.
pub struct ReplayBatch {
    pub valid: bool,
    pub terms: Vec<i64>,
    // (has_value, is_tpc_commit, kind, payload_bytes, digest), one per term
    // of a valid payload.
    pub entries: Vec<(bool, bool, i32, u64, u64)>,
}

impl InboundBatch<ReplayCmd> for ReplayBatch {
    fn decode_terms(&self, _leader_prev_log_index: u64, terms: &mut Vec<i64>) -> bool {
        terms.clear();
        terms.extend_from_slice(&self.terms);
        self.valid
    }

    fn entry_at(&self, k: u64) -> RaftEntry<ReplayCmd> {
        let (has_value, is_tpc_commit, kind, payload_bytes, digest) = self.entries[k as usize];
        RaftEntry::new(self.terms[k as usize], ReplayCmd(digest), has_value, is_tpc_commit,
                       kind, payload_bytes)
    }
}

/// A recorded event, owning what the Event borrows.
pub enum OwnedEvent {
    SetIdentity { loc_id: u32, site_id: u16, partition_id: u32 },
    Configure { members: Vec<u16> },
    EnterGates { snapshots_enabled: bool, failover: bool },
    RebuildPeers { next_index: u64 },
    Propose { cmd: ReplayCmd, has_value: bool, is_tpc_commit: bool, kind: i32, payload_bytes: u64 },
    StartElection { timer_guarded: bool, expected_generation: u64, now: u64, stopped: bool },
    SettleElection {
        term: u64, loc_id: u32, voters: Vec<u16>, granted: Vec<bool>, reply_terms: Vec<i64>,
        n_total: u64, timed_out: bool, stopped: bool, looping: bool, failover: bool,
        election_debug: bool,
    },
    ResetElectionTimer { now: u64, timeout_us: u64 },
    RecvRequestVote {
        stopped: bool, candidate_is_current_voter: bool, lst_log_idx: u64, lst_log_term: i64,
        can_id: u16, can_term: i64, failover: bool, election_debug: bool,
    },
    RecvAppendEntries {
        wire: ReplayBatch, stopped: bool, sender_is_current_voter: bool, has_cmd: bool,
        leader_current_term: u64, leader_site_id: u16, leader_prev_log_index: u64,
        leader_prev_log_term: u64, leader_commit_index: u64, failover: bool,
    },
    TickHeartbeat {
        is_leader: bool, snapshot_configured: bool, batching: bool, max_batch_entries: u64,
        max_batch_bytes: u64,
    },
    RecvAppendReply {
        ord: usize, status: bool, term: u64, last_log_index: u64, is_leader: bool,
        stopped: bool, failover: bool,
    },
    AbandonRound,
    RoundEnd { is_leader: bool },
    ResetRoundState,
    Applied { index: u64, published: u64 },
    SetFollower { stopped: bool, failover: bool },
    StepDown { stopped: bool, failover: bool },
    ObserveTerm { term: u64, stopped: bool, failover: bool },
    // entries last first, as Event::Restore takes them: (term, has_value,
    // is_tpc_commit, kind, payload_bytes, digest)
    Restore { term: u64, vote: u16, commit: u64, entries_rev: Vec<(i64, bool, bool, i32, u64, u64)> },
}

impl OwnedEvent {
    pub fn as_event(&self) -> Event<'_, ReplayCmd, ReplayBatch> {
        match self {
            OwnedEvent::SetIdentity { loc_id, site_id, partition_id } => Event::SetIdentity {
                loc_id: *loc_id, site_id: *site_id, partition_id: *partition_id,
            },
            OwnedEvent::Configure { members } => Event::Configure { members },
            OwnedEvent::EnterGates { snapshots_enabled, failover } => Event::EnterGates {
                snapshots_enabled: *snapshots_enabled, failover: *failover,
            },
            OwnedEvent::RebuildPeers { next_index } => Event::RebuildPeers { next_index: *next_index },
            OwnedEvent::Propose { cmd, has_value, is_tpc_commit, kind, payload_bytes } => Event::Propose {
                cmd: *cmd, has_value: *has_value, is_tpc_commit: *is_tpc_commit, kind: *kind,
                payload_bytes: *payload_bytes,
            },
            OwnedEvent::StartElection { timer_guarded, expected_generation, now, stopped } => {
                Event::StartElection {
                    timer_guarded: *timer_guarded, expected_generation: *expected_generation,
                    now: *now, stopped: *stopped,
                }
            }
            OwnedEvent::SettleElection {
                term, loc_id, voters, granted, reply_terms, n_total, timed_out, stopped, looping,
                failover, election_debug,
            } => Event::SettleElection {
                term: *term, loc_id: *loc_id, voters, granted, reply_terms, n_total: *n_total,
                timed_out: *timed_out, stopped: *stopped, looping: *looping, failover: *failover,
                election_debug: *election_debug,
            },
            OwnedEvent::ResetElectionTimer { now, timeout_us } => Event::ResetElectionTimer {
                now: *now, timeout_us: *timeout_us,
            },
            OwnedEvent::RecvRequestVote {
                stopped, candidate_is_current_voter, lst_log_idx, lst_log_term, can_id, can_term,
                failover, election_debug,
            } => Event::RecvRequestVote {
                stopped: *stopped, candidate_is_current_voter: *candidate_is_current_voter,
                lst_log_idx: *lst_log_idx, lst_log_term: *lst_log_term, can_id: *can_id,
                can_term: *can_term, failover: *failover, election_debug: *election_debug,
            },
            OwnedEvent::RecvAppendEntries {
                wire, stopped, sender_is_current_voter, has_cmd, leader_current_term,
                leader_site_id, leader_prev_log_index, leader_prev_log_term, leader_commit_index,
                failover,
            } => Event::RecvAppendEntries {
                wire, stopped: *stopped, sender_is_current_voter: *sender_is_current_voter,
                has_cmd: *has_cmd, leader_current_term: *leader_current_term,
                leader_site_id: *leader_site_id, leader_prev_log_index: *leader_prev_log_index,
                leader_prev_log_term: *leader_prev_log_term,
                leader_commit_index: *leader_commit_index, failover: *failover,
            },
            OwnedEvent::TickHeartbeat {
                is_leader, snapshot_configured, batching, max_batch_entries, max_batch_bytes,
            } => Event::TickHeartbeat {
                is_leader: *is_leader, snapshot_configured: *snapshot_configured,
                batching: *batching, max_batch_entries: *max_batch_entries,
                max_batch_bytes: *max_batch_bytes,
            },
            OwnedEvent::RecvAppendReply { ord, status, term, last_log_index, is_leader, stopped, failover } => {
                Event::RecvAppendReply {
                    ord: *ord, status: *status, term: *term, last_log_index: *last_log_index,
                    is_leader: *is_leader, stopped: *stopped, failover: *failover,
                }
            }
            OwnedEvent::AbandonRound => Event::AbandonRound,
            OwnedEvent::RoundEnd { is_leader } => Event::RoundEnd { is_leader: *is_leader },
            OwnedEvent::ResetRoundState => Event::ResetRoundState,
            OwnedEvent::Applied { index, published } => Event::Applied {
                index: *index, published: *published,
            },
            OwnedEvent::SetFollower { stopped, failover } => Event::SetFollower {
                stopped: *stopped, failover: *failover,
            },
            OwnedEvent::StepDown { stopped, failover } => Event::StepDown {
                stopped: *stopped, failover: *failover,
            },
            OwnedEvent::ObserveTerm { term, stopped, failover } => Event::ObserveTerm {
                term: *term, stopped: *stopped, failover: *failover,
            },
            OwnedEvent::Restore { term, vote, commit, entries_rev } => Event::Restore {
                term: *term, vote: *vote, commit: *commit,
                entries_rev: entries_rev.iter().map(|&(t, v, tpc, kind, bytes, d)| {
                    RaftEntry::new(t, ReplayCmd(d), v, tpc, kind, bytes)
                }).collect(),
            },
        }
    }
}

struct Tokens<'a> {
    it: std::str::SplitWhitespace<'a>,
}

impl<'a> Tokens<'a> {
    fn word(&mut self) -> Result<&'a str, String> {
        self.it.next().ok_or_else(|| "record ends early".to_string())
    }

    fn u(&mut self) -> Result<u64, String> {
        let w = self.word()?;
        w.parse::<u64>().map_err(|_| format!("not a u64: {w}"))
    }

    fn i(&mut self) -> Result<i64, String> {
        let w = self.word()?;
        w.parse::<i64>().map_err(|_| format!("not an i64: {w}"))
    }

    fn b(&mut self) -> Result<bool, String> {
        match self.word()? {
            "1" => Ok(true),
            "0" => Ok(false),
            w => Err(format!("not a bool: {w}")),
        }
    }
}

/// The event section of a record (`E <name> <fields>...`).
pub fn parse_event(text: &str) -> Result<(bool, OwnedEvent), String> {
    let mut t = Tokens { it: text.split_whitespace() };
    let checked = match t.word()? {
        "E" => false,
        "C" => true,
        _ => return Err("an event section starts with E or C".to_string()),
    };
    let name = t.word()?;
    let ev = match name {
        "identity" => OwnedEvent::SetIdentity {
            loc_id: t.u()? as u32, site_id: t.u()? as u16, partition_id: t.u()? as u32,
        },
        "configure" => {
            let n = t.u()?;
            let mut members = Vec::new();
            for _ in 0..n {
                members.push(t.u()? as u16);
            }
            OwnedEvent::Configure { members }
        }
        "gates" => OwnedEvent::EnterGates { snapshots_enabled: t.b()?, failover: t.b()? },
        "peers" => OwnedEvent::RebuildPeers { next_index: t.u()? },
        "propose" => OwnedEvent::Propose {
            cmd: ReplayCmd(t.u()?), has_value: t.b()?, is_tpc_commit: t.b()?,
            kind: t.i()? as i32, payload_bytes: t.u()?,
        },
        "campaign" => OwnedEvent::StartElection {
            timer_guarded: t.b()?, expected_generation: t.u()?, now: t.u()?, stopped: t.b()?,
        },
        "settle" => {
            let term = t.u()?;
            let loc_id = t.u()? as u32;
            let n_total = t.u()?;
            let timed_out = t.b()?;
            let stopped = t.b()?;
            let looping = t.b()?;
            let failover = t.b()?;
            let election_debug = t.b()?;
            let k = t.u()?;
            let (mut voters, mut granted, mut reply_terms) = (Vec::new(), Vec::new(), Vec::new());
            for _ in 0..k {
                voters.push(t.u()? as u16);
                granted.push(t.b()?);
                reply_terms.push(t.i()?);
            }
            OwnedEvent::SettleElection {
                term, loc_id, voters, granted, reply_terms, n_total, timed_out, stopped,
                looping, failover, election_debug,
            }
        }
        "timer" => OwnedEvent::ResetElectionTimer { now: t.u()?, timeout_us: t.u()? },
        "vote" => OwnedEvent::RecvRequestVote {
            stopped: t.b()?, candidate_is_current_voter: t.b()?, lst_log_idx: t.u()?,
            lst_log_term: t.i()?, can_id: t.u()? as u16, can_term: t.i()?, failover: t.b()?,
            election_debug: t.b()?,
        },
        "append" => {
            let stopped = t.b()?;
            let sender_is_current_voter = t.b()?;
            let has_cmd = t.b()?;
            let leader_current_term = t.u()?;
            let leader_site_id = t.u()? as u16;
            let leader_prev_log_index = t.u()?;
            let leader_prev_log_term = t.u()?;
            let leader_commit_index = t.u()?;
            let failover = t.b()?;
            let valid = t.b()?;
            let n = t.u()?;
            let mut terms = Vec::new();
            for _ in 0..n {
                terms.push(t.i()?);
            }
            let m = t.u()?;
            let mut entries = Vec::new();
            for _ in 0..m {
                entries.push((t.b()?, t.b()?, t.i()? as i32, t.u()?, t.u()?));
            }
            OwnedEvent::RecvAppendEntries {
                wire: ReplayBatch { valid, terms, entries }, stopped, sender_is_current_voter,
                has_cmd, leader_current_term, leader_site_id, leader_prev_log_index,
                leader_prev_log_term, leader_commit_index, failover,
            }
        }
        "tick" => OwnedEvent::TickHeartbeat {
            is_leader: t.b()?, snapshot_configured: t.b()?, batching: t.b()?,
            max_batch_entries: t.u()?, max_batch_bytes: t.u()?,
        },
        "reply" => OwnedEvent::RecvAppendReply {
            ord: t.u()? as usize, status: t.b()?, term: t.u()?, last_log_index: t.u()?,
            is_leader: t.b()?, stopped: t.b()?, failover: t.b()?,
        },
        "abandon" => OwnedEvent::AbandonRound,
        "round_end" => OwnedEvent::RoundEnd { is_leader: t.b()? },
        "reset_round" => OwnedEvent::ResetRoundState,
        "applied" => OwnedEvent::Applied { index: t.u()?, published: t.u()? },
        "follower" => OwnedEvent::SetFollower { stopped: t.b()?, failover: t.b()? },
        "step_down" => OwnedEvent::StepDown { stopped: t.b()?, failover: t.b()? },
        "observe_term" => OwnedEvent::ObserveTerm { term: t.u()?, stopped: t.b()?, failover: t.b()? },
        "restore" => {
            let term = t.u()?;
            let vote = t.u()? as u16;
            let commit = t.u()?;
            let n = t.u()?;
            let mut entries_rev = Vec::new();
            for _ in 0..n {
                entries_rev.push((t.i()?, t.b()?, t.b()?, t.i()? as i32, t.u()?, t.u()?));
            }
            OwnedEvent::Restore { term, vote, commit, entries_rev }
        }
        other => return Err(format!("unknown event {other}")),
    };
    if let Some(extra) = t.it.next() {
        return Err(format!("extra token after the {name} event: {extra}"));
    }
    Ok((checked, ev))
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

/// Where a replay first produced different text from the recording.
#[derive(Debug)]
pub struct Mismatch {
    /// 1-based line of the recording.
    pub line: usize,
    pub event: String,
    pub recorded: String,
    pub replayed: String,
}

/// What a replay of one recording did.
#[derive(Debug, PartialEq, Eq)]
pub struct Replayed {
    /// Records fed to the core and matched.
    pub steps: usize,
    /// The `T` line the replay stopped at, if any.
    pub tainted_at: Option<usize>,
}

/// The saved state the persist notes describe ([fix, F21]; disk plan P1):
/// folded from the replay core's own notes, and compared with the core.
/// A step that changed term, vote, commit or the log without a note that
/// covers the change makes the two differ: the notes are not exact.
struct Shadow {
    term: u64,
    vote: u16,
    commit: u64,
    // (term, digest) for each index from `base`
    base: u64,
    log: Vec<(i64, u64)>,
}

fn core_entry(core: &RaftCore<ReplayCmd>, index: u64) -> (i64, u64) {
    let e = core.raft_log_.get(index).expect("index within the core's log");
    (e.term(), e.cmd().0)
}

impl Shadow {
    fn of(core: &RaftCore<ReplayCmd>) -> Shadow {
        let base = core.raft_log_.base();
        let last = core.raft_log_.last_index();
        Shadow {
            term: core.current_term_,
            vote: core.vote_for_,
            commit: core.commit_index_,
            base,
            log: (base..=last).map(|i| core_entry(core, i)).collect(),
        }
    }

    fn apply(&mut self, note: &PersistNote, core: &RaftCore<ReplayCmd>) -> Result<(), String> {
        if note.hard_ {
            self.term = note.term_;
            self.vote = note.vote_;
            self.commit = note.commit_;
        }
        if note.log_from_ != 0 {
            let from = note.log_from_;
            if from < self.base || from > self.base + self.log.len() as u64 {
                return Err(format!("note writes from {from}, outside {}..={}", self.base,
                                   self.base + self.log.len() as u64));
            }
            self.log.truncate((from - self.base) as usize);
            for i in from..=core.raft_log_.last_index() {
                self.log.push(core_entry(core, i));
            }
        }
        Ok(())
    }

    /// The hard state and the log's tail every step; the whole log when
    /// `full`.
    fn check(&self, core: &RaftCore<ReplayCmd>, full: bool) -> Result<(), String> {
        let hard = (core.current_term_, core.vote_for_, core.commit_index_);
        if hard != (self.term, self.vote, self.commit) {
            return Err(format!("persist notes: hard state {:?}, the core has {hard:?}",
                               (self.term, self.vote, self.commit)));
        }
        let last = core.raft_log_.last_index();
        let mine = self.base + self.log.len() as u64 - 1;
        if core.raft_log_.base() != self.base || last != mine {
            return Err(format!("persist notes: log {}..={mine}, the core has {}..={last}",
                               self.base, core.raft_log_.base()));
        }
        let from = if full { self.base } else { last.saturating_sub(1).max(self.base) };
        for i in from..=last {
            if self.log[(i - self.base) as usize] != core_entry(core, i) {
                return Err(format!("persist notes: entry {i} differs"));
            }
        }
        Ok(())
    }
}

/// Replays one node's recording through a fresh core. Every record's
/// actions, log lines and reply must match the recorded text exactly; where
/// the recording has `P` lines, each step's persist note must match its
/// line. Whether or not it has them, the notes the replay core emits must
/// describe its saved state exactly (the [`Shadow`]).
pub fn replay(text: &str) -> Result<Replayed, Mismatch> {
    let mut core: RaftCore<ReplayCmd> = RaftCore::new();
    let mut shadow = Shadow::of(&core);
    let has_notes = text.lines().any(|l| l.starts_with("P "));
    let mut expect_note: Option<(usize, String)> = None;
    let mut steps = 0;
    for (n, line) in text.lines().enumerate() {
        let lineno = n + 1;
        if line.is_empty() {
            continue;
        }
        if let Some(note) = line.strip_prefix("P ") {
            match expect_note.take() {
                Some((_, produced)) if produced == line => continue,
                other => {
                    return Err(Mismatch {
                        line: lineno, event: "P".to_string(), recorded: note.to_string(),
                        replayed: other.map(|(_, p)| p).unwrap_or_else(|| "no note".to_string()),
                    })
                }
            }
        }
        if let Some((at, produced)) = expect_note.take() {
            return Err(Mismatch {
                line: at, event: "P".to_string(), recorded: "no P line".to_string(),
                replayed: produced,
            });
        }
        if line.starts_with("T ") || line == "T" {
            return Ok(Replayed { steps, tainted_at: Some(lineno) });
        }
        let parts: Vec<&str> = line.splitn(3, " | ").collect();
        let fail = |why: String| Mismatch {
            line: lineno, event: parts.first().copied().unwrap_or("").to_string(),
            recorded: line.to_string(), replayed: why,
        };
        if parts.len() != 3 {
            return Err(fail("a record has three sections".to_string()));
        }
        let (checked, ev) = parse_event(parts[0]).map_err(fail)?;
        let level: i32 = parts[1].trim().parse().map_err(|_| fail("bad log level".to_string()))?;
        let mut out = CoreOutput::new();
        out.set_log_level(level);
        let restoring = matches!(ev, OwnedEvent::Restore { .. });
        let produced = if checked {
            let reply = core.step_checked(ev.as_event(), &mut out);
            checked_result_text(&out, 0, 0, &reply, &replay_digest)
        } else {
            let reply = core.step(ev.as_event(), &mut out);
            result_text(&out, 0, 0, &reply, &replay_digest)
        };
        let recorded = parts[2];
        if produced != recorded {
            return Err(Mismatch {
                line: lineno, event: parts[0].to_string(), recorded: recorded.to_string(),
                replayed: produced,
            });
        }
        // The shadow: a Restore loads state the store already holds (no
        // note); every other step's changes must be its note's.
        let note = out.take_persist();
        let shadowed = if restoring {
            shadow = Shadow::of(&core);
            Ok(())
        } else {
            note.as_ref().map_or(Ok(()), |p| shadow.apply(p, &core))
                .and_then(|_| shadow.check(&core, steps % 4096 == 0))
        };
        if let Err(why) = shadowed {
            return Err(Mismatch { line: lineno, event: parts[0].to_string(),
                                  recorded: recorded.to_string(), replayed: why });
        }
        if has_notes {
            if let Some(p) = &note {
                expect_note = Some((lineno, persist_line(p)));
            }
        }
        steps += 1;
    }
    if let Some((at, produced)) = expect_note {
        return Err(Mismatch { line: at, event: "P".to_string(), recorded: "no P line".to_string(),
                              replayed: produced });
    }
    if let Err(why) = shadow.check(&core, true) {
        return Err(Mismatch { line: 0, event: "end".to_string(), recorded: String::new(), replayed: why });
    }
    Ok(Replayed { steps, tainted_at: None })
}

#[cfg(test)]
mod tests {
    use super::*;

    // A payload-free append, for the round trip.
    struct NoEntries;

    impl InboundBatch<ReplayCmd> for NoEntries {
        fn decode_terms(&self, _prev: u64, terms: &mut Vec<i64>) -> bool {
            terms.clear();
            true
        }

        fn entry_at(&self, _k: u64) -> RaftEntry<ReplayCmd> {
            unreachable!()
        }
    }

    // One step, recorded the way the shell records it.
    fn record<'a>(core: &mut RaftCore<ReplayCmd>, ev: Event<'a, ReplayCmd, NoEntries>,
                  text: &mut String) -> Reply<ReplayCmd> {
        let mut event = String::new();
        write_event(&mut event, &ev, &replay_digest);
        let mut out = CoreOutput::new();
        let reply = core.step(ev, &mut out);
        let result = result_text(&out, 0, 0, &reply, &replay_digest);
        text.push_str(&record_line(&event, out.log_level(), &result));
        text.push('\n');
        reply
    }

    // Setup, an election, a proposal, a heartbeat round with one reply, and
    // an inbound append from a newer leader: recorded, then replayed.
    #[test]
    fn a_recorded_history_replays() {
        let mut core: RaftCore<ReplayCmd> = RaftCore::new();
        let mut text = String::new();
        record(&mut core, Event::SetIdentity { loc_id: 0, site_id: 1, partition_id: 0 }, &mut text);
        record(&mut core, Event::Configure { members: &[1, 2, 3] }, &mut text);
        record(&mut core, Event::EnterGates { snapshots_enabled: false, failover: true }, &mut text);
        let c = record(&mut core, Event::StartElection {
            timer_guarded: false, expected_generation: 0, now: 5, stopped: false,
        }, &mut text).into_campaign();
        record(&mut core, Event::SettleElection {
            term: c.term_, loc_id: 0, voters: &[2, 3], granted: &[true, false],
            reply_terms: &[c.term_ as i64, c.term_ as i64], n_total: 3, timed_out: false,
            stopped: false, looping: true, failover: true, election_debug: true,
        }, &mut text);
        record(&mut core, Event::Propose {
            cmd: ReplayCmd(77), has_value: true, is_tpc_commit: false, kind: 3, payload_bytes: 9,
        }, &mut text);
        record(&mut core, Event::TickHeartbeat {
            is_leader: true, snapshot_configured: false, batching: false,
            max_batch_entries: 256, max_batch_bytes: 1 << 24,
        }, &mut text);
        record(&mut core, Event::RecvAppendReply {
            ord: 0, status: true, term: c.term_, last_log_index: 1, is_leader: true,
            stopped: false, failover: true,
        }, &mut text);
        record(&mut core, Event::RoundEnd { is_leader: true }, &mut text);
        record(&mut core, Event::Applied { index: 1, published: 0 }, &mut text);
        record(&mut core, Event::RecvAppendEntries {
            wire: &NoEntries, stopped: false, sender_is_current_voter: true, has_cmd: false,
            leader_current_term: c.term_ + 1, leader_site_id: 3, leader_prev_log_index: 1,
            leader_prev_log_term: c.term_, leader_commit_index: 1, failover: true,
        }, &mut text);
        assert!(text.lines().count() == 11);
        assert_eq!(replay(&text).unwrap(), Replayed { steps: 11, tainted_at: None });
        // A changed reply is caught, at its line.
        let tampered = text.replacen("R round_end 1", "R round_end 0", 1);
        assert_ne!(tampered, text, "the round end should have advanced the commit index");
        assert_eq!(replay(&tampered).unwrap_err().line, 9);
        // A write outside step stops the replay where it is marked.
        let tainted = format!("{}T test\n{}", text, text);
        assert_eq!(replay(&tainted).unwrap(), Replayed { steps: 11, tainted_at: Some(12) });
    }

    #[test]
    fn strings_survive_encoding() {
        for text in ["", "a b", "100%|x", "[RAFT] {} -> {} (x=y)", "\u{2192}"] {
            let mut s = String::new();
            encode(&mut s, text);
            assert!(!s.contains(' ') && !s.contains('|'));
            assert_eq!(decode(&s), text);
        }
    }
}
