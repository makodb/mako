#pragma once

#include "__dep__.h"

#include "rrr/rrr.hpp"
#include "mako_commands.h"

namespace janus {

class SimpleCommand;  // forward decl for ctor below; full def in procedure.h

// Workstream N L8: TypeList-derived kind. Wire payload preserved:
// int32_t type_ | key_t key_ | int32_t value_ (the cmd_id_,
// rule_mode_*, is_recovery_command_, zero_time_ fields are local
// state, never serialized).
class SimpleRWCommand : public rrr::Serializable<SimpleRWCommand,
                                                 MakoCommands> {
 public:
  int32_t type_;
  key_t key_;
  int32_t value_;
  pair<int32_t, int32_t> cmd_id_;
  bool rule_mode_on_and_is_original_path_only_command_;
  static double zero_time_;
  bool is_recovery_command_{false};
  SimpleRWCommand();
  // SimpleRWCommand(const SimpleRWCommand &o);
  SimpleRWCommand(shared_ptr<rrr::Marshallable> cmd);
  // Workstream N L10f-prep6bc (2026-05-03): Command-taking ctor.
  // rule/coordinator.cc:55 was the only call site that passed
  // shared_ptr<DerivedT>; now uses an explicit
  // static_pointer_cast<Marshallable>, so adding this overload no
  // longer creates ambiguity.
  SimpleRWCommand(const Command& cmd) : SimpleRWCommand(cmd.inner_marshallable()) {}
  // Workstream N L10f-1 (2026-05-04): SimpleCommand-direct ctor.
  // After CmdData stops inheriting Marshallable, callers that hold a
  // SimpleCommand directly must use this overload (not the
  // shared_ptr<Marshallable> path that no longer accepts derived
  // pointers).  Body just reads SimpleCommand fields directly.
  SimpleRWCommand(const SimpleCommand& cmd);
  std::string cmd_to_string();
  bool same_as(SimpleRWCommand &other);

  void save(rrr::BinaryWriteArchive& ar) const;
  void load(rrr::BinaryReadArchive& ar);

  bool IsRead();
  bool IsWrite();
  bool IsRecoveryCommand();
  static pair<int32_t, int32_t> GetCmdID(shared_ptr<rrr::Marshallable> cmd);
  static uint64_t GetCombinedCmdID(shared_ptr<rrr::Marshallable> cmd);
  static double GetCurrentMsTime();
  static void SetZeroTime();
  static double GetMsTimeElaps();
  static double GetCommandMsTime(shared_ptr<rrr::Marshallable> cmd);
  static double GetCommandMsTimeElaps(shared_ptr<rrr::Marshallable> cmd);
  static key_t GetKey(shared_ptr<rrr::Marshallable> cmd);
  static bool NeedRecordConflictInOriginalPath(shared_ptr<rrr::Marshallable> cmd);
  static bool Conflict(shared_ptr<rrr::Marshallable> cmd1, shared_ptr<rrr::Marshallable> cmd2);

  // Workstream N L10f-prep6ay/prep6bb (2026-05-03): Command-taking
  // overloads for the static methods.  Callers passing
  // shared_ptr<DerivedT> were updated (rule/coordinator.cc,
  // copilot/server.cc) to use std::static_pointer_cast<Marshallable>
  // explicitly to disambiguate.  Each overload forwards via
  // inner_marshallable().
  static pair<int32_t, int32_t> GetCmdID(const Command& cmd) {
    return GetCmdID(cmd.inner_marshallable());
  }
  static uint64_t GetCombinedCmdID(const Command& cmd) {
    return GetCombinedCmdID(cmd.inner_marshallable());
  }
  static double GetCommandMsTime(const Command& cmd) {
    return GetCommandMsTime(cmd.inner_marshallable());
  }
  static double GetCommandMsTimeElaps(const Command& cmd) {
    return GetCommandMsTimeElaps(cmd.inner_marshallable());
  }
  static key_t GetKey(const Command& cmd) {
    return GetKey(cmd.inner_marshallable());
  }
  static bool NeedRecordConflictInOriginalPath(const Command& cmd) {
    return NeedRecordConflictInOriginalPath(cmd.inner_marshallable());
  }
  static bool Conflict(const Command& cmd1, const Command& cmd2) {
    return Conflict(cmd1.inner_marshallable(), cmd2.inner_marshallable());
  }
  static uint64_t CombineInt32(pair<uint32_t, uint32_t> a) {
    return (((uint64_t)a.first) << 31) | a.second;
    // return (((uint64_t)a.first) * 1000000000) + a.second;
  }
  static uint64_t CombineInt32(uint32_t a, uint32_t b) {
    return (((uint64_t)a) << 31) | b;
    // return (((uint64_t)a) * 1000000000) + b;
  }
  static pair<uint32_t, uint32_t> GetInt32(uint64_t a) {
    return make_pair(a >> 31, a & ((1ll << 31) - 1));
    // return make_pair(a / 1000000000, a % 1000000000);
  }
  static int MaxFailure(int n) {
    return (n - 1) / 2;
  }
  static int RuleSuperMajority(int n) {
    int f = MaxFailure(n);
    return f + (f + 1) / 2 + 1;
  }
};

class KeyDistribution {
  unordered_map<key_t, int> key_count_;
  vector<pair<int, key_t>> sort_vec_;
 public:
  void Insert(key_t key);
  void Print();
};

class OneArmedBandit {
  static const int prediction_granularity = 100;
  bool records[100];
  int attempt_cnt = 0;
  int success_cnt = 0;
  int ptr = 0;
 public:
  // Record an attempt
  void Record(bool success);
  // Record a success attempt
  void RecordSuccess();
  // Record a fail attempt
  void RecordFail();
  // Consult attempt rate (0~1) --- Success rate +5% (maximum 100%), addition 5% is for recovery from pessimism
  double ConsultAttemptRate();
  // Consult attempt or not --- Success rate +5% (maximum 100%), addition 5% is for recovery from pessimism
  bool ConsultAttempt();
};

}
