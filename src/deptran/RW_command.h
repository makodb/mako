#pragma once

#include "__dep__.h"
#include "../rrr/misc/marshal.hpp"

namespace janus {

class SimpleRWCommand: public rrr::Marshallable {
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
  std::string cmd_to_string();
  bool same_as(SimpleRWCommand &other);
  rrr::Marshal& ToMarshal(rrr::Marshal& m) const override;
  rrr::Marshal& FromMarshal(rrr::Marshal& m) override;
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