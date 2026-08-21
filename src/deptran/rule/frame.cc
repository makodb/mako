
#include "../__dep__.h"
#include "../command.h"
#include "../command_marshaler.h"
#include "../communicator.h"
#include "../rcc/row.h"
#include "frame.h"
#include "coordinator.h"
#include "tx.h"

namespace janus {

REG_FRAME(MODE_RULE, vector<string>({"rule"}), RuleFrame);

Coordinator *RuleFrame::CreateCoordinator(cooid_t coo_id,
                                           Config *config,
                                           int benchmark,
                                           rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                           uint32_t id,
                                           shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  auto *coord = new CoordinatorRule(coo_id,
                                     benchmark,
                                     std::move(client_status),
                                     id);
  coord->txn_reg_ = txn_reg;
  coord->frame_ = this;
  return coord;
}

} // namespace janus
