// The protocol half of the in-flight AppendEntries slots.
//
// [move, M1] Moved verbatim from src/deptran/raft/src/server_h.rs (Phase 6),
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

// ==========================================================================
// THE HEARTBEAT ROUND'S STATE ([move, M1], from server_cc.rs, where the
// heartbeat driver owned it as HeartbeatRoundState). RaftCore holds it now,
// beside the consensus state it is decided with: at most one AppendEntries
// in flight per follower (PendingTable), the read-index authority evidence
// (AuthorityLedger) and the membership and term one round latches
// (HeartbeatRoundScope). Bodies unchanged.
// ==========================================================================

pub struct PendingAppend {
    follower_: u16,
    sent_term_: u64,
    sent_round_: u64,
    // Inclusive end of the exact prefix proved by this RPC's wire payload. A
    // heartbeat proves only prevLogIndex; raw and batched payloads extend it
    // by their encoded entry count.
    sent_end_index_: u64,
    // [move, M5] Whether the RPC carried entries, which is all the reply's
    // log line asked of its command. The command and the response handle
    // stay with the shell (RaftServerBase::append_responses_), which sends
    // the RPC after the core has decided it.
    has_entries_: bool,
}

impl PendingAppend {
    pub fn new(follower: u16, sent_term: u64, sent_round: u64,
               sent_end_index: u64, has_entries: bool) -> PendingAppend {
        PendingAppend {
            follower_: follower,
            sent_term_: sent_term,
            sent_round_: sent_round,
            sent_end_index_: sent_end_index,
            has_entries_: has_entries,  // [move, M5]
        }
    }
}

pub struct PendingTable {
    slots_: Vec<Option<PendingAppend>>,
}

impl PendingTable {
    // How many follower slots the table holds (ghost).
    pub closed spec fn spec_len(&self) -> int {
        self.slots_@.len() as int
    }

    // [M12] The follower an in-flight slot was sent to (0 when empty)
    // (ghost).
    pub closed spec fn spec_follower(&self, ordinal: int) -> u16 {
        match self.slots_@[ordinal] {
            Some(p) => p.follower_,
            None => 0,
        }
    }
}

#[allow(clippy::new_without_default)]
impl PendingTable {
    pub fn new() -> (r: PendingTable)
        ensures r.spec_len() == 0,
    {
        PendingTable { slots_: Vec::new() }
    }

    // One slot per follower, all empty. Called wherever the peer table is
    // sized, so the two always agree on what an ordinal means.
    pub fn resize(&mut self, peers: usize)
        ensures final(self).spec_len() == peers,
    {
        self.slots_.clear();
        let mut i: usize = 0;
        while i < peers
            invariant
                i <= peers,
                self.slots_@.len() == i,
            decreases peers - i,
        {
            self.slots_.push(None);
            i += 1;
        }
    }

    // Drops every in-flight context. Used on leadership loss and on a term
    // change, so a prior epoch's RPC can never occupy a slot.
    pub fn abandon(&mut self)
        ensures final(self).spec_len() == old(self).spec_len(),
    {
        let peers = self.slots_.len();
        self.resize(peers);
    }

    pub fn len(&self) -> (r: usize)
        ensures r == self.spec_len(),
    {
        self.slots_.len()
    }

    pub fn is_empty(&self) -> (r: bool)
        ensures r == (self.spec_len() == 0),
    {
        self.slots_.is_empty()
    }

    pub fn occupied(&self, ordinal: usize) -> bool
        requires ordinal < self.spec_len(),
     {
        self.slots_[ordinal].is_some()
    }

    pub fn place(&mut self, ordinal: usize, pending: PendingAppend)
        requires ordinal < old(self).spec_len(),
        ensures final(self).spec_len() == old(self).spec_len(),
     {
        self.slots_[ordinal] = Some(pending);
    }

    pub fn release(&mut self, ordinal: usize)
        requires ordinal < old(self).spec_len(),
        ensures final(self).spec_len() == old(self).spec_len(),
     {
        self.slots_[ordinal] = None;
    }

    pub fn follower(&self, ordinal: usize) -> (r: u16)
        requires ordinal < self.spec_len(),
        ensures r == self.spec_follower(ordinal as int),  // [M12]
     {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().follower_
    }

    pub fn sent_term(&self, ordinal: usize) -> u64
        requires ordinal < self.spec_len(),
     {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_term_
    }

    pub fn sent_round(&self, ordinal: usize) -> u64
        requires ordinal < self.spec_len(),
     {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_round_
    }

    pub fn sent_end_index(&self, ordinal: usize) -> u64
        requires ordinal < self.spec_len(),
     {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_end_index_
    }

    // [move, M5] Whether the in-flight RPC carried entries.
    pub fn has_entries(&self, ordinal: usize) -> bool
        requires ordinal < self.spec_len(),
     {
        if self.slots_[ordinal].is_none() {
            return false;
        }
        self.slots_[ordinal].as_ref().unwrap().has_entries_
    }
}

} // verus!
