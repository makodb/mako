#pragma once
#include "../__dep__.h"
#include "../command.h"
#include "../mako_commands.h"
#include "deptran/procedure.h"

namespace janus {


class TxData;
// Workstream N L8: TypeList-derived kind from `MakoCommands` position.
// Previously held a manual `kMarshallKind = MarshallDeputy::CMD_TPC_PREPARE`
// constant; the position in `MakoCommands` (see mako_commands.h) now
// supplies the kind via the `Serializable` CRTP base.
class TpcPrepareCommand : public rrr::Serializable<TpcPrepareCommand,
                                                   MakoCommands> {
 public:
  txnid_t tx_id_ = 0;
  int32_t ret_ = -1;
  shared_ptr<Marshallable> cmd_{nullptr};

  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

// Workstream N L8: TypeList-derived kind via `MakoCommands` position.
class TpcCommitCommand : public rrr::Serializable<TpcCommitCommand,
                                                  MakoCommands> {
 public:
  txnid_t tx_id_ = 0;
  int ret_ = -1;
  shared_ptr<Marshallable> cmd_{nullptr};
  ballot_t term;
  // Optional view data for WRONG_LEADER responses
  std::shared_ptr<ViewData> sp_view_data_ = nullptr;

  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

// Workstream N L8: TypeList-derived kind via `MakoCommands` position.
// Wire payload empty (no fields). The `event` member is local state
// used for sender↔apply synchronization on the leader; it is NOT
// serialized. Construction sites that need the leader-local "wrap,
// replicate, wait" pattern use `wrap_serializable_aliased<T>` to
// preserve `shared_ptr` aliasing — `serializable_cast<T>` on the apply
// path returns the SAME instance the sender is waiting on.
class TpcEmptyCommand : public rrr::Serializable<TpcEmptyCommand,
                                                 MakoCommands> {
 private:
  shared_ptr<BoxEvent<bool>> event{Reactor::create_sp_event<BoxEvent<bool>>()};

 public:
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
  void Wait() { event->wait(); };
  void Done() { event->set(1); };
};

// Workstream N L8: TypeList-derived kind. Stateless tag command — no
// fields, save/load are no-ops.
class TpcNoopCommand : public rrr::Serializable<TpcNoopCommand,
                                                MakoCommands> {
 public:
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
};

// Workstream N L8: TypeList-derived kind. Holds a vector of
// TpcCommitCommand; each element's save/load uses TpcCommitCommand's
// Serializable interface.
class TpcBatchCommand : public rrr::Serializable<TpcBatchCommand,
                                                 MakoCommands> {
  uint32_t size_ = 0;
public:
  vector<shared_ptr<TpcCommitCommand> > cmds_;

  void AddCmd(shared_ptr<TpcCommitCommand> cmd);
  // @safe
  void AddCmds(vector<shared_ptr<TpcCommitCommand> >& cmds);
  void ClearCmd();
  inline size_t Size() const { return cmds_.size(); }

  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

} // namespace janus

// Workstream N Phase 4e-2: removed an empty `namespace rrr {}` block
// that previously held `TypedMarshallableAdapterTraits<T>`
// specializations for the TPC command types.  The traits machinery
// went away in Phase 5b-5; all TPC types are now Serializables, and
// their `reg_serializable_in_deputy` registrations live in
// `tpc_command.cc`.
