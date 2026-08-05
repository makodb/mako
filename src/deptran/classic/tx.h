#pragma once

#include "../tx.h"

namespace janus {

class TxClassic: public Tx {
 public:
  using Tx::Tx;
  rusty::Arc<BoxEvent<bool>> prepare_result{create_sp_box_event<bool>()};
  rusty::Arc<BoxEvent<int>> commit_result{create_sp_box_event<int>()};
  bool is_leader_hint_{false};
};

} // namespace janus
