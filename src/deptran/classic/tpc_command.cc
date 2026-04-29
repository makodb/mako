#include "tpc_command.h"
#include "../command.h"
#include "../command_marshaler.h"
#include "rrr/misc/marshal_serializable_bridge.hpp"

using namespace janus;

// Workstream N Phase 4a-3a: TpcPrepareCommand migrated to Serializable.
// Wire payload: tx_id (i64) | ret (i32) | nested MarshallDeputy
// (kind | inner save bytes). Byte-for-byte identical to the previous
// Marshallable encoding, since the nested-deputy archive operators
// (Phase 3f-prep) match the legacy `m << md` byte layout.
static int volatile x1 =
    rrr::reg_serializable_in_deputy<TpcPrepareCommand>(
        MarshallDeputy::CMD_TPC_PREPARE);

static int volatile x2 =
    MarshallDeputy::reg_initializer<TpcCommitCommand>(
        MarshallDeputy::CMD_TPC_COMMIT);

// Workstream N Phase 4a-2: TpcEmptyCommand migrated to Serializable.
// Wire payload is empty (no fields); on the wire this is byte-for-byte
// identical to the previous Marshallable encoding. Construction sites
// use `wrap_serializable_aliased<T>` to preserve `event` member
// aliasing for the leader-local sender↔apply sync. The read-side
// factory uses value semantics (creates a fresh TpcEmptyCommand with
// its own BoxEvent — nothing's waiting on it on the receiver).
static int volatile x3 =
    rrr::reg_serializable_in_deputy<TpcEmptyCommand>(
        MarshallDeputy::CMD_TPC_EMPTY);

// Workstream N Phase 4a-1: TpcNoopCommand migrated from Marshallable
// to Serializable. Uses the new `reg_serializable_in_deputy`
// registration path; on the wire it produces zero payload bytes (just
// the kind prefix that the MarshallDeputy framing layer prepends),
// byte-for-byte identical to the previous Marshallable encoding.
static int volatile x4 =
    rrr::reg_serializable_in_deputy<TpcNoopCommand>(
        MarshallDeputy::CMD_NOOP);

static int volatile x5 =
    MarshallDeputy::reg_initializer<TpcBatchCommand>(
        MarshallDeputy::CMD_TPC_BATCH);


// Workstream N Phase 4a-3a: TpcPrepareCommand serialization via
// BinaryWriteArchive / BinaryReadArchive. The nested
// `shared_ptr<Marshallable> cmd_` field is wrapped/unwrapped through
// a MarshallDeputy on each save/load — the Phase 3f-prep
// `operator<<>>(BinaryWriteArchive/BinaryReadArchive, MarshallDeputy)`
// overloads make this byte-for-byte equivalent to the legacy
// Marshal encoding.
void TpcPrepareCommand::save(BinaryWriteArchive& ar) const {
  ar << tx_id_;
  ar << ret_;
  MarshallDeputy md(cmd_);
  ar << md;
}

void TpcPrepareCommand::load(BinaryReadArchive& ar) {
  ar >> tx_id_;
  ar >> ret_;
  MarshallDeputy md;
  ar >> md;
  if (!cmd_) {
    if (md.inner() != nullptr) {
      cmd_ = md.inner();
    }
  } else {
    verify(0);
  }
}

Marshal& TpcCommitCommand::to_marshal(Marshal& m) const {
  m << tx_id_;
  m << ret_;
  m << term;  // Marshal the term field
  MarshallDeputy md(cmd_);
  m << md;
  // Marshal view data if present
  bool_t has_view_data = (sp_view_data_ != nullptr) ? 1 : 0;
  m << has_view_data;
  if (has_view_data) {
    MarshallDeputy view_md(sp_view_data_);
    m << view_md;
  }
  return m;
}

Marshal& TpcCommitCommand::from_marshal(Marshal& m) {
  m >> tx_id_;
  m >> ret_;
  m >> term;  // Unmarshal the term field
  MarshallDeputy md;
  m >> md;
  if (!cmd_)
    cmd_ = md.inner();
  else
    verify(0);
  // Unmarshal view data if present
  bool_t has_view_data;
  m >> has_view_data;
  if (has_view_data) {
    MarshallDeputy view_md;
    m >> view_md;
    sp_view_data_ = marshallable_cast<ViewData>(view_md);
  }
  return m;
}

// (TpcEmptyCommand's Marshal-based serialization removed in Phase
// 4a-2; see save/load methods inline in tpc_command.h.)

// (TpcNoopCommand's Marshal-based serialization removed in Phase 4a-1;
// see save/load methods inline in tpc_command.h.)

Marshal& TpcBatchCommand::to_marshal(Marshal& m) const {
  verify(size_ == cmds_.size());
  m << size_;
  for (auto it = cmds_.begin(); it != cmds_.end(); ++it) {
    (*it)->to_marshal(m);
  }
  return m;
}

Marshal& TpcBatchCommand::from_marshal(Marshal& m) {
  m >> size_;
  for (uint32_t i = 0; i < size_; i++) {
    cmds_.emplace_back(std::make_shared<TpcCommitCommand>());
    cmds_[i]->from_marshal(m);
  }
  return m;
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
