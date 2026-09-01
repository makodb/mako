#pragma once

#include <std_compat.hpp>

#include <cstdint>
#include <string>

#include "mako_commands.h"
#include "view.h"

namespace janus {

// Wire payload for a replication view update. Kind 16 is an immutable
// MakoCommands discriminant; keep the field order and fixed-width archive
// encoding stable for persisted logs and RPC compatibility.
class ViewData
    : public srpc::Serializable<
          srpc::PayloadMember<MakoCommands, ViewData>::KIND> {
 public:
  View view_;
  parid_t partition_id_ = 0;

  ViewData() = default;

  explicit ViewData(const View& view) : view_(view) {}

  ViewData(const View& view, parid_t pid) : view_(view), partition_id_(pid) {}

  const View& GetView() const { return view_; }
  View& GetView() { return view_; }

  void save(srpc::BinaryWriteArchive& ar) const {
    srpc::Serialize_::serialize(view_.n_, ar);
    srpc::Serialize_::serialize(view_.view_id_, ar);
    srpc::Serialize_::serialize(view_.timestamp_, ar);
    srpc::Serialize_::serialize(static_cast<int32_t>(view_.leaders_.size()), ar);
    for (int leader : view_.leaders_) {
      srpc::Serialize_::serialize(leader, ar);
    }
    srpc::Serialize_::serialize(partition_id_, ar);
  }

  void load(srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(view_.n_, ar);
    srpc::Deserialize_::deserialize(view_.view_id_, ar);
    srpc::Deserialize_::deserialize(view_.timestamp_, ar);
    int32_t leader_count;
    srpc::Deserialize_::deserialize(leader_count, ar);
    srpc::verify(leader_count >= 0 && leader_count <= 10000);
    view_.leaders_.clear();
    view_.leaders_.reserve(leader_count);
    for (int i = 0; i < leader_count; ++i) {
      int leader;
      srpc::Deserialize_::deserialize(leader, ar);
      view_.leaders_.push_back(leader);
    }
    srpc::Deserialize_::deserialize(partition_id_, ar);
  }

  std::string ToString() const {
    return "ViewData{partition=" + std::to_string(partition_id_) +
           ", " + view_.ToString() + "}";
  }
};

// Explicit link/registration anchor for static-library consumers that decode
// a kind-16 Command without otherwise referencing this translation unit.
void EnsureViewDataRegistered();

}  // namespace janus
