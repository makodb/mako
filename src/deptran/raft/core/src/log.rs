// The replicated log: entries (a command handle and the facts cached about
// it, M6) in fixed blocks, never moved once written.
//
// [move, M1] From src/deptran/raft/shell/server_h.rs (Phase 6); the
// command is the type parameter C (M11) and logging is records in the
// output (M7).

#[allow(unused_imports)]
use crate::*;
use vstd::prelude::*;
#[allow(unused_imports)]
use vstd::arithmetic::div_mod::*;

verus! {

// The ceiling the ghost invariants keep every log index below: 2^62. A log
// reaching it would hold 2^62 entries (4.6e18; at a million appends a second,
// about 146,000 years), so no execution comes near it. Keeping indices below
// it is what lets the index arithmetic below be proved free of overflow; the
// shell's side of it is the host-contract assumption that no index it hands
// the core reaches it (the Phase 6 report, since removed).
pub open spec fn raft_index_limit() -> int {
    0x4000_0000_0000_0000int
}

// [fix, F22] The same ceiling as a value, for Restore's refusals.
pub const RAFT_INDEX_LIMIT: u64 = 0x4000_0000_0000_0000;

// [fix, F22] A sequence reversed (ghost): Restore's entries arrive last first.
pub open spec fn rev_seq<T>(s: Seq<T>) -> Seq<T> {
    Seq::new(s.len(), |i: int| s[s.len() - 1 - i])
}

// How many 4096-entry blocks `positions` physical positions fill:
// u64::div_ceil, which vstd does not specify, so its std meaning is stated
// here and trusted ([move, M11]: the same call, at the same point).
#[verifier::external_body]
fn blocks_for(positions: u64) -> (r: u64)
    ensures r as int == (positions as int + 4095) / 4096,
{
    positions.div_ceil(4096)  // [move, M11]
}

#[repr(C)]
pub struct RaftEntry<C> {
    term_: i64,
    cmd_: C,
    // [move, M6] What the per-entry kernels report about cmd_, asked once
    // when the entry is made (raft_entry_from_command) instead of at every
    // use. The command is never modified once logged: a send stamps a copy
    // (server.cc raft_stamped_commit_into). The last three are read only
    // when has_value_ holds, as the kernels were only called then.
    has_value_: bool,
    is_tpc_commit_: bool,
    kind_: i32,
    payload_bytes_: u64,
}

impl<C> RaftEntry<C> {
    // The entry's term and command (ghost).
    pub closed spec fn spec_term(&self) -> i64 {
        self.term_
    }

    pub closed spec fn spec_cmd(&self) -> C {
        self.cmd_
    }

    // [M12] Whether its command has a value (ghost).
    pub closed spec fn spec_has_value(&self) -> bool {
        self.has_value_
    }

    pub fn new(term: i64, cmd: C, has_value: bool,
               is_tpc_commit: bool, kind: i32, payload_bytes: u64) -> (r: RaftEntry<C>)
        ensures
            r.spec_term() == term,
            r.spec_cmd() == cmd,
            r.spec_has_value() == has_value,  // [M12]
    {
        RaftEntry {
            term_: term,
            cmd_: cmd,
            has_value_: has_value,  // [move, M6]
            is_tpc_commit_: is_tpc_commit,  // [move, M6]
            kind_: kind,  // [move, M6]
            payload_bytes_: payload_bytes,  // [move, M6]
        }
    }

    pub fn term(&self) -> (r: i64)
        ensures r == self.spec_term(),
    {
        self.term_
    }

    // Handed back to C++, never followed from Rust.
    pub fn cmd(&self) -> (r: &C)
        ensures *r == self.spec_cmd(),
    {
        &self.cmd_
    }

    // [move, M6] raft_command_has_value(cmd)
    pub fn has_value(&self) -> (r: bool)
        ensures r == self.spec_has_value(),  // [M12]
    {
        self.has_value_
    }

    // [move, M6] raft_command_is_tpc_commit(cmd)
    pub fn is_tpc_commit(&self) -> bool {
        self.is_tpc_commit_
    }

    // [move, M6] raft_command_kind(cmd)
    pub fn kind(&self) -> i32 {
        self.kind_
    }

    // [move, M6] raft_command_payload_bytes(cmd)
    pub fn payload_bytes(&self) -> u64 {
        self.payload_bytes_
    }
}

#[repr(C)]
pub struct RaftLog<C> {
    // Logical index of the first live entry.
    base_: u64,
    // How many entries at the front of blocks_[0] are dead (compacted away).
    head_: u64,
    // Live entry count.
    len_: u64,
    // Fixed-size blocks. Every block is exactly BLOCK long except the last.
    // Physical position of logical index i is head_ + (i - base_).
    blocks_: Vec<Vec<RaftEntry<C>>>,
}

impl<C> RaftLog<C> {
    // The block layout (ghost): live entries occupy physical positions
    // [head_, head_ + len_) of the blocks laid end to end; every block but
    // the last is full, the last holds at least one entry, and there are
    // blocks exactly when there are positions. Indices stay below
    // raft_index_limit().
    pub closed spec fn wf(&self) -> bool {
        let nb = self.blocks_@.len() as int;
        &&& 1 <= self.base_
        &&& self.base_ as int + (self.len_ as int) <= raft_index_limit()
        &&& self.head_ < 4096
        &&& (nb == 0 ==> self.head_ == 0 && self.len_ == 0)
        &&& (nb > 0 ==> {
            &&& (forall|b: int| 0 <= b < nb - 1 ==> (#[trigger] self.blocks_@[b])@.len() == 4096)
            &&& 1 <= self.blocks_@[nb - 1]@.len() <= 4096
            &&& self.head_ as int + self.len_ as int
                    == 4096 * (nb - 1) + self.blocks_@[nb - 1]@.len()
        })
    }

    pub closed spec fn spec_base(&self) -> int {
        self.base_ as int
    }

    pub closed spec fn spec_len(&self) -> int {
        self.len_ as int
    }

    pub open spec fn spec_last_index(&self) -> int {
        self.spec_base() + self.spec_len() - 1
    }

    pub open spec fn spec_holds(&self, index: int) -> bool {
        self.spec_base() <= index < self.spec_base() + self.spec_len()
    }

    // Room for one more entry below the ceiling (ghost).
    pub open spec fn spec_has_room(&self) -> bool {
        self.spec_base() + self.spec_len() < raft_index_limit()
    }

    // wf's facts a caller can use: the index bounds.
    pub proof fn lemma_wf_bounds(&self)
        requires self.wf(),
        ensures
            self.spec_base() >= 1,
            self.spec_base() + self.spec_len() <= raft_index_limit(),
            self.spec_len() >= 0,
            self.view().len() == self.spec_len(),
    {
    }

    // The entry at physical position p (ghost): block p / 4096, slot
    // p % 4096.
    pub closed spec fn at_phys(&self, p: int) -> RaftEntry<C> {
        self.blocks_@[p / 4096]@[p % 4096]
    }

    // The live entries in index order (ghost): entry k is logical index
    // base + k, at physical position head + k. What the proof reads the log
    // as; the blocks are how it is stored.
    pub closed spec fn view(&self) -> Seq<RaftEntry<C>> {
        Seq::new(self.len_ as nat, |k: int| self.at_phys(self.head_ as int + k))
    }
}

// A physical position's block and slot: p is 4096 * (p / 4096) + p % 4096,
// and q, r are those two exactly when p == 4096 * q + r with 0 <= r < 4096.
proof fn lemma_pos(p: int, q: int, r: int)
    requires
        p >= 0,
        0 <= r < 4096,
        p == 4096 * q + r,
    ensures
        p / 4096 == q,
        p % 4096 == r,
{
    lemma_fundamental_div_mod_converse(p, 4096, q, r);
}

#[allow(clippy::new_without_default)]
impl<C> RaftLog<C> {
    pub fn new() -> (r: RaftLog<C>)
        ensures
            r.wf(),
            r.spec_base() == 1,
            r.spec_len() == 0,
            r.view().len() == 0,
    {
        RaftLog { base_: 1, head_: 0, len_: 0, blocks_: Vec::new() }
    }

    // Entries per block. 4096 * sizeof(RaftEntry) = 128KB, so a block is a
    // handful of huge pages' worth and the outer vector stays tiny: a
    // 400k-entry log is 98 pointers.
    pub fn block_len() -> (r: u64)
        ensures r == 4096,
    {
        4096
    }

    pub fn base(&self) -> (r: u64)
        ensures r == self.spec_base(),
    {
        self.base_
    }

    pub fn len(&self) -> (r: usize)
        ensures r == self.spec_len(),
    {
        self.len_ as usize
    }

    pub fn is_empty(&self) -> (r: bool)
        ensures r == (self.spec_len() == 0),
    {
        self.len_ == 0
    }

    pub fn last_index(&self) -> (r: u64)
        requires self.wf(),
        ensures
            r == self.spec_last_index(),
            (r as int) < raft_index_limit(),
    {
        self.base_ + self.len_ - 1
    }

    pub fn holds(&self, index: u64) -> (r: bool)
        ensures r == self.spec_holds(index as int),
    {
        index >= self.base_ && index - self.base_ < self.len_
    }

    pub fn get(&self, index: u64) -> (r: Option<&RaftEntry<C>>)
        requires self.wf(),
        ensures
            r.is_some() == self.spec_holds(index as int),
            r matches Some(e) ==> *e == self.view()[index as int - self.spec_base()],
    {
        if !self.holds(index) {
            return None;
        }
        let phys = self.head_ + (index - self.base_);
        proof {
            let nb = self.blocks_@.len() as int;
            let last_len = self.blocks_@[nb - 1]@.len() as int;
            lemma_fundamental_div_mod(phys as int, 4096);
            let q = phys as int / 4096;
            let r = phys as int % 4096;
            assert(0 <= r < 4096);
            assert(phys as int == 4096 * q + r);
            assert((phys as int) < 4096 * (nb - 1) + last_len);
            assert(q < nb) by (nonlinear_arith)
                requires
                    phys as int == 4096 * q + r,
                    0 <= r,
                    (phys as int) < 4096 * (nb - 1) + last_len,
                    last_len <= 4096,
                    nb > 0,
            ;
            assert(q >= 0) by (nonlinear_arith)
                requires phys as int == 4096 * q + r, phys >= 0, r < 4096;
            if q == nb - 1 {
                assert(r < last_len) by (nonlinear_arith)
                    requires
                        phys as int == 4096 * q + r,
                        q == nb - 1,
                        (phys as int) < 4096 * (nb - 1) + last_len,
                ;
            }
        }
        let block = (phys / 4096) as usize;
        let slot = (phys % 4096) as usize;
        assert(self.view()[index as int - self.base_ as int] == self.at_phys(phys as int));
        Some(&self.blocks_[block][slot])
    }

    // Appends at last_index() + 1 and returns it. Never moves an existing
    // entry: a full block is left alone and a new one is pushed, so the
    // reallocation stall that a single growing vector pays under the Raft
    // mutex does not exist here.
    pub fn append(&mut self, entry: RaftEntry<C>) -> (r: u64)
        requires
            old(self).wf(),
            old(self).spec_has_room(),
        ensures
            final(self).wf(),
            final(self).spec_base() == old(self).spec_base(),
            final(self).spec_len() == old(self).spec_len() + 1,
            r == final(self).spec_last_index(),
            final(self).view() == old(self).view().push(entry),
    {
        let ghost pre = *self;
        // "is there room in the last block", said directly rather than as
        // (head_ + len_) % BLOCK == 0, which clippy reads as a hand-rolled
        // is_multiple_of and which emits as a method call on a uint64_t.
        let need_block = self.blocks_.is_empty()
            || self.blocks_[self.blocks_.len() - 1].len() == 4096;
        if need_block {
            let fresh: Vec<RaftEntry<C>> = Vec::with_capacity(4096);
            self.blocks_.push(fresh);
        }
        let last = self.blocks_.len() - 1;
        self.blocks_[last].push(entry);
        self.len_ += 1;
        proof {
            let nb = self.blocks_@.len() as int;
            assert forall|b: int| 0 <= b < nb - 1 implies (#[trigger] self.blocks_@[b])@.len() == 4096 by {
                if need_block {
                    if b < nb - 2 {
                        assert(self.blocks_@[b] == old(self).blocks_@[b]);
                    }
                } else {
                    assert(self.blocks_@[b] == old(self).blocks_@[b]);
                }
            }
            // the view: every old position keeps its entry, and the new one
            // sits right after them
            let nb0 = pre.blocks_@.len() as int;
            let h = self.head_ as int;
            let n0 = pre.len_ as int;
            assert forall|k: int| 0 <= k < n0
                implies #[trigger] self.view()[k] == pre.view()[k] by {
                let p = h + k;
                assert(self.view()[k] == self.at_phys(p));
                assert(pre.view()[k] == pre.at_phys(p));
                let q = p / 4096;
                let r = p % 4096;
                lemma_fundamental_div_mod(p, 4096);
                assert(0 <= r < 4096);
                assert(p == 4096 * q + r);
                // p is below the old layout's end: an old block, an old slot
                assert(nb0 > 0);
                let l0 = pre.blocks_@[nb0 - 1]@.len() as int;
                assert(p < 4096 * (nb0 - 1) + l0);
                assert(q < nb0) by (nonlinear_arith)
                    requires p == 4096 * q + r, 0 <= r, p < 4096 * (nb0 - 1) + l0, l0 <= 4096;
                assert(q >= 0) by (nonlinear_arith)
                    requires p == 4096 * q + r, p >= 0, r < 4096;
                if need_block || q < nb0 - 1 {
                    assert(self.blocks_@[q] == pre.blocks_@[q]);
                } else {
                    assert(q == nb0 - 1);
                    assert(r < l0) by (nonlinear_arith)
                        requires p == 4096 * q + r, q == nb0 - 1, p < 4096 * (nb0 - 1) + l0;
                    assert(self.blocks_@[q]@ == pre.blocks_@[q]@.push(entry));
                }
            }
            // the new entry's position
            let pn = h + n0;
            if need_block {
                assert(nb == nb0 + 1);
                if nb0 > 0 {
                    assert(pn == 4096 * nb0);
                }
                lemma_pos(pn, nb0, 0);
                assert(self.blocks_@[nb0]@ == Seq::<RaftEntry<C>>::empty().push(entry));
            } else {
                let l0 = pre.blocks_@[nb0 - 1]@.len() as int;
                lemma_pos(pn, nb0 - 1, l0);
                assert(self.blocks_@[nb0 - 1]@ == pre.blocks_@[nb0 - 1]@.push(entry));
            }
            assert(self.at_phys(pn) == entry);
            assert(self.view()[n0] == self.at_phys(pn));
            assert(self.view() =~= pre.view().push(entry));
        }
        self.base_ + self.len_ - 1
    }

    // [fix, F22] (whole item) Restore's append: the entries of `rev`, last
    // first, so each is moved out with pop and none is copied. The loop pops
    // inside its body because Verus takes no `while let` (the lint's form).
    #[allow(clippy::manual_while_let_some)]
    pub fn append_rev(&mut self, rev: Vec<RaftEntry<C>>)
        requires
            old(self).wf(),
            old(self).spec_base() + old(self).spec_len() + rev@.len() < raft_index_limit(),
        ensures
            final(self).wf(),
            final(self).spec_base() == old(self).spec_base(),
            final(self).spec_len() == old(self).spec_len() + rev@.len(),
            final(self).view() == old(self).view() + rev_seq(rev@),
    {
        let ghost orig = rev@;
        let ghost v0 = self.view();
        let mut rev = rev;
        let ghost total: int = self.spec_len() + rev@.len();
        while !rev.is_empty()
            invariant
                self.wf(),
                self.spec_base() == old(self).spec_base(),
                self.spec_len() + rev@.len() == total,
                old(self).spec_base() + total < raft_index_limit(),
                total == v0.len() + orig.len(),
                rev@.len() <= orig.len(),
                rev@ == orig.subrange(0, rev@.len() as int),
                self.view() == v0 + rev_seq(orig).subrange(0, orig.len() - rev@.len()),
            decreases rev@.len(),
        {
            let ghost before = self.view();
            let ghost m: int = orig.len() - rev@.len();
            let e = rev.pop().unwrap();
            proof {
                assert(e == orig[orig.len() - 1 - m]);
            }
            self.append(e);
            proof {
                assert(rev_seq(orig).subrange(0, m).push(rev_seq(orig)[m])
                    =~= rev_seq(orig).subrange(0, m + 1));
                assert(rev@ =~= orig.subrange(0, rev@.len() as int));
            }
        }
        proof {
            assert(rev_seq(orig).subrange(0, orig.len() as int) =~= rev_seq(orig));
        }
    }

    // Discard [index, end). A no-op past the tail, which is the ordinary
    // extend case.
    pub fn truncate_from(&mut self, index: u64)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            final(self).spec_base() == old(self).spec_base(),
            final(self).spec_len() == (if (index as int) <= old(self).spec_base() {
                0
            } else if index as int - old(self).spec_base() >= old(self).spec_len() {
                old(self).spec_len()
            } else {
                index as int - old(self).spec_base()
            }),
            final(self).view() == old(self).view().subrange(0, final(self).spec_len()),
    {
        let ghost pre = *self;
        if index <= self.base_ {
            self.blocks_.clear();
            self.head_ = 0;
            self.len_ = 0;
            assert(self.view() =~= pre.view().subrange(0, 0));
            return;
        }
        let keep = index - self.base_;
        if keep >= self.len_ {
            assert(self.view() =~= pre.view().subrange(0, pre.len_ as int));
            return;
        }
        let new_phys = self.head_ + keep;
        if new_phys == 0 {
            self.blocks_.clear();
        } else {
            let nblocks = blocks_for(new_phys) as usize;  // [move, M11]
            proof {
                let nb = old(self).blocks_@.len() as int;
                let np = new_phys as int;
                let k = nblocks as int;
                assert(k == (np + 4095) / 4096);
                lemma_fundamental_div_mod(np + 4095, 4096);
                assert(1 <= k) by (nonlinear_arith)
                    requires k == (np + 4095) / 4096, np >= 1;
                assert(4096 * (k - 1) < np <= 4096 * k) by (nonlinear_arith)
                    requires
                        np + 4095 == 4096 * k + (np + 4095) % 4096,
                        0 <= (np + 4095) % 4096 < 4096,
                ;
                // the old layout holds at least np positions, so k <= nb
                let old_last_len = old(self).blocks_@[nb - 1]@.len() as int;
                assert(np < 4096 * (nb - 1) + old_last_len);
                assert(k <= nb) by (nonlinear_arith)
                    requires
                        4096 * (k - 1) < np,
                        np < 4096 * (nb - 1) + old_last_len,
                        old_last_len <= 4096,
                ;
            }
            self.blocks_.truncate(nblocks);
            let tail = (new_phys - 4096 * ((nblocks as u64) - 1)) as usize;
            proof {
                let nb = old(self).blocks_@.len() as int;
                let k = nblocks as int;
                assert(self.blocks_@.len() == k);
                assert(1 <= tail <= 4096);
                if k < nb {
                    assert(self.blocks_@[k - 1] == old(self).blocks_@[k - 1]);
                    assert(self.blocks_@[k - 1]@.len() == 4096);
                } else {
                    let old_last_len = old(self).blocks_@[nb - 1]@.len() as int;
                    let cur_last_len = self.blocks_@[k - 1]@.len() as int;
                    assert(cur_last_len == old_last_len);
                    assert((tail as int) <= cur_last_len) by (nonlinear_arith)
                        requires
                            tail as int == new_phys as int - 4096 * (k - 1),
                            k == nb,
                            (new_phys as int) < 4096 * (nb - 1) + old_last_len,
                            cur_last_len == old_last_len,
                    ;
                }
            }
            self.blocks_[nblocks - 1].truncate(tail);
            proof {
                let k = nblocks as int;
                assert forall|b: int| 0 <= b < k - 1 implies (#[trigger] self.blocks_@[b])@.len() == 4096 by {
                    assert(self.blocks_@[b] == old(self).blocks_@[b]);
                }
            }
        }
        self.len_ = keep;
        proof {
            // the view: every kept position keeps its entry
            let h = self.head_ as int;
            let np = new_phys as int;
            assert forall|j: int| 0 <= j < keep as int
                implies #[trigger] self.view()[j] == pre.view()[j] by {
                let p = h + j;
                assert(self.view()[j] == self.at_phys(p));
                assert(pre.view()[j] == pre.at_phys(p));
                let q = p / 4096;
                let r = p % 4096;
                lemma_fundamental_div_mod(p, 4096);
                assert(p == 4096 * q + r);
                assert(q >= 0) by (nonlinear_arith)
                    requires p == 4096 * q + r, p >= 0, r < 4096;
                // np > 0 here (a zero new_phys means keep == 0)
                let nk = self.blocks_@.len() as int;
                lemma_fundamental_div_mod(np + 4095, 4096);
                let tl = self.blocks_@[nk - 1]@.len() as int;
                assert(np == 4096 * (nk - 1) + tl);
                assert(q < nk) by (nonlinear_arith)
                    requires p == 4096 * q + r, 0 <= r, p < np, np == 4096 * (nk - 1) + tl, tl <= 4096;
                if q < nk - 1 {
                    assert(self.blocks_@[q] == pre.blocks_@[q]);
                } else {
                    assert(q == nk - 1);
                    assert(r < tl) by (nonlinear_arith)
                        requires p == 4096 * q + r, q == nk - 1, p < np, np == 4096 * (nk - 1) + tl;
                    assert(self.blocks_@[q]@ == pre.blocks_@[q]@.subrange(0, tl));
                }
            }
            assert(self.view() =~= pre.view().subrange(0, keep as int));
        }
    }

    // Discard [base, index] -- snapshot compaction. Returns how many went.
    // Whole leading blocks are released; a partial block is retained and its
    // dead prefix is recorded in head_, so the index arithmetic stays exact
    // and no surviving entry is ever copied. Outside the verified
    // configuration (CompactLog does nothing under the gates, [fix, F5]);
    // proved here for its layout only.
    pub fn compact_through(&mut self, index: u64) -> (r: usize)
        requires
            old(self).wf(),
            index as int + 1 + old(self).spec_len() <= raft_index_limit(),
        ensures
            final(self).wf(),
    {
        if index < self.base_ {
            return 0;
        }
        let mut drop_count = index - self.base_ + 1;
        if drop_count > self.len_ {
            drop_count = self.len_;
        }
        self.head_ += drop_count;
        self.len_ -= drop_count;
        // index + 1, not base_ + drop_count. They agree whenever index is
        // inside the log, and when it is past the tail this is what the flat
        // vector did: the log empties and the index space restarts above the
        // compaction point rather than at the old tail.
        self.base_ = index + 1;
        while self.head_ >= 4096 && !self.blocks_.is_empty()
            invariant
                1 <= self.base_,
                self.base_ as int + (self.len_ as int) <= raft_index_limit(),
                self.blocks_@.len() == 0 ==> self.head_ as int + self.len_ as int == 0
                    || self.len_ == 0,
                self.blocks_@.len() > 0 ==> {
                    let nb = self.blocks_@.len() as int;
                    &&& (forall|b: int| 0 <= b < nb - 1 ==> (#[trigger] self.blocks_@[b])@.len() == 4096)
                    &&& 1 <= self.blocks_@[nb - 1]@.len() <= 4096
                    &&& self.head_ as int + self.len_ as int
                            == 4096 * (nb - 1) + self.blocks_@[nb - 1]@.len()
                },
            decreases self.blocks_@.len(),
        {
            proof {
                let nb = self.blocks_@.len() as int;
                if nb == 1 {
                    // the only block holds every position; head_ >= 4096
                    // means none of them is live
                    assert(self.len_ == 0);
                }
            }
            let ghost pre = self.blocks_@;
            self.blocks_.remove(0);
            self.head_ -= 4096;
            proof {
                let nb = self.blocks_@.len() as int;
                assert(self.blocks_@ == pre.remove(0));
                if nb > 0 {
                    assert forall|b: int| 0 <= b < nb - 1 implies (#[trigger] self.blocks_@[b])@.len() == 4096 by {
                        assert(self.blocks_@[b] == pre[b + 1]);
                    }
                    assert(self.blocks_@[nb - 1] == pre[nb]);
                }
            }
        }
        if self.len_ == 0 {
            self.blocks_.clear();
            self.head_ = 0;
        }
        drop_count as usize
    }

    // Drop everything and restart the index space at `base`. The follower
    // path after an InstallSnapshot that supersedes the whole local log.
    // Outside the verified configuration (snapshots are off under the gates).
    pub fn reset(&mut self, base: u64)
        requires
            1 <= base,
            (base as int) <= raft_index_limit(),
        ensures
            final(self).wf(),
            final(self).spec_base() == base,
            final(self).spec_len() == 0,
            final(self).view() == Seq::<RaftEntry<C>>::empty(),
    {
        self.blocks_.clear();
        self.head_ = 0;
        self.len_ = 0;
        self.base_ = base;
        assert(self.view() =~= Seq::<RaftEntry<C>>::empty());
    }
}

} // verus!
