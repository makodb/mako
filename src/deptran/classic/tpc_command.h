#pragma once
#include "../__dep__.h"
#include "../command.h"
#include "deptran/procedure.h"

namespace janus {


class TxData;
// Workstream N Phase 4a-3a: migrated from Marshallable to
// Serializable. The nested `cmd_` field (shared_ptr<Marshallable>)
// is wrapped/unwrapped through a MarshallDeputy on each save/load,
// using the Phase 3f-prep BinaryWriteArchive/BinaryReadArchive
// operator<<>> overloads for MarshallDeputy. Wire format byte-for-
// byte identical to the previous Marshallable encoding.
class TpcPrepareCommand {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_PREPARE;
  txnid_t tx_id_ = 0;
  int32_t ret_ = -1;
  shared_ptr<Marshallable> cmd_{nullptr};

  int32_t kind() const { return kMarshallKind; }
  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

// Workstream N Phase 4a-3b: migrated from Marshallable to
// Serializable. Has nested `cmd_` (shared_ptr<Marshallable>) and
// optional `sp_view_data_` (shared_ptr<ViewData>) fields, both
// serialized through the Phase 3f-prep BinaryWriteArchive /
// BinaryReadArchive operator<<>> overloads for MarshallDeputy. Wire
// format byte-for-byte identical to the previous Marshallable
// encoding.
class TpcCommitCommand {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_COMMIT;
  txnid_t tx_id_ = 0;
  int ret_ = -1;
  shared_ptr<Marshallable> cmd_{nullptr};
  ballot_t term;
  // Optional view data for WRONG_LEADER responses
  std::shared_ptr<ViewData> sp_view_data_ = nullptr;

  int32_t kind() const { return kMarshallKind; }
  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

// Workstream N Phase 4a-2: migrated from Marshallable to Serializable.
// The wire payload is empty (no fields). The `event` member is local
// state used for sender↔apply synchronization on the leader; it is
// NOT serialized. Construction sites that need the leader-local
// "wrap, replicate, wait" pattern use `wrap_serializable_aliased<T>`
// to preserve `shared_ptr` aliasing — `serializable_cast<T>` on the
// apply path returns the SAME instance the sender is waiting on.
class TpcEmptyCommand {
 private:
  shared_ptr<BoxEvent<bool>> event{Reactor::create_sp_event<BoxEvent<bool>>()};

 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_EMPTY;
  int32_t kind() const { return kMarshallKind; }
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
  void Wait() { event->wait(); };
  void Done() { event->set(1); };
};

// Workstream N Phase 4a-1: migrated from Marshallable to Serializable.
// Stateless tag command — no fields, save/load are no-ops.
// Registered with `MarshallDeputy::reg_initializer` via the
// `reg_serializable_in_deputy` bridge in tpc_command.cc.
class TpcNoopCommand {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_NOOP;

  int32_t kind() const { return kMarshallKind; }
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
};

// Workstream N Phase 4a-3c: migrated from Marshallable to
// Serializable. Holds a vector of TpcCommitCommand; each element's
// save/load uses TpcCommitCommand's Serializable interface (4a-3b).
// Wire format byte-for-byte identical to the previous Marshallable
// encoding (uint32_t size prefix + concatenated commit bytes).
class TpcBatchCommand {
  uint32_t size_ = 0;
public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_BATCH;
  vector<shared_ptr<TpcCommitCommand> > cmds_;

  void AddCmd(shared_ptr<TpcCommitCommand> cmd);
  // @safe
  void AddCmds(vector<shared_ptr<TpcCommitCommand> >& cmds);
  void ClearCmd();
  inline size_t Size() const { return cmds_.size(); }

  int32_t kind() const { return kMarshallKind; }
  void save(BinaryWriteArchive& ar) const;
  void load(BinaryReadArchive& ar);
};

} // namespace janus

namespace rrr {

// (All TPC command types are now Serializables — no
// TypedMarshallableAdapter traits. See tpc_command.cc for their
// `reg_serializable_in_deputy` registrations. Construction sites use
// `wrap_serializable` (Prepare/Commit/Batch/Noop, stateless) or
// `wrap_serializable_aliased` (Empty, preserves event-member
// aliasing). Cast sites continue to use `marshallable_cast<T>` —
// the Phase 4a-prep bridge overload routes Serializable T's to
// `serializable_cast<T>` and synthesizes a `shared_ptr<T>`.)

}  // namespace rrr
