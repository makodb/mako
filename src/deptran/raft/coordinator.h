#pragma once

#include "../__dep__.h"
#include "../coordinator.h"
#include "../frame.h"
#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/slice.hpp>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Config::GetConfig: [safe, () -> Config*],
//   Reactor::create_sp_event: [safe, () -> shared_ptr<IntEvent>],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>]
// }

namespace janus {

class RaftCommo;
class RaftServer;

// @safe - pure CoordinatorRaft phase/quorum helpers over copied scalar
// values. The CoordinatorRaft class keeps inherited phase_ state, raw
// server/commo back-pointers, and command orchestration in C++.
#if RUSTYCPP_RUST
pub fn coordinator_raft_phase_value(phase: i32, n_phase: i32) -> i32 {
    phase % n_phase
}

pub fn coordinator_raft_phase_is_prepare(phase: i32) -> bool {
    phase == 1
}

pub fn coordinator_raft_phase_is_accept(phase: i32) -> bool {
    phase == 2
}

pub fn coordinator_raft_phase_is_commit(phase: i32) -> bool {
    phase == 3
}

pub fn coordinator_raft_majority_count(n_replica: u32) -> u32 {
    (n_replica / 2) + 1
}

pub fn coordinator_raft_should_handle_wrong_leader(is_leader: bool) -> bool {
    !is_leader
}

pub fn coordinator_raft_command_kind_matches(kind: i32, expected_kind: i32) -> bool {
    kind == expected_kind
}

pub fn coordinator_raft_should_create_fallback_view(view_is_empty: bool) -> bool {
    view_is_empty
}

pub fn coordinator_raft_should_wait_for_commit(commit_index: u64,
                                               target_index: u64) -> bool {
    commit_index < target_index
}

pub fn coordinator_raft_term_changed(current_term: u64, expected_term: u64) -> bool {
    current_term != expected_term
}

pub fn coordinator_raft_append_succeeded(start_ok: bool) -> bool {
    start_ok
}

pub fn coordinator_raft_should_learn(committed: bool) -> bool {
    committed
}

pub fn coordinator_raft_should_run_leader_init_path(current_phase: i32,
                                                    is_leader: bool) -> bool {
    current_phase == 0 && is_leader
}

pub fn coordinator_raft_should_skip_to_commit_from_init(current_phase: i32,
                                                        is_leader: bool) -> bool {
    current_phase == 0 && !is_leader
}

pub fn coordinator_raft_should_append_from_prepare(current_phase: i32) -> bool {
    current_phase == 1
}

pub fn coordinator_raft_should_finish_accept_without_learn(current_phase: i32,
                                                           committed: bool) -> bool {
    current_phase == 2 && !committed
}

pub fn coordinator_raft_submission_is_available(in_submission: bool,
                                                has_command: bool) -> bool {
    !in_submission && !has_command
}

pub fn coordinator_raft_append_is_available(in_append_entries: bool) -> bool {
    !in_append_entries
}

pub fn coordinator_raft_callback_is_ready(has_callback: bool) -> bool {
    has_callback
}

pub fn coordinator_raft_append_should_cancel_on_term_change(current_term: u64,
                                                            expected_term: u64) -> bool {
    current_term != expected_term
}
#endif
/*RUSTYCPP:GEN-BEGIN id=coordinator.1 version=1 rust_sha256=5c62e5fd1fa9df8f524f23385507c964b532abd55acf3ad643d57958983003c4*/
inline int32_t coordinator_raft_phase_value(int32_t phase, int32_t n_phase);
inline bool coordinator_raft_phase_is_prepare(int32_t phase);
inline bool coordinator_raft_phase_is_accept(int32_t phase);
inline bool coordinator_raft_phase_is_commit(int32_t phase);
inline uint32_t coordinator_raft_majority_count(uint32_t n_replica);
inline bool coordinator_raft_should_handle_wrong_leader(bool is_leader);
inline bool coordinator_raft_command_kind_matches(int32_t kind, int32_t expected_kind);
inline bool coordinator_raft_should_create_fallback_view(bool view_is_empty);
inline bool coordinator_raft_should_wait_for_commit(uint64_t commit_index, uint64_t target_index);
inline bool coordinator_raft_term_changed(uint64_t current_term, uint64_t expected_term);
inline bool coordinator_raft_append_succeeded(bool start_ok);
inline bool coordinator_raft_should_learn(bool committed);
inline bool coordinator_raft_should_run_leader_init_path(int32_t current_phase, bool is_leader);
inline bool coordinator_raft_should_skip_to_commit_from_init(int32_t current_phase, bool is_leader);
inline bool coordinator_raft_should_append_from_prepare(int32_t current_phase);
inline bool coordinator_raft_should_finish_accept_without_learn(int32_t current_phase, bool committed);
inline bool coordinator_raft_submission_is_available(bool in_submission, bool has_command);
inline bool coordinator_raft_append_is_available(bool in_append_entries);
inline bool coordinator_raft_callback_is_ready(bool has_callback);
inline bool coordinator_raft_append_should_cancel_on_term_change(uint64_t current_term, uint64_t expected_term);

inline int32_t coordinator_raft_phase_value(int32_t phase, int32_t n_phase) {
    return phase % n_phase;
}

inline bool coordinator_raft_phase_is_prepare(int32_t phase) {
    return phase == 1;
}

inline bool coordinator_raft_phase_is_accept(int32_t phase) {
    return phase == 2;
}

inline bool coordinator_raft_phase_is_commit(int32_t phase) {
    return phase == 3;
}

inline uint32_t coordinator_raft_majority_count(uint32_t n_replica) {
    return (n_replica / 2) + 1;
}

inline bool coordinator_raft_should_handle_wrong_leader(bool is_leader) {
    return !is_leader;
}

inline bool coordinator_raft_command_kind_matches(int32_t kind, int32_t expected_kind) {
    return kind == expected_kind;
}

inline bool coordinator_raft_should_create_fallback_view(bool view_is_empty) {
    return std::move(view_is_empty);
}

inline bool coordinator_raft_should_wait_for_commit(uint64_t commit_index, uint64_t target_index) {
    return rusty::detail::deref_if_pointer_like(commit_index) < rusty::detail::deref_if_pointer_like(target_index);
}

inline bool coordinator_raft_term_changed(uint64_t current_term, uint64_t expected_term) {
    return rusty::detail::deref_if_pointer_like(current_term) != rusty::detail::deref_if_pointer_like(expected_term);
}

inline bool coordinator_raft_append_succeeded(bool start_ok) {
    return std::move(start_ok);
}

inline bool coordinator_raft_should_learn(bool committed) {
    return std::move(committed);
}

inline bool coordinator_raft_should_run_leader_init_path(int32_t current_phase, bool is_leader) {
    return (rusty::detail::deref_if_pointer_like(current_phase) == static_cast<int32_t>(0)) && rusty::detail::deref_if_pointer_like(is_leader);
}

inline bool coordinator_raft_should_skip_to_commit_from_init(int32_t current_phase, bool is_leader) {
    return (rusty::detail::deref_if_pointer_like(current_phase) == static_cast<int32_t>(0)) && !is_leader;
}

inline bool coordinator_raft_should_append_from_prepare(int32_t current_phase) {
    return rusty::detail::deref_if_pointer_like(current_phase) == static_cast<int32_t>(1);
}

inline bool coordinator_raft_should_finish_accept_without_learn(int32_t current_phase, bool committed) {
    return (rusty::detail::deref_if_pointer_like(current_phase) == static_cast<int32_t>(2)) && !committed;
}

inline bool coordinator_raft_submission_is_available(bool in_submission, bool has_command) {
    return !in_submission && !has_command;
}

inline bool coordinator_raft_append_is_available(bool in_append_entries) {
    return !in_append_entries;
}

inline bool coordinator_raft_callback_is_ready(bool has_callback) {
    return std::move(has_callback);
}

inline bool coordinator_raft_append_should_cancel_on_term_change(uint64_t current_term, uint64_t expected_term) {
    return rusty::detail::deref_if_pointer_like(current_term) != rusty::detail::deref_if_pointer_like(expected_term);
}
/*RUSTYCPP:GEN-END id=coordinator.1*/

#if RUSTYCPP_RUST
pub struct CoordinatorRaftStateCore {
    curr_ballot_: rusty::Cell<u64>,
    in_submission_: rusty::Cell<bool>,
    in_append_entries_: rusty::Cell<bool>,
    min_index_: rusty::Cell<u64>,
}

impl CoordinatorRaftStateCore {
    // @safe
    fn new() -> CoordinatorRaftStateCore {
        CoordinatorRaftStateCore {
            curr_ballot_: rusty::Cell::<u64>::new_(1),
            in_submission_: rusty::Cell::<bool>::new_(false),
            in_append_entries_: rusty::Cell::<bool>::new_(false),
            min_index_: rusty::Cell::<u64>::new_(0),
        }
    }

    // @safe
    fn curr_ballot(&self) -> u64 {
        self.curr_ballot_.get()
    }

    // @safe
    fn set_curr_ballot(&mut self, ballot: u64) {
        self.curr_ballot_.set(ballot)
    }

    // @safe
    fn in_submission(&self) -> bool {
        self.in_submission_.get()
    }

    // @safe
    fn set_in_submission(&mut self, value: bool) {
        self.in_submission_.set(value)
    }

    // @safe
    fn in_append_entries(&self) -> bool {
        self.in_append_entries_.get()
    }

    // @safe
    fn set_in_append_entries(&mut self, value: bool) {
        self.in_append_entries_.set(value)
    }

    // @safe
    fn min_index(&self) -> u64 {
        self.min_index_.get()
    }

    // @safe
    fn set_min_index(&mut self, index: u64) {
        self.min_index_.set(index)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=coordinator.2 version=1 rust_sha256=53d0197a41d1325087a5368df7d9a92e32864ace8e31f343f43f7b5e224729a7*/
struct CoordinatorRaftStateCore;

struct CoordinatorRaftStateCore {
    rusty::Cell<uint64_t> curr_ballot_;
    rusty::Cell<bool> in_submission_;
    rusty::Cell<bool> in_append_entries_;
    rusty::Cell<uint64_t> min_index_;

    static CoordinatorRaftStateCore new_();
    uint64_t curr_ballot() const;
    void set_curr_ballot(uint64_t ballot);
    bool in_submission() const;
    void set_in_submission(bool value);
    bool in_append_entries() const;
    void set_in_append_entries(bool value);
    uint64_t min_index() const;
    void set_min_index(uint64_t index);
};


inline CoordinatorRaftStateCore CoordinatorRaftStateCore::new_() {
    return CoordinatorRaftStateCore{.curr_ballot_ = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(1)), .in_submission_ = rusty::Cell<bool>::new_(false), .in_append_entries_ = rusty::Cell<bool>::new_(false), .min_index_ = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0))};
}

inline uint64_t CoordinatorRaftStateCore::curr_ballot() const {
    return this->curr_ballot_.get();
}

inline void CoordinatorRaftStateCore::set_curr_ballot(uint64_t ballot) {
    this->curr_ballot_.set(std::move(ballot));
}

inline bool CoordinatorRaftStateCore::in_submission() const {
    return this->in_submission_.get();
}

inline void CoordinatorRaftStateCore::set_in_submission(bool value) {
    this->in_submission_.set(std::move(value));
}

inline bool CoordinatorRaftStateCore::in_append_entries() const {
    return this->in_append_entries_.get();
}

inline void CoordinatorRaftStateCore::set_in_append_entries(bool value) {
    this->in_append_entries_.set(std::move(value));
}

inline uint64_t CoordinatorRaftStateCore::min_index() const {
    return this->min_index_.get();
}

inline void CoordinatorRaftStateCore::set_min_index(uint64_t index) {
    this->min_index_.set(std::move(index));
}
/*RUSTYCPP:GEN-END id=coordinator.2*/

// @unsafe - inherits from non-@interface Coordinator and keeps borrowed raw
// back-pointers into RaftFrame/RaftServer state.
class CoordinatorRaft : public Coordinator {
 public:
//  static ballot_t next_slot_s;
  // @unsafe - borrowed RaftServer back-pointer set by RaftFrame; CoordinatorRaft
  // does not own or delete it.
  RaftServer* svr_ = nullptr;
 private:
  enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3, FORWARD = 4 };
  const int32_t n_phase_ = 4;

  // @unsafe - C-style cast on borrowed Coordinator::commo_ pointer. The
  // communicator is owned by RaftFrame, not this coordinator.
  RaftCommo *commo() {
    // Coordinator stores the legacy base pointer; RaftFrame guarantees that
    // this instance is the RaftCommo implementation for Raft coordinators.
    verify(commo_ != nullptr);
    // @unsafe
    { return (RaftCommo *) commo_; }
  }
  CoordinatorRaftStateCore state_core_;
  // removed `in_prepare_` and `in_accept`
  // debug-guard fields — neither was ever written or read in the
  // raft path (the comparable guards on the paxos / mencius
  // coordinators ARE used; CoordinatorRaft just had the shape
  // copied over).
 public:
  // migrated from
  // `shared_ptr<Marshallable>` to `janus::Command`.
  Command cmd_{};
  CoordinatorRaft(uint32_t coo_id,
                        int32_t benchmark,
                        rusty::Option<rusty::Arc<ClientStatus>> client_status,
                        uint32_t thread_id);
  // Set by RaftFrame during coordinator construction; n_replica() verifies it
  // before use.
  uint32_t n_replica_ = 0;
  slotid_t slot_id_ = 0;
  // Safe shared mutable counter - shares ownership with RaftFrame.
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_;
  // removed `uint64_t cmt_idx_ = 0;` —
  // declared but never written or read on CoordinatorRaft (the
  // sibling field on CoordinatorFpgaRaft IS live; this one was a
  // copy-paste that never got wired up).

  // @safe
  uint32_t n_replica() {
    verify(n_replica_ > 0);
    return n_replica_;
  }

  // @unsafe - raw pointer dereference svr_->
  bool IsLeader() ;
  // @unsafe - raw pointer dereference svr_->
  bool IsFPGALeader() ;

  // @unsafe - calls Log_warn (non-borrow-checked I/O), Uses Arc<Cell<T>> for safe shared mutable access
  slotid_t GetNextSlot() {
    // @unsafe { Log_warn is not borrow-checked }
    Log_warn("[RAFT] CoordinatorRaft::GetNextSlot called but not implemented for Raft");
    slot_id_ = slot_hint_->get();
    slot_hint_->set(slot_hint_->get() + 1);
    return 0;
  }

  // @safe
  uint32_t GetQuorum() {
    return coordinator_raft_majority_count(n_replica());
  }

  // @safe
  void DoTxAsync(TxRequest &req) override {}

  // @unsafe - raw pointer dereferences, shared_ptr, complex logic
  void Submit(const janus::Command& cmd,
              rusty::Function<void()> func = {},
              rusty::Function<void()> exe_callback = {}) override;

  // @unsafe - raw pointer dereference svr_->, address-of &
  void AppendEntries();
  // @unsafe - locks the coordinator mutex and invokes the completion callback.
  void Commit();
  // @unsafe - locks the coordinator mutex, invokes the callback, and advances phase.
  void LeaderLearn();

  // @safe
  void Reset() override {}
  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  void Restart() override {
    // @unsafe { Log_warn is not borrow-checked }
    Log_warn("[RAFT] CoordinatorRaft::Restart called but not implemented for Raft");
  }

  // @unsafe - calls AppendEntries which is @unsafe
  void GotoNextPhase();
};

} //namespace janus
