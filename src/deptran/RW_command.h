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
  // Workstream N L10f-2 (2026-05-04): Command is now the primary
  // ctor; the legacy `shared_ptr<Marshallable>` overload delegates
  // through Command to keep call sites that already wrap their
  // payload via `wrap_typed_marshallable` working unchanged.
  SimpleRWCommand(const Command& cmd);
  SimpleRWCommand(shared_ptr<rrr::Marshallable> cmd) : SimpleRWCommand(Command(std::move(cmd))) {}
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
  static double GetCurrentMsTime();
  static void SetZeroTime();
  static double GetMsTimeElaps();

  // Workstream N L10f-3 (2026-05-04): Command-taking statics are
  // primary; the legacy `shared_ptr<Marshallable>` overloads delegate
  // through Command(sp).
  static pair<int32_t, int32_t> GetCmdID(const Command& cmd);
  static uint64_t GetCombinedCmdID(const Command& cmd);
  static double GetCommandMsTime(const Command& cmd);
  static double GetCommandMsTimeElaps(const Command& cmd);
  static key_t GetKey(const Command& cmd);
  static bool NeedRecordConflictInOriginalPath(const Command& cmd);
  static bool Conflict(const Command& cmd1, const Command& cmd2);

  static pair<int32_t, int32_t> GetCmdID(shared_ptr<rrr::Marshallable> cmd) {
    return GetCmdID(Command(std::move(cmd)));
  }
  static uint64_t GetCombinedCmdID(shared_ptr<rrr::Marshallable> cmd) {
    return GetCombinedCmdID(Command(std::move(cmd)));
  }
  static double GetCommandMsTime(shared_ptr<rrr::Marshallable> cmd) {
    return GetCommandMsTime(Command(std::move(cmd)));
  }
  static double GetCommandMsTimeElaps(shared_ptr<rrr::Marshallable> cmd) {
    return GetCommandMsTimeElaps(Command(std::move(cmd)));
  }
  static key_t GetKey(shared_ptr<rrr::Marshallable> cmd) {
    return GetKey(Command(std::move(cmd)));
  }
  static bool NeedRecordConflictInOriginalPath(shared_ptr<rrr::Marshallable> cmd) {
    return NeedRecordConflictInOriginalPath(Command(std::move(cmd)));
  }
  static bool Conflict(shared_ptr<rrr::Marshallable> cmd1, shared_ptr<rrr::Marshallable> cmd2) {
    return Conflict(Command(std::move(cmd1)), Command(std::move(cmd2)));
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
