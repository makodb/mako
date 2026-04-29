#pragma once
#include "../__dep__.h"
#include "../command.h"
#include "deptran/procedure.h"

namespace janus {


class TxData;
class TpcPrepareCommand {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_PREPARE;
  txnid_t tx_id_ = 0;
  int32_t ret_ = -1;
  shared_ptr<Marshallable> cmd_{nullptr};

  Marshal& to_marshal(Marshal&) const;
  Marshal& from_marshal(Marshal&);
};

class TpcCommitCommand {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_COMMIT;
  txnid_t tx_id_ = 0;
  int ret_ = -1;
  shared_ptr<Marshallable> cmd_{nullptr};
  ballot_t term;
  // Optional view data for WRONG_LEADER responses
  std::shared_ptr<ViewData> sp_view_data_ = nullptr;
  
  Marshal& to_marshal(Marshal&) const;
  Marshal& from_marshal(Marshal&);
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
  
  Marshal& to_marshal(Marshal&) const;
  Marshal& from_marshal(Marshal&);
};

} // namespace janus

namespace rrr {

template <>
struct TypedMarshallableAdapterTraits<janus::TpcPrepareCommand> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::TpcPrepareCommand,
                               MarshallDeputy::CMD_TPC_PREPARE>;
};

template <>
struct TypedMarshallableAdapterTraits<janus::TpcCommitCommand> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::TpcCommitCommand,
                               MarshallDeputy::CMD_TPC_COMMIT>;
};

// (TpcEmptyCommand and TpcNoopCommand are Serializables now — no
// TypedMarshallableAdapter traits. See tpc_command.cc for their
// `reg_serializable_in_deputy` registrations; construction sites use
// `wrap_serializable_aliased` (TpcEmpty, preserves event-member
// aliasing) and `wrap_serializable` (TpcNoop, stateless).)

template <>
struct TypedMarshallableAdapterTraits<janus::TpcBatchCommand> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::TpcBatchCommand,
                               MarshallDeputy::CMD_TPC_BATCH>;
};

}  // namespace rrr
