#include "../constants.h"
#include "../tx.h"
#include "../procedure.h"
#include "../coordinator.h"
#include "../2pl/tx.h"
#include "../tx.h"
#include "../classic/tpc_command.h"
#include "scheduler.h"

namespace janus
{

int SchedulerNoneCopilot::OnCommit(cmdid_t tx_id,
								   struct DepId dep_id,
								   int commit_or_abort) {
	std::lock_guard<std::recursive_mutex> lock(mtx_);
	Log_debug("{}: at site {}, tx: %" PRIx64,
            __FUNCTION__, this->site_id_, tx_id);
	auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(tx_id));
	// TODO maybe change inuse to an event?
	//  verify(!sp_tx->inuse);
	//  sp_tx->inuse = true;
	//
	//always true
	if (Config::GetConfig()->IsReplicated()) {
		// fill the payload on a LOCAL, then freeze it into a shared Arc —
		// rusty::Arc payloads are const-view after construction.
		TpcCommitCommand commit_cmd_local{};
		commit_cmd_local.tx_id_ = tx_id;
		commit_cmd_local.ret_ = commit_or_abort;
		commit_cmd_local.cmd_ = sp_tx->cmd_;
		auto cmd = rusty::Arc<TpcCommitCommand>::make(std::move(commit_cmd_local));
		sp_tx->is_leader_hint_ = true;

#ifdef COPILOT_TIME_DEBUG
	struct timeval tp;
	gettimeofday(&tp, NULL);
	Log_info("[0/start] [tx={}] before submit {:.3f}", tx_id, tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif
		bool enable_pingpong_batching = false;
		if (enable_pingpong_batching) {
			auto copilot_server = static_cast<CopilotServer*>(rep_sched_);
			batch_buffer_.push_back(std::move(cmd));
			// uint64_t before, after;
			// before = Time::now(true);
			if (!in_waiting_) {
				
				int _no_use;
				if (copilot_server->WillWait(_no_use))
					in_waiting_ = true;
				copilot_server->WaitForPingPong();
				in_waiting_ = false;

				// after = Time::now(true);
				// Log_info("wait {}", after - before);
				
				// batch assembled on a LOCAL, then frozen into a shared Arc
				// (AddCmds needs mutable access; Arc payloads are const-view).
				TpcBatchCommand batch_cmd_local{};
				batch_cmd_local.AddCmds(batch_buffer_);
				batch_buffer_.clear();
				auto batch_cmd =
						rusty::Arc<TpcBatchCommand>::make(std::move(batch_cmd_local));

				submit_++;
				total_ += batch_cmd->Size();
				if (submit_ % 1000 == 0)
					Log_info("avg batch size {:f}", (double)total_/submit_);
				shared_ptr<Coordinator> coo{CreateRepCoord(dep_id.id)};
				coo->Submit(std::move(batch_cmd));
			}
		} else {
			shared_ptr<Coordinator> coo{CreateRepCoord(dep_id.id)};
			// batch assembled on a LOCAL, then frozen into a shared Arc
			// (AddCmds needs mutable access; Arc payloads are const-view).
			TpcBatchCommand batch_cmd_local{};
			batch_buffer_.push_back(std::move(cmd));
			batch_cmd_local.AddCmds(batch_buffer_);
			batch_buffer_.clear();
			auto batch_cmd =
					rusty::Arc<TpcBatchCommand>::make(std::move(batch_cmd_local));
			coo->Submit(std::move(batch_cmd));
		}
		sp_tx->commit_result->wait();
#ifdef COPILOT_TIME_DEBUG
	struct timeval tp;
    gettimeofday(&tp, NULL);
    Log_info("[end] [tx={}] after commit_result wait {:.3f}", tx_id, tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif
	} else {
		if (commit_or_abort == SUCCESS) {
			DoCommit(*sp_tx);
		} else if (commit_or_abort == REJECT) {
		//      exec->AbortLaunch(res, callback);
			DoAbort(*sp_tx);
		} else {
			verify(0);
		}
	}
	return 0;
}

} // namespace janus
