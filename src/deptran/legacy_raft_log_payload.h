#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mako_commands.h"
#include "rrr/misc/serializable.hpp"

namespace janus {

// MDB-free representation of the Value bytes embedded in the historical
// VecPieceData command.  The numeric tags are part of the persisted wire
// format and must not be changed.
struct LegacyRaftValue {
  enum class Kind : int32_t {
    I32 = 0,
    I64 = 1,
    Double = 2,
    String = 3,
  };

  uint64_t version = 0;
  Kind kind = Kind::I32;
  int32_t i32_value = 0;
  int64_t i64_value = 0;
  double double_value = 0;
  std::string string_value;

  void save(rrr::BinaryWriteArchive& ar) const;
  void load(rrr::BinaryReadArchive& ar);
};

// Complete wire state for one historical SimpleCommand.  Vectors are used
// instead of the former set/map containers so a decoded persisted value keeps
// its original ordering when it is replicated to a catching-up follower.
struct LegacyRaftPiece {
  uint64_t id = 0;
  uint32_t type = 0;
  uint32_t inner_id = 0;
  uint64_t root_id = 0;
  uint32_t root_type = 0;
  int32_t client_id = -1;
  int32_t command_id = -1;
  int8_t reserved_rule_bit = 0;

  std::vector<int32_t> input_keys;
  std::vector<std::pair<int32_t, LegacyRaftValue>> input_values;
  std::vector<std::pair<int32_t, LegacyRaftValue>> output_values;

  int32_t output_size = 0;
  uint32_t partition_id = UINT32_MAX;
  uint64_t timestamp = 0;
  int32_t rank = 0;

  void save(rrr::BinaryWriteArchive& ar) const;
  void load(rrr::BinaryReadArchive& ar);
};

// Reader/writer for command kind 4, retained solely so Raft can replay log
// values written before the MemDB-backed transaction plane was removed.
//
// Historical wire format:
//   i32 piece count
//   repeated LegacyRaftPiece
//   double client-send time
//   i8 recovery flag
//
// New writes never use this type.  save() exists because RocksDB recovery can
// relay an old entry unchanged to another replica.
class LegacyVecPieceData : public rrr::Serializable<4> {
 public:
  std::vector<LegacyRaftPiece> pieces;
  double time_sent_from_client = -1e9;
  int8_t is_recovery_command = 0;

  LegacyVecPieceData() = default;

  void save(rrr::BinaryWriteArchive& ar) const;
  void load(rrr::BinaryReadArchive& ar);

  // Extract the only kind-4 shape emitted by Mako's former Raft application
  // path: one piece whose sole input value is string key 0.
  bool TryGetApplicationLog(std::string* payload,
                            uint32_t* partition_id) const;
};

// Explicitly referenced by the Raft recovery path.  Keeping registration
// behind a function both forces extraction from static archives and avoids a
// kind-4 registration race while the old VecPieceData implementation still
// exists during the staged cleanup.
void EnsureLegacyRaftLogPayloadRegistered();

}  // namespace janus

// The closed command set historically assigned kind 4 to VecPieceData.  This
// compatibility-only replacement intentionally shares that immutable wire
// number without adding a new command kind.
template <>
struct rrr::PayloadMember<janus::MakoCommands, janus::LegacyVecPieceData> {
  static constexpr bool value = true;
  static constexpr int32_t KIND = 4;
};
