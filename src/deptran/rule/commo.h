#pragma once

#include "deptran/communicator.h"

namespace janus
{

class CommunicatorRule : public Communicator {
public:
    unordered_map<int, uint32_t> n_pending_rpc_;
    const uint32_t max_pending_rpc_ = 200;
    SharedIntEvent dispatch_quota{};

    map<parid_t, vector<SiteProxyPair>> jetpack_leader_cache_ = {};

    CommunicatorRule(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>())
     :Communicator(poll_thread_worker) {
        dispatch_quota.value_ = 3 * max_pending_rpc_;
    }

    vector<SiteProxyPair> LeaderProxyForPartition(parid_t, int idx=-1) const;

    std::vector<int> LeadersForPartition(parid_t par_id) const;

    // SiteProxyPair FindSiteProxyPair(parid_t par_id, int replica_id) const;

    // std::vector<SiteProxyPair>
    // LeaderProxyForPartition(parid_t par_id) const;

    shared_ptr<RuleSpeculativeExecuteQuorumEvent>
    BroadcastRuleSpeculativeExecute(shared_ptr<vector<shared_ptr<SimpleCommand>>> vec_piece_data);

    void BroadcastDispatch(bool fastpath_broadcast_mode,
                         shared_ptr<vector<shared_ptr<SimpleCommand>>> vec_piece_data,
                         Coordinator *coo,
                         const std::function<void(int res, TxnOutput &)> &);
};
    
} // namespace janus
