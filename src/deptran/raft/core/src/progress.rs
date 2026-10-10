// Per-follower replication progress: next and match index, backoff.
//
// [move, M1] Moved verbatim from src/deptran/raft/shell/server_h.rs (Phase 6),
// paths aside: the rust lane's rusty::Vec/Option are std's own.

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;

verus! {

// SCREAMING_CASE variants match the surrounding C++ enum convention and the
// existing DSL enums in snapshot_format.hpp, which carries this same allow.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[derive(Structural)]  // [M12] ghost: `==` is equality to the verifier
#[repr(i32)]
pub enum BackoffKind {
    FAST = 0,
    TERM_CONFLICT = 1,
    EXPONENTIAL = 2,
    LINEAR = 3,
    FLOOR = 4,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct FollowerProgress {
    pub next_: u64,
    pub match_: u64,
}

#[allow(clippy::new_without_default)]
impl FollowerProgress {
    pub fn new(next: u64, matched: u64) -> (r: FollowerProgress)
        ensures r.next_ == next && r.match_ == matched,  // [M12]
    {
        FollowerProgress { next_: next, match_: matched }
    }

    pub fn next_index(&self) -> (r: u64)
        ensures r == self.next_,
    {
        self.next_
    }

    pub fn match_index(&self) -> (r: u64)
        ensures r == self.match_,
    {
        self.match_
    }

    pub fn set_next_index(&mut self, value: u64)
        ensures
            final(self).next_ == value,
            final(self).match_ == old(self).match_,  // [M12]
    {
        self.next_ = value;
    }

    // The five-way backoff ladder taken when a follower rejects AppendEntries.
    // Returns which rung was used so the caller can log it; the arithmetic
    // itself is identical to the inline version it replaces.
    // [move, M10] `follower_last_log_index` comes off the wire, so its + 1
    // can wrap; release builds always wrapped it (the next round's
    // "Repairing wrapped next_index" catches a resulting 0). wrapping_add
    // says so, and is the same machine operation.
    pub fn back_off_after_reject(&mut self, follower_last_log_index: u64) -> (r: BackoffKind)
        ensures
            r == BackoffKind::FAST ==> final(self).next_ < old(self).next_,
            final(self).match_ == old(self).match_,  // [M12]
    {
        if follower_last_log_index > 0
            && follower_last_log_index.wrapping_add(1) < self.next_  // [move, M10]
        {
            self.next_ = follower_last_log_index.wrapping_add(1);  // [move, M10]
            return BackoffKind::FAST;
        }
        if follower_last_log_index > 0
            && follower_last_log_index.wrapping_add(1) == self.next_  // [move, M10]
            && self.next_ > 1
        {
            self.next_ -= 1;
            return BackoffKind::TERM_CONFLICT;
        }
        if self.next_ > 10 {
            self.next_ /= 2;
            return BackoffKind::EXPONENTIAL;
        }
        if self.next_ > 1 {
            self.next_ -= 1;
            return BackoffKind::LINEAR;
        }
        self.next_ = 1;
        BackoffKind::FLOOR
    }

    // A successful AppendEntries proves the exact payload end and no more.
    // Both indices are monotonic: a late reply can never move them backwards.
    pub fn accept_through(&mut self, acknowledged_through: u64, has_successor: bool,
                          follower_next: u64)
        ensures  // [M12] the match index rises to the acknowledged end, never falls
            final(self).match_ == (if acknowledged_through > old(self).match_ {
                acknowledged_through } else { old(self).match_ }),
    {
        if acknowledged_through > self.match_ {
            self.match_ = acknowledged_through;
        }
        if has_successor && follower_next > self.next_ {
            self.next_ = follower_next;
        }
    }
}

// The whole peer-progress cluster, owned by one type instead of scattered
// across a std::map keyed by site id.
//
// WHY A DENSE VECTOR. The replica set is fixed for the process lifetime:
// current_config_ has exactly one write, at server.cc:1671 inside Setup, and
// progress_'s key set was established once from it. Every follower therefore
// has a stable ordinal, and the map was paying a comparison and a cursor for
// what is an array index. The original plan proposed this shape and then
// abandoned it, recording that "the dense rewrite needs a stable
// site-to-ordinal mapping that does not exist" -- which that single-write
// measurement shows is not so.
//
// It also removes the map cursor as a category. Every access is by ordinal,
// computed fresh at each use, so there is no iterator to hold across an RPC
// send or a synchronous completion callback -- the hazard commit 4427129a9
// fixed by hand for next_index_, now unspellable.
//
// Vec specifically, not rusty::BTreeMap: Vec's rustc model is a
// re-export of std::vec::Vec and its C++ side is the real vec_port, so both
// sides are faithful. BTreeMap's rustc model is not -- its insert is a plain
// push with no key replacement and its get returns the first match
// (src/srpc/rusty-rustc/src/lib.rs:907) -- so a DSL type owning one would be
// verified against semantics production does not have.
#[repr(C)]
pub struct PeerTable {
    progress_: Vec<FollowerProgress>,
}

impl PeerTable {
    // How many followers the table holds, and a follower's next index
    // (ghost).
    pub closed spec fn spec_len(&self) -> int {
        self.progress_@.len() as int
    }

    pub closed spec fn spec_next(&self, ordinal: int) -> u64 {
        self.progress_@[ordinal].next_
    }

    // [M12] A follower's match index, and all of them in ordinal order
    // (ghost).
    pub closed spec fn spec_match(&self, ordinal: int) -> u64 {
        self.progress_@[ordinal].match_
    }

    pub closed spec fn spec_matches(&self) -> Seq<u64> {
        Seq::new(self.progress_@.len(), |o: int| self.progress_@[o].match_)
    }

    pub proof fn lemma_matches(&self)
        ensures
            self.spec_matches().len() == self.spec_len(),
            forall|o: int| 0 <= o < self.spec_len() ==> #[trigger] self.spec_matches()[o] == self.spec_match(o),
    {
    }
}

// [M12] How many of the first k match indices are at least v / below v (ghost;
// the leader's commit rule counts with these, src/deptran/raft/verus/commit_rule.rs).
pub open spec fn count_ge(s: Seq<u64>, v: u64, k: int) -> nat
    decreases k,
{
    if k <= 0 { 0 } else { count_ge(s, v, k - 1) + if s[k - 1] >= v { 1nat } else { 0nat } }
}

pub open spec fn count_lt(s: Seq<u64>, v: u64, k: int) -> nat
    decreases k,
{
    if k <= 0 { 0 } else { count_lt(s, v, k - 1) + if s[k - 1] < v { 1nat } else { 0nat } }
}

pub proof fn lemma_ge_lt_partition(s: Seq<u64>, v: u64, k: int)
    requires 0 <= k,
    ensures count_ge(s, v, k) + count_lt(s, v, k) == k,
    decreases k,
{
    if k > 0 { lemma_ge_lt_partition(s, v, k - 1); }
}

pub proof fn lemma_all_ge_zero(s: Seq<u64>, k: int)
    requires 0 <= k,
    ensures count_ge(s, 0, k) == k,
    decreases k,
{
    if k > 0 { lemma_all_ge_zero(s, k - 1); }
}

// Lowering the threshold keeps every follower counted.
pub proof fn lemma_count_ge_antitone(s: Seq<u64>, lo: u64, hi: u64, k: int)
    requires 0 <= k, lo <= hi,
    ensures count_ge(s, lo, k) >= count_ge(s, hi, k),
    decreases k,
{
    if k > 0 { lemma_count_ge_antitone(s, lo, hi, k - 1); }
}

#[allow(clippy::new_without_default)]
impl PeerTable {
    pub fn new() -> (r: PeerTable)
        ensures r.spec_len() == 0,
    {
        PeerTable { progress_: Vec::new() }
    }

    // One slot per follower, in ordinal order.
    pub fn reset(&mut self, peers: usize, next_index: u64)
        ensures
            final(self).spec_len() == peers,
            forall|o: int| 0 <= o < peers ==> final(self).spec_match(o) == 0,  // [M12]
    {
        self.progress_.clear();
        let mut i: usize = 0;
        while i < peers
            invariant
                i <= peers,
                self.progress_@.len() == i,
                forall|o: int| 0 <= o < i ==> self.progress_@[o].match_ == 0,  // [M12]
            decreases peers - i,
        {
            self.progress_.push(FollowerProgress::new(next_index, 0));
            i += 1;
        }
    }

    pub fn len(&self) -> (r: usize)
        ensures r == self.spec_len(),
    {
        self.progress_.len()
    }

    // Required by clippy alongside len(). A leader always has followers in
    // this table unless the partition is single-replica, which is exactly the
    // case the commit-index selector special-cases.
    pub fn is_empty(&self) -> (r: bool)
        ensures r == (self.spec_len() == 0),
    {
        self.progress_.is_empty()
    }

    pub fn next_index(&self, ordinal: usize) -> (r: u64)
        requires ordinal < self.spec_len(),
        ensures r == self.spec_next(ordinal as int),
    {
        self.progress_[ordinal].next_index()
    }

    pub fn set_next_index(&mut self, ordinal: usize, value: u64)
        requires ordinal < old(self).spec_len(),
        ensures
            final(self).spec_len() == old(self).spec_len(),
            final(self).spec_next(ordinal as int) == value,
            forall|o: int| 0 <= o < old(self).spec_len()
                ==> final(self).spec_match(o) == old(self).spec_match(o),  // [M12]
    {
        self.progress_[ordinal].set_next_index(value);
    }

    pub fn match_index(&self, ordinal: usize) -> (r: u64)
        requires ordinal < self.spec_len(),
        ensures r == self.spec_match(ordinal as int),  // [M12]
    {
        self.progress_[ordinal].match_index()
    }

    // The committable index this table's evidence supports.
    //
    // Both heartbeat phases used to build a std::vector of match indices,
    // std::sort it, and index (nservers - 1) / 2. The table owns those values,
    // so it can answer directly -- and it does so by RANK SELECTION rather
    // than sorting, because a DSL body has no working spelling for .sort():
    // the emitter lowers every receiver shape to rusty::sort, which is defined
    // only in the non-exported global module fragment of the transpiled ports
    // and is declared by no header. Selection is O(n^2) where sorting is
    // O(n log n), which is free at the replica counts this system runs (3 or
    // 5) and is on the per-round path, not the per-entry path.
    //
    // Ties are broken by ordinal so the result matches a stable sort exactly.
    pub fn majority_match_index(&self, nservers: usize, last_log_index: u64) -> (r: u64)
        requires nservers >= 1,
        ensures
            r <= last_log_index,
            // [M12] with this server, a majority holds r (the counting is
            // commit_rule.rs's)
            nservers > 1 && self.spec_len() == nservers - 1 ==> count_ge(self.spec_matches(), r,
                self.spec_len()) >= self.spec_len() - (nservers - 1) / 2,
    {
        let target = (nservers - 1) / 2;
        let n = self.progress_.len();
        let mut selected: u64 = 0;
        let ghost ms = self.spec_matches();
        proof {
            self.lemma_matches();
            lemma_all_ge_zero(ms, n as int);
        }
        let mut i: usize = 0;
        while i < n
            invariant
                i <= n,
                n == self.progress_@.len(),
                // [M12]
                ms == self.spec_matches(),
                ms.len() == n,
                target == (nservers - 1) / 2,
                count_ge(ms, selected, n as int) >= n - target,
            decreases n - i,
        {
            let value = self.progress_[i].match_index();
            let mut rank: usize = 0;
            let ghost mut lt: nat = 0;  // [M12]
            let mut j: usize = 0;
            while j < n
                invariant
                    i < n,
                    j <= n,
                    rank <= j,
                    n == self.progress_@.len(),
                    // [M12] rank counts the matches below value, and ties
                    ms == self.spec_matches(),
                    value == ms[i as int],
                    lt == count_lt(ms, value, j as int),
                    lt <= rank,
                decreases n - j,
            {
                let other = self.progress_[j].match_index();
                if other < value || (other == value && j < i) {
                    rank += 1;
                }
                proof {
                    assert(other == ms[j as int]);
                    lt = lt + if other < value { 1nat } else { 0nat };
                }
                j += 1;
            }
            if rank == target {
                proof { lemma_ge_lt_partition(ms, value, n as int); }
                selected = value;
            }
            i += 1;
        }
        proof {
            if nservers > 1 {
                let r = raft_server_commit_index_candidate(selected, nservers, last_log_index);
                lemma_count_ge_antitone(ms, r, selected, n as int);
            }
        }
        raft_server_commit_index_candidate(selected, nservers, last_log_index)
    }

    pub fn back_off_after_reject(&mut self, ordinal: usize,
                                 follower_last_log_index: u64) -> (r: BackoffKind)
        requires ordinal < old(self).spec_len(),
        ensures
            final(self).spec_len() == old(self).spec_len(),
            r == BackoffKind::FAST
                ==> final(self).spec_next(ordinal as int) < old(self).spec_next(ordinal as int),
            forall|o: int| 0 <= o < old(self).spec_len()
                ==> final(self).spec_match(o) == old(self).spec_match(o),  // [M12]
    {
        self.progress_[ordinal].back_off_after_reject(follower_last_log_index)
    }

    pub fn accept_through(&mut self, ordinal: usize, acknowledged_through: u64,
                          has_successor: bool, follower_next: u64)
        requires ordinal < old(self).spec_len(),
        ensures
            final(self).spec_len() == old(self).spec_len(),
            // [M12] the follower's match rises to the acknowledged end; the
            // others stay
            final(self).spec_match(ordinal as int) == (if acknowledged_through > old(self).spec_match(ordinal as int) {
                acknowledged_through } else { old(self).spec_match(ordinal as int) }),
            forall|o: int| 0 <= o < old(self).spec_len() && o != ordinal
                ==> final(self).spec_match(o) == old(self).spec_match(o),
    {
        self.progress_[ordinal].accept_through(acknowledged_through,
                                               has_successor, follower_next);
    }
}

} // verus!
