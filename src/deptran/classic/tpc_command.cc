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
  ar << tx_id_;
  ar << ret_;
  // cmd_ is janus::Command — drive its archive op
  // directly instead of wrapping it in a temporary MarshallDeputy.
  // Wire format identical (`[v32 kind][payload]`).
  ar << cmd_;
}

void TpcPrepareCommand::load(BinaryReadArchive& ar) {
  ar >> tx_id_;
  ar >> ret_;
  // cmd_ load through Command's archive op.
  if (!cmd_.has_value()) {
    ar >> cmd_;
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
  ar << tx_id_;
  ar << ret_;
  ar << term;
  // drive cmd_ through Command's archive op directly.
  ar << cmd_;
  bool_t has_view_data = (sp_view_data_ != nullptr) ? 1 : 0;
  ar << has_view_data;
  if (has_view_data) {
    // was MarshallDeputy view_md(sp_view_data_) — Command
    // produces identical wire bytes via the same registry-dispatched
    // save/load path.
    janus::Command view_md = sp_view_data_;
    ar << view_md;
  }
}

void TpcCommitCommand::load(BinaryReadArchive& ar) {
  ar >> tx_id_;
  ar >> ret_;
  ar >> term;
  // cmd_ load through Command's archive op.
  if (!cmd_.has_value())
    ar >> cmd_;
  else
    verify(0);
  bool_t has_view_data;
  ar >> has_view_data;
  if (has_view_data) {
    janus::Command view_md;
    ar >> view_md;
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
  ar << size_;
  for (auto it = cmds_.begin(); it != cmds_.end(); ++it) {
    (*it)->save(ar);
  }
}

void TpcBatchCommand::load(BinaryReadArchive& ar) {
  ar >> size_;
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
