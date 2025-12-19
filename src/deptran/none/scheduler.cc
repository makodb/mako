#include "../config.h"
#include "../multi_value.h"
#include "../procedure.h"
#include "../txn_reg.h"
#include "scheduler.h"
#include "../rcc_rpc.h"

namespace janus {
int32_t SchedulerNone::Dispatch(cmdid_t cmd_id, shared_ptr<Marshallable> cmd,
								TxnOutput& ret_output, std::shared_ptr<ViewData>& view_data) {
	auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(cmd_id));
	DepId di;
	di.str = "dep";
	di.id = 0;
	SchedulerClassic::Dispatch(cmd_id, di, cmd, ret_output);
	sp_tx->fully_dispatched_->Wait();

	int ret = OnCommit(cmd_id, di, SUCCESS);  // it waits for the command to be executed
	// if (ret == WRONG_LEADER) {
	// 	Log_info("[DISPATCH_FLOW] SchedulerNone::Dispatch got WRONG_LEADER for cmd_id: 0x%lx", cmd_id);
	// }
	view_data = sp_tx->sp_view_data_;

	return ret;
}

} // namespace janus
