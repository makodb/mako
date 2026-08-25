#pragma once
#include "__dep__.h"
#include "constants.h"

#include "srpc/srpc.hpp"


namespace janus {
// CmdData no longer inherits
// Marshallable.  The metadata fields + virtuals stay; the
// Marshallable-based polymorphism (kind_, to_marshal/from_marshal)
// was vestigial.
class CmdData {
 public:
  cmdid_t id_ = 0;
  cmdtype_t type_ = 0;
  innid_t inn_id_ = 0;
  cmdid_t root_id_ = 0;
  cmdtype_t root_type_ = 0;

  /****global unique id begin******/
  // [Jetpack] TODO: initialize?
  int32_t client_id_ = -1;
  int32_t cmd_id_in_client_ = -1;
  // pair<int, int> cmd_id_ = make_pair<int, int>(-1, -1);
  /****global unique id end******/
  // for rule use
  // this is true only when rule mode is on, and fastpath is disabled for this command
  bool_t rule_mode_on_and_is_original_path_only_command_ = false;

  virtual innid_t inn_id() const {
    return inn_id_;
  }
  virtual cmdtype_t type() {
    return type_;
  }
  virtual void Merge(CmdData&) {
    verify(0);
  }
  virtual set<parid_t>& GetPartitionIds() {
    verify(0);
    static set<parid_t> l;
    return l;
  }
  virtual void Reset() {
    verify(0);
  }
  // removed `virtual CmdData* Clone() const`
  // (and the matching `SimpleCommand::Clone` override).  The method
  // was annotated `// deprecated.` and `verify(0)`-stubbed; no caller
  // anywhere in the codebase invoked it (`grep "->Clone()"` /
  // `".Clone()"` across src/ returns zero production hits — only the
  // two definitions themselves).  Same Phase 4e-3 removal covered
  // `SimpleCommand::RootCmd()` and the `SimpleCommand::root_`
  // back-pointer it returned: both were unused (`root_` was written
  // by `TxData::GetReadyPiecesData` / `TxData::GetNextReadySubCmd`
  // but never read by anything).

  CmdData() = default;
  virtual ~CmdData() = default;
  // removed `to_marshal` / `from_marshal`
  // overrides. CmdData is never registered with
  // `MarshallDeputy::reg_initializer` and never instantiated directly
  // (no `make_shared<CmdData>` / `new CmdData` callers); its only
  // subclasses are `SimpleCommand` (serialized via free `operator<<`
  // overloads on `Marshal&` / `BinaryWriteArchive&` in
  // `command_marshaler.cc`, never via virtual dispatch) and `TxData`
  //. The base
  // `Marshallable::to_marshal` / `from_marshal` `verify(0)` defaults
  // remain in place.
};
} // namespace janus
