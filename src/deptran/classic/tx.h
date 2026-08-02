#pragma once

#include "../tx.h"

namespace janus {

class TxClassic: public Tx {
 public:
  using Tx::Tx;
  rusty::Arc<BoxEvent<bool>> prepare_result{reactor_create_sp_event<BoxEvent<bool>>()};
  rusty::Arc<BoxEvent<int>> commit_result{reactor_create_sp_event<BoxEvent<int>>()};
  bool is_leader_hint_{false};
};

} // namespace janus
