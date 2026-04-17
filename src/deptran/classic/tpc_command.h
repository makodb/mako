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

class TpcEmptyCommand {
 private:
  shared_ptr<BoxEvent<bool>> event{Reactor::create_sp_event<BoxEvent<bool>>()};

 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_TPC_EMPTY;
  Marshal& to_marshal(Marshal&) const;
  Marshal& from_marshal(Marshal&);
  void Wait() { event->wait(); };
  void Done() { event->set(1); };
};

class TpcNoopCommand {
  public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::CMD_NOOP;

  Marshal& to_marshal(Marshal&) const;
  Marshal& from_marshal(Marshal&);
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

template <>
struct TypedMarshallableAdapterTraits<janus::TpcEmptyCommand> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::TpcEmptyCommand,
                               MarshallDeputy::CMD_TPC_EMPTY>;
};

template <>
struct TypedMarshallableAdapterTraits<janus::TpcNoopCommand> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::TpcNoopCommand,
                               MarshallDeputy::CMD_NOOP>;
};

template <>
struct TypedMarshallableAdapterTraits<janus::TpcBatchCommand> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::TpcBatchCommand,
                               MarshallDeputy::CMD_TPC_BATCH>;
};

}  // namespace rrr
