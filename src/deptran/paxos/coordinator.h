#pragma once

#include <std_compat.hpp>

#include <mutex>
#include <utility>

#include <rusty/function.hpp>

#include "../constants.h"
#include "../coordinator.h"
#include "../mako_commands.h"

namespace janus {

class MultiPaxosCommo;

class CoordinatorMultiPaxos : public Coordinator {
 public:
  enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3 };

  bool in_submission_ = false;
  bool in_accept_ = false;
  bool in_commit_ = false;
  Command cmd_{};
  ballot_t curr_ballot_ = 1;
  slotid_t slot_id_ = 0;
  phase_t phase_ = 0;
  bool committed_ = false;
  std::recursive_mutex mtx_{};
  rusty::Function<void()> commit_callback_ = [] { rrr::verify(false); };

  CoordinatorMultiPaxos() = default;
  ~CoordinatorMultiPaxos() override = default;

  MultiPaxosCommo* commo();

  bool IsLeader() const { return loc_id_ == 0; }

  void SetCommand(const Command& cmd) { cmd_ = cmd; }
  void SetSlot(slotid_t slot) { slot_id_ = slot; }

  void Submit(const Command& cmd,
              rusty::Function<void()> commit_callback = {});
  void Accept();
  void Commit();
  void GotoNextPhase();
};

class BulkCoordinatorMultiPaxos : public CoordinatorMultiPaxos {
 public:
  BulkCoordinatorMultiPaxos() = default;

  void BulkSubmit(const Command& cmd,
                  rusty::Function<void()> commit_callback = {});
  void Accept();
  void Commit();
  void GotoNextPhase();
};

}  // namespace janus
