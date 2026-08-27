#include "legacy_raft_log_payload.h"

import rrr.debugging;

namespace janus {
namespace {

// The old writer asserted fewer than 10,000 input keys.  Use the same bound
// for every legacy collection so corrupt persisted lengths cannot trigger an
// unbounded allocation/loop during startup.
constexpr int64_t kMaxLegacyCollectionSize = 9999;

void SaveCount(std::size_t size, rrr::BinaryWriteArchive& ar) {
  rrr::verify(size <= static_cast<std::size_t>(kMaxLegacyCollectionSize));
  rrr::Serialize_::serialize(rrr::v64{static_cast<int64_t>(size)}, ar);
}

std::size_t LoadCount(rrr::BinaryReadArchive& ar) {
  rrr::v64 encoded{0};
  rrr::Deserialize_::deserialize(encoded, ar);
  const int64_t size = encoded.get();
  rrr::verify(size >= 0 && size <= kMaxLegacyCollectionSize);
  return static_cast<std::size_t>(size);
}

void SaveValuePairs(
    const std::vector<std::pair<int32_t, LegacyRaftValue>>& values,
    rrr::BinaryWriteArchive& ar) {
  SaveCount(values.size(), ar);
  for (const auto& [key, value] : values) {
    rrr::Serialize_::serialize(key, ar);
    value.save(ar);
  }
}

void LoadValuePairs(
    std::vector<std::pair<int32_t, LegacyRaftValue>>* values,
    rrr::BinaryReadArchive& ar) {
  rrr::verify(values != nullptr);
  const std::size_t size = LoadCount(ar);
  values->clear();
  values->reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    int32_t key = 0;
    LegacyRaftValue value;
    rrr::Deserialize_::deserialize(key, ar);
    value.load(ar);
    values->emplace_back(key, std::move(value));
  }
}

}  // namespace

void LegacyRaftValue::save(rrr::BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(version, ar);
  rrr::Serialize_::serialize(static_cast<int32_t>(kind), ar);
  switch (kind) {
    case Kind::I32:
      rrr::Serialize_::serialize(i32_value, ar);
      break;
    case Kind::I64:
      rrr::Serialize_::serialize(i64_value, ar);
      break;
    case Kind::Double:
      rrr::Serialize_::serialize(double_value, ar);
      break;
    case Kind::String:
      rrr::Serialize_::serialize(string_value, ar);
      break;
    default:
      rrr::verify(false);
  }
}

void LegacyRaftValue::load(rrr::BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(version, ar);
  int32_t encoded_kind = -1;
  rrr::Deserialize_::deserialize(encoded_kind, ar);
  rrr::verify(encoded_kind >= static_cast<int32_t>(Kind::I32) &&
              encoded_kind <= static_cast<int32_t>(Kind::String));
  kind = static_cast<Kind>(encoded_kind);

  switch (kind) {
    case Kind::I32:
      rrr::Deserialize_::deserialize(i32_value, ar);
      break;
    case Kind::I64:
      rrr::Deserialize_::deserialize(i64_value, ar);
      break;
    case Kind::Double:
      rrr::Deserialize_::deserialize(double_value, ar);
      break;
    case Kind::String:
      rrr::Deserialize_::deserialize(string_value, ar);
      break;
    default:
      rrr::verify(false);
  }
}

void LegacyRaftPiece::save(rrr::BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(id, ar);
  rrr::Serialize_::serialize(type, ar);
  rrr::Serialize_::serialize(inner_id, ar);
  rrr::Serialize_::serialize(root_id, ar);
  rrr::Serialize_::serialize(root_type, ar);
  rrr::Serialize_::serialize(client_id, ar);
  rrr::Serialize_::serialize(command_id, ar);
  rrr::Serialize_::serialize(reserved_rule_bit, ar);

  SaveCount(input_keys.size(), ar);
  for (const int32_t key : input_keys) {
    rrr::Serialize_::serialize(key, ar);
  }

  // TxWorkspace used a sentinel-terminated sequence rather than a map count.
  rrr::verify(input_values.size() <=
              static_cast<std::size_t>(kMaxLegacyCollectionSize));
  for (const auto& [key, value] : input_values) {
    rrr::verify(key >= 0);
    rrr::Serialize_::serialize(key, ar);
    value.save(ar);
  }
  rrr::Serialize_::serialize(static_cast<int32_t>(-1), ar);

  SaveValuePairs(output_values, ar);
  rrr::Serialize_::serialize(output_size, ar);
  rrr::Serialize_::serialize(partition_id, ar);
  rrr::Serialize_::serialize(timestamp, ar);
  rrr::Serialize_::serialize(rank, ar);
}

void LegacyRaftPiece::load(rrr::BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(id, ar);
  rrr::Deserialize_::deserialize(type, ar);
  rrr::Deserialize_::deserialize(inner_id, ar);
  rrr::Deserialize_::deserialize(root_id, ar);
  rrr::Deserialize_::deserialize(root_type, ar);
  rrr::Deserialize_::deserialize(client_id, ar);
  rrr::Deserialize_::deserialize(command_id, ar);
  rrr::Deserialize_::deserialize(reserved_rule_bit, ar);

  const std::size_t key_count = LoadCount(ar);
  input_keys.clear();
  input_keys.reserve(key_count);
  for (std::size_t i = 0; i < key_count; ++i) {
    int32_t key = 0;
    rrr::Deserialize_::deserialize(key, ar);
    input_keys.push_back(key);
  }

  input_values.clear();
  while (true) {
    int32_t key = -1;
    rrr::Deserialize_::deserialize(key, ar);
    if (key < 0) {
      break;
    }
    rrr::verify(input_values.size() <
                static_cast<std::size_t>(kMaxLegacyCollectionSize));
    LegacyRaftValue value;
    value.load(ar);
    input_values.emplace_back(key, std::move(value));
  }

  LoadValuePairs(&output_values, ar);
  rrr::Deserialize_::deserialize(output_size, ar);
  rrr::Deserialize_::deserialize(partition_id, ar);
  rrr::Deserialize_::deserialize(timestamp, ar);
  rrr::Deserialize_::deserialize(rank, ar);
}

void LegacyVecPieceData::save(rrr::BinaryWriteArchive& ar) const {
  rrr::verify(pieces.size() <= 1);
  rrr::Serialize_::serialize(static_cast<int32_t>(pieces.size()), ar);
  for (const auto& piece : pieces) {
    piece.save(ar);
  }
  rrr::Serialize_::serialize(time_sent_from_client, ar);
  rrr::Serialize_::serialize(is_recovery_command, ar);
}

void LegacyVecPieceData::load(rrr::BinaryReadArchive& ar) {
  int32_t piece_count = 0;
  rrr::Deserialize_::deserialize(piece_count, ar);
  // Production Mako emitted one piece.  The retired Raft lab could persist an
  // empty VecPieceData, so keep that shape readable as well.
  rrr::verify(piece_count == 0 || piece_count == 1);

  pieces.clear();
  pieces.reserve(static_cast<std::size_t>(piece_count));
  for (int32_t i = 0; i < piece_count; ++i) {
    LegacyRaftPiece piece;
    piece.load(ar);
    pieces.push_back(std::move(piece));
  }
  rrr::Deserialize_::deserialize(time_sent_from_client, ar);
  rrr::Deserialize_::deserialize(is_recovery_command, ar);
}

bool LegacyVecPieceData::TryGetApplicationLog(
    std::string* payload, uint32_t* partition_id_out) const {
  if (payload == nullptr || partition_id_out == nullptr || pieces.size() != 1) {
    return false;
  }

  const LegacyRaftPiece& piece = pieces.front();
  if (piece.input_values.size() != 1 ||
      piece.input_values.front().first != 0 ||
      piece.input_values.front().second.kind != LegacyRaftValue::Kind::String) {
    return false;
  }

  *payload = piece.input_values.front().second.string_value;
  *partition_id_out = piece.partition_id;
  return true;
}

void EnsureLegacyRaftLogPayloadRegistered() {
  // Registry insertion replaces the temporary old kind-4 factory during the
  // staged cleanup.  Calling this explicitly after static initialization also
  // makes initialization order deterministic.
  rrr::SerializableRegistry::reg<LegacyVecPieceData>(
      LegacyVecPieceData::static_kind());
}

}  // namespace janus
