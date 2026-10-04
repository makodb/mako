// The campaign's vote set and outcome, and the election timer's gather.
//
// [move, M1] Moved verbatim from src/deptran/raft/src/server_h.rs (Phase 6),
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

// [move, M5] One campaign's outcome as the core counted it: the fields
// raft_vote_quorum_snapshot used to read out of the lane's quorum object,
// field for field.
pub struct VoteOutcome {
    pub term_: i64,
    pub yes_: bool,
    pub no_: bool,
    pub n_voted_yes_: i32,
    pub n_voted_no_: i32,
    pub timeouted_: bool,
}

// [move, M5] One campaign's votes, counted by the core with the rule both
// lanes' tallies use (RaftVoteQuorumEvent, raft-rt's TallyState): yes once
// the peer yes votes reach n/2, no once the peer no votes exceed n - n/2
// (the off-by-one bugs-found B1 records, kept for lane parity). Each voter
// counts once ([fix, F1], now on every lane), and a reply term that is
// non-negative and higher than any seen is kept, as FeedResponse did.
pub struct VoteSet {
    voters_: SiteSet,
    yes_: u64,
    no_: u64,
    highest_term_: i64,
}

impl VoteSet {
    // Each counted vote is one voter in the set, so neither count can
    // overflow (ghost).
    pub closed spec fn wf(&self) -> bool {
        self.yes_ as int + self.no_ as int == self.voters_.spec_len()
    }

    // The highest reply term seen so far (ghost).
    pub closed spec fn spec_highest_term(&self) -> i64 {
        self.highest_term_
    }
}

#[allow(clippy::new_without_default)]
impl VoteSet {
    pub fn new() -> (r: VoteSet)
        ensures
            r.wf(),
            r.spec_highest_term() == 0,
    {
        VoteSet { voters_: SiteSet::new(), yes_: 0, no_: 0, highest_term_: 0 }
    }

    pub fn feed(&mut self, voter: u16, granted: bool, term: i64)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            final(self).spec_highest_term() == old(self).spec_highest_term()
                || final(self).spec_highest_term() == term,
    {
        if !self.voters_.insert(voter) {
            return;
        }
        if term >= 0 && term > self.highest_term_ {
            self.highest_term_ = term;
        }
        if granted {
            self.yes_ += 1;
        } else {
            self.no_ += 1;
        }
    }

    // `n_total` is the configured partition size, self included, as the
    // lane counted it.
    pub fn outcome(&self, n_total: u64, timed_out: bool) -> (r: VoteOutcome)
        ensures r.term_ == self.spec_highest_term(),
    {
        let quorum: u64 = n_total / 2;
        VoteOutcome {
            term_: self.highest_term_,
            yes_: self.yes_ >= quorum,
            no_: self.no_ > n_total - quorum,
            n_voted_yes_: self.yes_ as i32,
            n_voted_no_: self.no_ as i32,
            timeouted_: timed_out,
        }
    }
}

// [move, M5] What start_election decided: whether the campaign starts, and
// the values the broadcast and its log line need.
pub struct CampaignStart {
    pub started_: bool,
    pub term_: u64,
    pub prev_term_: u64,
    pub prev_vote_for_: u16,
    pub lst_idx_: u64,
    pub lst_term_: i64,
}

impl CampaignStart {
    pub fn not_started() -> CampaignStart {
        CampaignStart {
            started_: false,
            term_: 0,
            prev_term_: 0,
            prev_vote_for_: RAFT_SERVER_INVALID_SITE_ID,
            lst_idx_: 0,
            lst_term_: 0,
        }
    }
}

// One locked gather's worth of election state. Plain copies, so the loop can
// branch on them after the lock is released, exactly as the C++ did.
// repr(C) is mandatory, not decorative: raft_election_gather returns this
// across an extern "C" boundary, so Rust's layout must be the C++ struct's.
#[repr(C)]
pub struct ElectionTick {
    time_elapsed_: u64,
    election_timeout_: u64,
    heartbeat_time_: u64,
    generation_: u64,
    term_: u64,
    vote_for_: u16,
    fired_: bool,
}

#[allow(clippy::too_many_arguments)]
impl ElectionTick {
    pub fn new(time_elapsed: u64, election_timeout: u64, heartbeat_time: u64,
               generation: u64, term: u64, vote_for: u16, fired: bool) -> ElectionTick {
        ElectionTick {
            time_elapsed_: time_elapsed,
            election_timeout_: election_timeout,
            heartbeat_time_: heartbeat_time,
            generation_: generation,
            term_: term,
            vote_for_: vote_for,
            fired_: fired,
        }
    }

    pub fn fired(&self) -> bool { self.fired_ }
    pub fn generation(&self) -> u64 { self.generation_ }
    pub fn time_elapsed(&self) -> u64 { self.time_elapsed_ }
    pub fn election_timeout(&self) -> u64 { self.election_timeout_ }
    pub fn heartbeat_time(&self) -> u64 { self.heartbeat_time_ }
    pub fn term(&self) -> u64 { self.term_ }
    pub fn vote_for(&self) -> u16 { self.vote_for_ }
}

} // verus!
