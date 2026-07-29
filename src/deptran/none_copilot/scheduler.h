#pragma once
#include <rusty/arc.hpp>
#include <rusty/option.hpp>

#include "../none/scheduler.h"
#include "../classic/tx.h"
#include "../copilot/server.h"

namespace janus
{

class SchedulerNoneCopilot : public SchedulerNone {
	using SchedulerNone::SchedulerNone;

	bool in_waiting_ = false;
	vector<rusty::Arc<TpcCommitCommand> > batch_buffer_;

	uint64_t total_ = 0;
	uint64_t submit_ = 0;
  public:
	int OnCommit(cmdid_t cmd_id,
				struct DepId dep_id,
				int commit_or_abort) override;
};

} // namespace janus
