#include "tpc_command.h"
#include "../command.h"
#include "../command_marshaler.h"
#include "rrr/misc/serializable.hpp"

using namespace janus;

// registrations switched to the no-arg
// `SerializableRegistry::reg<T>(T::static_kind())` overload — kind is auto-derived
// from each type's `static_kind()` method (provided by the
// `Serializable<T, MakoCommands>` CRTP base, which returns the type's
// 1-indexed position in the `MakoCommands` TypeList).
static int volatile x1 = rrr::SerializableRegistry::reg<TpcPrepareCommand>(TpcPrepareCommand::static_kind());
static int volatile x2 = rrr::SerializableRegistry::reg<TpcCommitCommand>(TpcCommitCommand::static_kind());
static int volatile x3 = rrr::SerializableRegistry::reg<TpcEmptyCommand>(TpcEmptyCommand::static_kind());
static int volatile x4 = rrr::SerializableRegistry::reg<TpcNoopCommand>(TpcNoopCommand::static_kind());
static int volatile x5 = rrr::SerializableRegistry::reg<TpcBatchCommand>(TpcBatchCommand::static_kind());


// TpcPrepareCommand serialization via
// BinaryWriteArchive / BinaryReadArchive. The nested
// `shared_ptr<Marshallable> cmd_` field is wrapped/unwrapped through
// a MarshallDeputy on each save/load — the Phase 3f-prep
// `operator<<>>(BinaryWriteArchive/BinaryReadArchive, MarshallDeputy)`
// overloads make this byte-for-byte equivalent to the legacy
// Marshal encoding.
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

// TpcCommitCommand serialization via
// BinaryWriteArchive / BinaryReadArchive. Both nested
// `cmd_` (shared_ptr<Marshallable>) and optional
// `sp_view_data_` (shared_ptr<ViewData>) are wrapped/unwrapped through
// MarshallDeputy on each save/load — the Phase 3f-prep
// `operator<<>>(BinaryWriteArchive/BinaryReadArchive, MarshallDeputy)`
// overloads make this byte-for-byte equivalent to the legacy
// Marshal encoding.
void TpcCommitCommand::save(BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(tx_id_, ar);
  rrr::Serialize_::serialize(ret_, ar);
  rrr::Serialize_::serialize(term, ar);
  // drive cmd_ through Command's archive op directly.
  rrr::Serialize_::serialize(cmd_, ar);
  bool_t has_view_data = (sp_view_data_ != nullptr) ? 1 : 0;
  rrr::Serialize_::serialize(has_view_data, ar);
  if (has_view_data) {
    // was MarshallDeputy view_md(sp_view_data_) — Command
    // produces identical wire bytes via the same registry-dispatched
    // save/load path.
    janus::Command view_md = sp_view_data_;
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
    cmds_.emplace_back(std::make_shared<TpcCommitCommand>());
    cmds_[i]->load(ar);
  }
}

void TpcBatchCommand::AddCmd(shared_ptr<TpcCommitCommand> cmd) {
  size_++;
  cmds_.push_back(cmd);
}

void TpcBatchCommand::AddCmds(vector<shared_ptr<TpcCommitCommand> >& cmds) {
  cmds_ = cmds;
  size_ = cmds_.size();
}

void TpcBatchCommand::ClearCmd() {
  cmds_.clear();
  size_ = 0;
}
