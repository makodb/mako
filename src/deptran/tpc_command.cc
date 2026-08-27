#include "tpc_command.h"
#include "rrr/misc/serializable.hpp"

using namespace janus;

// Registry keys come from each payload's explicit MakoCommands membership.
static int volatile x1 = rrr::SerializableRegistry::reg<TpcPrepareCommand>(TpcPrepareCommand::static_kind());
static int volatile x2 = rrr::SerializableRegistry::reg<TpcCommitCommand>(TpcCommitCommand::static_kind());
static int volatile x3 = rrr::SerializableRegistry::reg<TpcEmptyCommand>(TpcEmptyCommand::static_kind());
static int volatile x4 = rrr::SerializableRegistry::reg<TpcNoopCommand>(TpcNoopCommand::static_kind());
static int volatile x5 = rrr::SerializableRegistry::reg<TpcBatchCommand>(TpcBatchCommand::static_kind());
static int volatile x6 = (EnsureViewDataRegistered(), 0);


// TpcPrepareCommand serialization via BinaryWriteArchive /
// BinaryReadArchive. Command preserves the historical kind-plus-payload wire
// encoding directly.
void TpcPrepareCommand::save(BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(tx_id_, ar);
  rrr::Serialize_::serialize(ret_, ar);
  // cmd_ is janus::Command — drive its archive op
  // directly instead of wrapping it in a temporary MarshallDeputy.
  // Wire format identical (`[v32 kind][payload]`).
  rrr::Serialize_::serialize(cmd_, ar);
}

void TpcPrepareCommand::load(BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(tx_id_, ar);
  rrr::Deserialize_::deserialize(ret_, ar);
  // cmd_ load through Command's archive op.
  if (!cmd_.has_value()) {
    rrr::Deserialize_::deserialize(cmd_, ar);
  } else {
    verify(0);
  }
}

// TpcCommitCommand serialization keeps the historical field order and uses
// Command for both nested command and optional view payloads.
void TpcCommitCommand::save(BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(tx_id_, ar);
  rrr::Serialize_::serialize(ret_, ar);
  rrr::Serialize_::serialize(term, ar);
  // drive cmd_ through Command's archive op directly.
  rrr::Serialize_::serialize(cmd_, ar);
  bool_t has_view_data = sp_view_data_.is_some() ? 1 : 0;
  rrr::Serialize_::serialize(has_view_data, ar);
  if (has_view_data) {
    // was MarshallDeputy view_md(sp_view_data_) — Command
    // produces identical wire bytes via the same registry-dispatched
    // save/load path.
    janus::Command view_md = sp_view_data_.unwrap().clone();
    rrr::Serialize_::serialize(view_md, ar);
  }
}

void TpcCommitCommand::load(BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(tx_id_, ar);
  rrr::Deserialize_::deserialize(ret_, ar);
  rrr::Deserialize_::deserialize(term, ar);
  // cmd_ load through Command's archive op.
  if (!cmd_.has_value())
    rrr::Deserialize_::deserialize(cmd_, ar);
  else
    verify(0);
  bool_t has_view_data;
  rrr::Deserialize_::deserialize(has_view_data, ar);
  if (has_view_data) {
    janus::Command view_md;
    rrr::Deserialize_::deserialize(view_md, ar);
    sp_view_data_ = marshallable_cast<ViewData>(view_md);
  }
}

// (TpcEmptyCommand's Marshal-based serialization removed in Phase
// 4a-2; see save/load methods inline in tpc_command.h.)

// (TpcNoopCommand's Marshal-based serialization removed in Phase 4a-1;
// see save/load methods inline in tpc_command.h.)

// TpcBatchCommand serialization via
// BinaryWriteArchive / BinaryReadArchive. Iterates each commit and
// delegates to its save/load. uint32_t size prefix matches the
// legacy encoding.
void TpcBatchCommand::save(BinaryWriteArchive& ar) const {
  verify(size_ == cmds_.size());
  rrr::Serialize_::serialize(size_, ar);
  for (auto it = cmds_.begin(); it != cmds_.end(); ++it) {
    (*it)->save(ar);
  }
}

void TpcBatchCommand::load(BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(size_, ar);
  for (uint32_t i = 0; i < size_; i++) {
    auto cmd = rusty::Arc<TpcCommitCommand>::make();
    // @unsafe - unique-owner mutation window (factory-fresh Arc).
    cmd.get_mut().unwrap().load(ar);
    cmds_.push_back(std::move(cmd));
  }
}

void TpcBatchCommand::AddCmd(rusty::Arc<TpcCommitCommand> cmd) {
  size_++;
  cmds_.push_back(std::move(cmd));
}

void TpcBatchCommand::AddCmds(vector<rusty::Arc<TpcCommitCommand>>& cmds) {
  cmds_ = cmds;
  size_ = cmds_.size();
}

void TpcBatchCommand::ClearCmd() {
  cmds_.clear();
  size_ = 0;
}
