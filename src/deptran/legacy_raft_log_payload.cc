#include "legacy_raft_log_payload.h"

import srpc.debugging;

namespace janus {
namespace {

// The old writer asserted fewer than 10,000 input keys.  Use the same bound
// for every legacy collection so corrupt persisted lengths cannot trigger an
// unbounded allocation/loop during startup.
constexpr int64_t kMaxLegacyCollectionSize = 9999;

void SaveCount(std::size_t size, srpc::BinaryWriteArchive& ar) {
  srpc::verify(size <= static_cast<std::size_t>(kMaxLegacyCollectionSize));
  srpc::Serialize_::serialize(srpc::v64{static_cast<int64_t>(size)}, ar);
}

std::size_t LoadCount(srpc::BinaryReadArchive& ar) {
  srpc::v64 encoded{0};
  srpc::Deserialize_::deserialize(encoded, ar);
  const int64_t size = encoded.get();
  srpc::verify(size >= 0 && size <= kMaxLegacyCollectionSize);
  return static_cast<std::size_t>(size);
}

void SaveValuePairs(
    const std::vector<std::pair<int32_t, LegacyRaftValue>>& values,
    srpc::BinaryWriteArchive& ar) {
  SaveCount(values.size(), ar);
  for (const auto& [key, value] : values) {
    srpc::Serialize_::serialize(key, ar);
    value.save(ar);
  }
}

void LoadValuePairs(
    std::vector<std::pair<int32_t, LegacyRaftValue>>* values,
    srpc::BinaryReadArchive& ar) {
  srpc::verify(values != nullptr);
  const std::size_t size = LoadCount(ar);
  values->clear();
  values->reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    int32_t key = 0;
    LegacyRaftValue value;
    srpc::Deserialize_::deserialize(key, ar);
    value.load(ar);
    values->emplace_back(key, std::move(value));
  }
}

}  // namespace

void LegacyRaftValue::save(srpc::BinaryWriteArchive& ar) const {
  srpc::Serialize_::serialize(version, ar);
  srpc::Serialize_::serialize(static_cast<int32_t>(kind), ar);
  switch (kind) {
    case Kind::I32:
      srpc::Serialize_::serialize(i32_value, ar);
      break;
    case Kind::I64:
      srpc::Serialize_::serialize(i64_value, ar);
      break;
    case Kind::Double:
      srpc::Serialize_::serialize(double_value, ar);
      break;
    case Kind::String:
      srpc::Serialize_::serialize(string_value, ar);
      break;
    default:
      srpc::verify(false);
  }
}

void LegacyRaftValue::load(srpc::BinaryReadArchive& ar) {
  srpc::Deserialize_::deserialize(version, ar);
  int32_t encoded_kind = -1;
  srpc::Deserialize_::deserialize(encoded_kind, ar);
  srpc::verify(encoded_kind >= static_cast<int32_t>(Kind::I32) &&
              encoded_kind <= static_cast<int32_t>(Kind::String));
  kind = static_cast<Kind>(encoded_kind);

  switch (kind) {
    case Kind::I32:
      srpc::Deserialize_::deserialize(i32_value, ar);
      break;
    case Kind::I64:
      srpc::Deserialize_::deserialize(i64_value, ar);
      break;
    case Kind::Double:
      srpc::Deserialize_::deserialize(double_value, ar);
      break;
    case Kind::String:
      srpc::Deserialize_::deserialize(string_value, ar);
      break;
    default:
      srpc::verify(false);
  }
}

void LegacyRaftPiece::save(srpc::BinaryWriteArchive& ar) const {
  srpc::Serialize_::serialize(id, ar);
  srpc::Serialize_::serialize(type, ar);
  srpc::Serialize_::serialize(inner_id, ar);
  srpc::Serialize_::serialize(root_id, ar);
  srpc::Serialize_::serialize(root_type, ar);
  srpc::Serialize_::serialize(client_id, ar);
  srpc::Serialize_::serialize(command_id, ar);
  srpc::Serialize_::serialize(reserved_rule_bit, ar);

  SaveCount(input_keys.size(), ar);
  for (const int32_t key : input_keys) {
    srpc::Serialize_::serialize(key, ar);
  }

  // TxWorkspace used a sentinel-terminated sequence rather than a map count.
  srpc::verify(input_values.size() <=
              static_cast<std::size_t>(kMaxLegacyCollectionSize));
  for (const auto& [key, value] : input_values) {
    srpc::verify(key >= 0);
    srpc::Serialize_::serialize(key, ar);
    value.save(ar);
  }
  srpc::Serialize_::serialize(static_cast<int32_t>(-1), ar);

  SaveValuePairs(output_values, ar);
  srpc::Serialize_::serialize(output_size, ar);
  srpc::Serialize_::serialize(partition_id, ar);
  srpc::Serialize_::serialize(timestamp, ar);
  srpc::Serialize_::serialize(rank, ar);
}

void LegacyRaftPiece::load(srpc::BinaryReadArchive& ar) {
  srpc::Deserialize_::deserialize(id, ar);
  srpc::Deserialize_::deserialize(type, ar);
  srpc::Deserialize_::deserialize(inner_id, ar);
  srpc::Deserialize_::deserialize(root_id, ar);
  srpc::Deserialize_::deserialize(root_type, ar);
  srpc::Deserialize_::deserialize(client_id, ar);
  srpc::Deserialize_::deserialize(command_id, ar);
  srpc::Deserialize_::deserialize(reserved_rule_bit, ar);

  const std::size_t key_count = LoadCount(ar);
  input_keys.clear();
  input_keys.reserve(key_count);
  for (std::size_t i = 0; i < key_count; ++i) {
    int32_t key = 0;
    srpc::Deserialize_::deserialize(key, ar);
    input_keys.push_back(key);
  }

  input_values.clear();
  while (true) {
    int32_t key = -1;
    srpc::Deserialize_::deserialize(key, ar);
    if (key < 0) {
      break;
    }
    srpc::verify(input_values.size() <
                static_cast<std::size_t>(kMaxLegacyCollectionSize));
    LegacyRaftValue value;
    value.load(ar);
    input_values.emplace_back(key, std::move(value));
  }

  LoadValuePairs(&output_values, ar);
  srpc::Deserialize_::deserialize(output_size, ar);
  srpc::Deserialize_::deserialize(partition_id, ar);
  srpc::Deserialize_::deserialize(timestamp, ar);
  srpc::Deserialize_::deserialize(rank, ar);
}

void LegacyVecPieceData::save(srpc::BinaryWriteArchive& ar) const {
  srpc::verify(pieces.size() <= 1);
  srpc::Serialize_::serialize(static_cast<int32_t>(pieces.size()), ar);
  for (const auto& piece : pieces) {
    piece.save(ar);
  }
  srpc::Serialize_::serialize(time_sent_from_client, ar);
  srpc::Serialize_::serialize(is_recovery_command, ar);
}

void LegacyVecPieceData::load(srpc::BinaryReadArchive& ar) {
  int32_t piece_count = 0;
  srpc::Deserialize_::deserialize(piece_count, ar);
  // Production Mako emitted one piece.  The retired Raft lab could persist an
  // empty VecPieceData, so keep that shape readable as well.
  srpc::verify(piece_count == 0 || piece_count == 1);

  pieces.clear();
  pieces.reserve(static_cast<std::size_t>(piece_count));
  for (int32_t i = 0; i < piece_count; ++i) {
    LegacyRaftPiece piece;
    piece.load(ar);
    pieces.push_back(std::move(piece));
  }
  srpc::Deserialize_::deserialize(time_sent_from_client, ar);
  srpc::Deserialize_::deserialize(is_recovery_command, ar);
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
  srpc::SerializableRegistry::reg<LegacyVecPieceData>(
      LegacyVecPieceData::static_kind());
}

}  // namespace janus
