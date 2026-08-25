#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "deptran/procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"

class SimpleCommand;
namespace janus {

class TxLogServer;
class FpgaRaftServer;
class FpgaRaftServiceImpl : public FpgaRaftService {
 public:
  FpgaRaftServer* sched_;
  FpgaRaftServiceImpl(TxLogServer* sched);

  // Defer handlers (preferred for async RPC completion).
  void Heartbeat(const uint64_t& leaderPrevLogIndex,
                 const DepId& dep_id,
                 uint64_t* followerPrevLogIndex,
                 srpc::DeferredReply defer);

  // removed `Forward(janus::Command, ...)`
  // declaration — paired with its typed-rpc override (also removed
  // below); the matching FpgaRaft::Forward RPC was dropped from
  // rcc_rpc.rpc and the receiver `FpgaRaftServer::OnForward` is also
  // gone.

  void Vote(const uint64_t& lst_log_idx,
            const ballot_t& lst_log_term,
            const parid_t& par_id,
            const ballot_t& cur_term,
            ballot_t* max_ballot,
            bool_t* vote_granted,
            srpc::DeferredReply defer);

  void Vote2FPGA(const uint64_t& lst_log_idx,
                 const ballot_t& lst_log_term,
                 const parid_t& par_id,
                 const ballot_t& cur_term,
                 ballot_t* max_ballot,
                 bool_t* vote_granted,
                 srpc::DeferredReply defer);

  void AppendEntries2(const uint64_t& slot,
                      const ballot_t& ballot,
                      const uint64_t& leaderCurrentTerm,
                      const uint64_t& leaderPrevLogIndex,
                      const uint64_t& leaderPrevLogTerm,
                      const uint64_t& leaderCommitIndex,
                      const DepId& dep_id,
                      const janus::Command& cmd,
                      uint64_t* followerAppendOK,
                      uint64_t* followerCurrentTerm,
                      uint64_t* followerLastLogIndex,
                      srpc::DeferredReply defer);

  void AppendEntries(const uint64_t& slot,
                     const ballot_t& ballot,
                     const uint64_t& leaderCurrentTerm,
                     const uint64_t& leaderPrevLogIndex,
                     const uint64_t& leaderPrevLogTerm,
                     const uint64_t& leaderCommitIndex,
                     const DepId& dep_id,
                     const janus::Command& cmd,
                     uint64_t* followerAppendOK,
                     uint64_t* followerCurrentTerm,
                     uint64_t* followerLastLogIndex,
                     srpc::DeferredReply defer);

  void Decide(const uint64_t& slot,
              const ballot_t& ballot,
              const DepId& dep_id,
              const janus::Command& cmd,
              srpc::DeferredReply defer);


  // BEGIN typed-rpc-decls (FpgaRaftServiceImpl)
  // Typed RPC interface overrides (new API).
  void Heartbeat(const FpgaRaftService::RpcHeartbeatRequest& req, FpgaRaftService::RpcHeartbeatResponse& resp, srpc::DeferredReply defer) override;
  // removed `Forward` typed-rpc override —
  // matching abstract base class virtual is gone (rcc_rpc.rpc updated).
  void Vote(const FpgaRaftService::RpcVoteRequest& req, FpgaRaftService::RpcVoteResponse& resp, srpc::DeferredReply defer) override;
  void Vote2FPGA(const FpgaRaftService::RpcVote2FPGARequest& req, FpgaRaftService::RpcVote2FPGAResponse& resp, srpc::DeferredReply defer) override;
  void AppendEntries2(const FpgaRaftService::RpcAppendEntries2Request& req, FpgaRaftService::RpcAppendEntries2Response& resp, srpc::DeferredReply defer) override;
  void AppendEntries(const FpgaRaftService::RpcAppendEntriesRequest& req, FpgaRaftService::RpcAppendEntriesResponse& resp, srpc::DeferredReply defer) override;
  void Decide(const FpgaRaftService::RpcDecideRequest& req, FpgaRaftService::RpcDecideResponse& resp, srpc::DeferredReply defer) override;
  // END typed-rpc-decls (FpgaRaftServiceImpl)
};

} // namespace janus
