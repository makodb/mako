// The protocol half of the in-flight AppendEntries slots.
//
// [move, M1] Moved verbatim from src/deptran/raft/shell/server_h.rs,
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

// ==========================================================================
// THE HEARTBEAT ROUND'S STATE ([move, M1], from server_cc.rs, where the
// heartbeat driver owned it as HeartbeatRoundState). RaftCore holds it now,
// beside the consensus state it is decided with: the AppendEntries in
// flight (PendingTable; one per follower until F23, a window of them since),
// the read-index authority evidence (AuthorityLedger) and the membership and
// term one round latches (HeartbeatRoundScope). Bodies unchanged.
// ==========================================================================

// [fix, F23] How many AppendEntries one follower may have in flight. With one
// (as before F23) a follower is sent once per round trip, so over a long link
// an entry waits for the batch ahead of it to come back. 32 batches at one per
// 5 ms heartbeat interval cover a round trip of about 160 ms.
pub const PIPELINE_WINDOW: usize = 32;

// [fix, F23] The table's size for `peers` followers: a window each (ghost).
pub open spec fn spec_slots_for(peers: int) -> int {
    if peers <= 1024 { peers * 32 } else { peers }
}

// [fix, F23] (whole item) The table's size for `peers` followers: a window
// each. 32 is PIPELINE_WINDOW, spelled out so the arithmetic stays linear; a
// peer count no configuration reaches gets one slot each.
pub fn slots_for(peers: usize) -> (r: usize)
    ensures r == spec_slots_for(peers as int),
{
    if peers <= 1024 { peers * 32 } else { peers }
}

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
    // [fix, F23] The RPC's prevLogIndex, which a reject backs off from; when
    // it left, on the shell's monotonic clock (TickHeartbeat's now_us); and
    // the payload bytes it carried. Pipelining paces by the second and caps
    // by the third.
    sent_prev_index_: u64,
    sent_at_us_: u64,  // [fix, F23]
    sent_bytes_: u64,  // [fix, F23]
}

impl PendingAppend {
    // One argument per field: the record is built whole, where the tick
    // decides the send.
    #[allow(clippy::too_many_arguments)]
    // [fix, F23] prev, the send time and the bytes are F23's.
    pub fn new(follower: u16, sent_term: u64, sent_round: u64,
               sent_end_index: u64, has_entries: bool, sent_prev_index: u64,
               sent_at_us: u64, sent_bytes: u64) -> PendingAppend {
        PendingAppend {
            follower_: follower,
            sent_term_: sent_term,
            sent_round_: sent_round,
            sent_end_index_: sent_end_index,
            has_entries_: has_entries,  // [move, M5]
            sent_prev_index_: sent_prev_index,  // [fix, F23]
            sent_at_us_: sent_at_us,  // [fix, F23]
            sent_bytes_: sent_bytes,  // [fix, F23]
        }
    }
}

// [fix, F23] (whole item) What one follower has in flight: how many
// AppendEntries, when the newest of them left, and their payload bytes
// (saturating).
pub struct InFlight {
    pub count_: usize,
    pub newest_at_us_: u64,
    pub bytes_: u64,
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

    // [fix, F23] (whole item) The in-flight RPC's prevLogIndex.
    pub fn sent_prev_index(&self, ordinal: usize) -> u64
        requires ordinal < self.spec_len(),
     {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_prev_index_
    }

    // [fix, F23] (whole item) What `follower` has in flight, over every slot.
    pub fn in_flight(&self, follower: u16) -> InFlight {
        let mut r = InFlight { count_: 0, newest_at_us_: 0, bytes_: 0 };
        let mut i: usize = 0;
        while i < self.slots_.len()
            invariant
                i <= self.slots_@.len(),
                r.count_ <= i,
            decreases self.slots_@.len() - i,
        {
            if self.slots_[i].is_some() {
                let p: &PendingAppend = self.slots_[i].as_ref().unwrap();
                if p.follower_ == follower {
                    r.count_ += 1;
                    if p.sent_at_us_ > r.newest_at_us_ {
                        r.newest_at_us_ = p.sent_at_us_;
                    }
                    r.bytes_ = r.bytes_.saturating_add(p.sent_bytes_);
                }
            }
            i += 1;
        }
        r
    }

    // [fix, F23] (whole item) The first empty slot, or the table's length when
    // every slot is taken (no follower is let past its window, so that is
    // never the case once the table holds a window per follower).
    pub fn free_slot(&self) -> (r: usize)
        ensures r <= self.spec_len(),
    {
        let mut i: usize = 0;
        while i < self.slots_.len()
            invariant i <= self.slots_@.len(),
            decreases self.slots_@.len() - i,
        {
            if self.slots_[i].is_none() {
                return i;
            }
            i += 1;
        }
        self.slots_.len()
    }
}

} // verus!
