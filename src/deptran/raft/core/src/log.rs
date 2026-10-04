// The replicated log: entries (a command handle and the facts cached about
// it, M6) in fixed blocks, never moved once written.
//
// [move, M1] From src/deptran/raft/src/server_h.rs (Phase 6); the
// command is the type parameter C (M11) and logging is records in the
// output (M7).

#[allow(unused_imports)]
use crate::*;

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
    pub fn new(term: i64, cmd: C, has_value: bool,
               is_tpc_commit: bool, kind: i32, payload_bytes: u64) -> RaftEntry<C> {
        RaftEntry {
            term_: term,
            cmd_: cmd,
            has_value_: has_value,  // [move, M6]
            is_tpc_commit_: is_tpc_commit,  // [move, M6]
            kind_: kind,  // [move, M6]
            payload_bytes_: payload_bytes,  // [move, M6]
        }
    }

    pub fn term(&self) -> i64 {
        self.term_
    }

    // Handed back to C++, never followed from Rust.
    pub fn cmd(&self) -> &C {
        &self.cmd_
    }

    // [move, M6] raft_command_has_value(cmd)
    pub fn has_value(&self) -> bool {
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

#[allow(clippy::new_without_default)]
impl<C> RaftLog<C> {
    pub fn new() -> RaftLog<C> {
        RaftLog { base_: 1, head_: 0, len_: 0, blocks_: Vec::new() }
    }

    // Entries per block. 4096 * sizeof(RaftEntry) = 128KB, so a block is a
    // handful of huge pages' worth and the outer vector stays tiny: a
    // 400k-entry log is 98 pointers.
    pub fn block_len() -> u64 {
        4096
    }

    pub fn base(&self) -> u64 {
        self.base_
    }

    pub fn len(&self) -> usize {
        self.len_ as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len_ == 0
    }

    pub fn last_index(&self) -> u64 {
        self.base_ + self.len_ - 1
    }

    pub fn holds(&self, index: u64) -> bool {
        index >= self.base_ && index - self.base_ < self.len_
    }

    pub fn get(&self, index: u64) -> Option<&RaftEntry<C>> {
        if !self.holds(index) {
            return None;
        }
        let phys = self.head_ + (index - self.base_);
        let block = (phys / 4096) as usize;
        let slot = (phys % 4096) as usize;
        Some(&self.blocks_[block][slot])
    }

    // Appends at last_index() + 1 and returns it. Never moves an existing
    // entry: a full block is left alone and a new one is pushed, so the
    // reallocation stall that a single growing vector pays under the Raft
    // mutex does not exist here.
    pub fn append(&mut self, entry: RaftEntry<C>) -> u64 {
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
        self.base_ + self.len_ - 1
    }

    // Discard [index, end). A no-op past the tail, which is the ordinary
    // extend case.
    pub fn truncate_from(&mut self, index: u64) {
        if index <= self.base_ {
            self.blocks_.clear();
            self.head_ = 0;
            self.len_ = 0;
            return;
        }
        let keep = index - self.base_;
        if keep >= self.len_ {
            return;
        }
        let new_phys = self.head_ + keep;
        if new_phys == 0 {
            self.blocks_.clear();
        } else {
            let nblocks = new_phys.div_ceil(4096) as usize;
            self.blocks_.truncate(nblocks);
            let tail = (new_phys - 4096 * ((nblocks as u64) - 1)) as usize;
            self.blocks_[nblocks - 1].truncate(tail);
        }
        self.len_ = keep;
    }

    // Discard [base, index] -- snapshot compaction. Returns how many went.
    // Whole leading blocks are released; a partial block is retained and its
    // dead prefix is recorded in head_, so the index arithmetic stays exact
    // and no surviving entry is ever copied.
    pub fn compact_through(&mut self, index: u64) -> usize {
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
        while self.head_ >= 4096 && !self.blocks_.is_empty() {
            self.blocks_.remove(0);
            self.head_ -= 4096;
        }
        if self.len_ == 0 {
            self.blocks_.clear();
            self.head_ = 0;
        }
        drop_count as usize
    }

    // Drop everything and restart the index space at `base`. The follower
    // path after an InstallSnapshot that supersedes the whole local log.
    pub fn reset(&mut self, base: u64) {
        self.blocks_.clear();
        self.head_ = 0;
        self.len_ = 0;
        self.base_ = base;
    }
}
