#pragma once
#include "__dep__.h"
#include "constants.h"
#include "../rrr/misc/marshal.hpp"


namespace janus {
class CmdData : public rrr::Marshallable {
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
  shared_ptr<ThreadSafeIntEvent> mongodb_finished = nullptr;

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
  virtual CmdData* Clone() const  {
    // deprecated.
    verify(0);
  };

  CmdData() : rrr::Marshallable(rrr::MarshallDeputy::CONTAINER_CMD) {}
  virtual ~CmdData() {};
  virtual rrr::Marshal& ToMarshal(rrr::Marshal&) const override;
  virtual rrr::Marshal& FromMarshal(rrr::Marshal&) override;
};
} // namespace janus
