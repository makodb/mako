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
};

class KeyDistribution {
  unordered_map<key_t, int> key_count_;
  vector<pair<int, key_t>> sort_vec_;
 public:
  void Insert(key_t key);
  void Print();
};

}
