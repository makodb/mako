#pragma once
#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include "../__dep__.h"
#include "../command.h"
#include "../mako_commands.h"
#include "deptran/procedure.h"

namespace janus {


class TxData;
// Explicit kind from the `PayloadMember<MakoCommands>` registration.
// Previously held a manual `kMarshallKind = MarshallDeputy::CMD_TPC_PREPARE`
// constant; the central marker registration (see mako_commands.h) now
// supplies the kind via the const-generic `Serializable` base.
//
// the nested polymorphic
// command field `cmd_` migrated from `shared_ptr<Marshallable>` to
// `janus::Command`.  Wire format unchanged; see
// `docs/dev/l10-unblock-plan.md`.
class TpcPrepareCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, TpcPrepareCommand>::KIND> {
 public:
  txnid_t tx_id_ = 0;
  int32_t ret_ = -1;
  Command cmd_{};

  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

// Explicit kind via the `PayloadMember<MakoCommands>` registration.
// nested `cmd_` migrated to janus::Command.
class TpcCommitCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, TpcCommitCommand>::KIND> {
 public:
  txnid_t tx_id_ = 0;
  int ret_ = -1;
  Command cmd_{};
  ballot_t term;
  // Optional view data for WRONG_LEADER responses
  rusty::Option<rusty::Arc<ViewData>> sp_view_data_{};

  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

// Explicit kind via the `PayloadMember<MakoCommands>` registration.
// Wire payload empty (no fields). The `event` member is local state
// used for sender↔apply synchronization on the leader; it is NOT
// serialized. Construction sites that need the leader-local "wrap,
// replicate, wait" pattern use `wrap_serializable_aliased<T>` to
// preserve `shared_ptr` aliasing — `serializable_cast<T>` on the apply
// path returns the SAME instance the sender is waiting on.
class TpcEmptyCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, TpcEmptyCommand>::KIND> {
 private:
  rusty::Arc<BoxEvent<bool>> event{create_sp_box_event<bool>()};

 public:
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
  // const: the sender parks and the apply path wakes it through
  // SHARED handles (pack_aliased sender side / serializable_cast apply
  // side). Mutation is confined to the BoxEvent behind the member
  // handle, so both are const-callable — required once payload
  // handles become const-view rusty::Arc.
  void Wait() const { event->wait(); };
  void Done() const { event->set(1); };
};

// Explicit registered kind. Stateless tag command — no
// fields, save/load are no-ops.
class TpcNoopCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, TpcNoopCommand>::KIND> {
 public:
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
};

// Explicit registered kind. Holds a vector of
// TpcCommitCommand; each element's save/load uses TpcCommitCommand's
// Serializable interface.
class TpcBatchCommand
    : public rrr::Serializable<
          rrr::PayloadMember<MakoCommands, TpcBatchCommand>::KIND> {
  uint32_t size_ = 0;
public:
  vector<rusty::Arc<TpcCommitCommand>> cmds_;

  void AddCmd(rusty::Arc<TpcCommitCommand> cmd);
  // @safe
  void AddCmds(vector<rusty::Arc<TpcCommitCommand>>& cmds);
  void ClearCmd();
  inline size_t Size() const { return cmds_.size(); }

  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

} // namespace janus

// removed an empty `namespace rrr {}` block
// that previously held `TypedMarshallableAdapterTraits<T>`
// specializations for the TPC command types.  The traits machinery
// went away in Phase 5b-5; all TPC types are now Serializables, and
// their `reg_serializable_in_deputy` registrations live in
// `tpc_command.cc`.
