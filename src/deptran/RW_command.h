#pragma once

#include "__dep__.h"

#include "rrr/rrr.hpp"
#include "mako_commands.h"

namespace janus {

class SimpleCommand;  // forward decl for ctor below; full def in procedure.h

// Explicit registered kind. Wire payload preserved:
// int32_t type_ | key_t key_ | int32_t value_ (the cmd_id_,
// rule_mode_*, is_recovery_command_, zero_time_ fields are local
// state, never serialized).
class SimpleRWCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, SimpleRWCommand>::KIND> {
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
  // Command is the only polymorphic
  // ctor.  The L10f-2 step 5 retirement of Marshallable removed the
  // legacy `shared_ptr<rrr::Marshallable>` overload — no production
  // callers remained.
  SimpleRWCommand(const Command& cmd);
  // SimpleCommand-direct ctor.
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

  // Command-taking statics are
  // primary; the legacy `shared_ptr<Marshallable>` overloads delegate
  // through Command(sp).
  static pair<int32_t, int32_t> GetCmdID(const Command& cmd);
  static uint64_t GetCombinedCmdID(const Command& cmd);
  static double GetCommandMsTime(const Command& cmd);
  static double GetCommandMsTimeElaps(const Command& cmd);
  static key_t GetKey(const Command& cmd);
  static bool NeedRecordConflictInOriginalPath(const Command& cmd);
  static bool Conflict(const Command& cmd1, const Command& cmd2);

  // removed the
  // `shared_ptr<rrr::Marshallable>` overloads of every static
  // helper above.  After Marshallable retires, no caller can
  // synthesize that argument shape.
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
