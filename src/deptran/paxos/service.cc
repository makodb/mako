
#include "service.h"
#include "server.h"
#include "../paxos_worker.h"

namespace janus {

MultiPaxosServiceImpl::MultiPaxosServiceImpl(TxLogServer *sched)
    : sched_((PaxosServer*)sched) {

}

void MultiPaxosServiceImpl::Forward(const MarshallDeputy& md_cmd,
                                    const uint64_t& dep_id,
                                    uint64_t* coro_id,
                                    rrr::DeferredReply* defer) {
  // NOTE: Original Mako leaves this empty - Mako uses ForwardToLearner instead
}

void MultiPaxosServiceImpl::Prepare(const uint64_t& slot,
                                    const ballot_t& ballot,
                                    ballot_t* max_ballot,
                                    uint64_t* coro_id,
                                    rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  sched_->OnPrepare(slot,
                    ballot,
                    max_ballot,
                    coro_id,
                    std::bind(&rrr::DeferredReply::reply, defer));
}

void MultiPaxosServiceImpl::Accept(const uint64_t& slot,
		                   const uint64_t& time,
                                   const ballot_t& ballot,
                                   const MarshallDeputy& md_cmd,
                                   ballot_t* max_ballot,
                                   uint64_t* coro_id,
                                   rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  auto start = chrono::system_clock::now();

  time_t tstart = chrono::system_clock::to_time_t(start);
  tm * date = localtime(&tstart);
  date->tm_hour = 0;
  date->tm_min = 0;
  date->tm_sec = 0;
  auto midn = chrono::system_clock::from_time_t(std::mktime(date));

  auto hours = chrono::duration_cast<chrono::hours>(start-midn);
  auto minutes = chrono::duration_cast<chrono::minutes>(start-midn);
  auto seconds = chrono::duration_cast<chrono::seconds>(start-midn);

  auto start_ = chrono::duration_cast<chrono::microseconds>(start-midn-hours-minutes).count();
  //Log_info("Duration of RPC is: %d", start_-time);

  auto coro = Coroutine::CreateRun([&] () {
    sched_->OnAccept(slot,
		     time,
                     ballot,
                     const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                     max_ballot,
                     coro_id,
                     std::bind(&rrr::DeferredReply::reply, defer));

  });

  auto end = chrono::system_clock::now();
  auto duration = chrono::duration_cast<chrono::microseconds>(end-start);
  //Log_info("Duration of Accept() at Follower's side is: %d", duration.count());
  //Log_info("coro id on service side: %d", coro->id);
}

void MultiPaxosServiceImpl::Decide(const uint64_t& slot,
                                   const ballot_t& ballot,
                                   const MarshallDeputy& md_cmd,
                                   rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  auto x = md_cmd.sp_data_;
  sched_->OnCommit(slot, ballot,x);
  defer->reply();
}


void MultiPaxosServiceImpl::BulkPrepare(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  Coroutine::CreateRun([&] () {
    //std::cout << "send a BulkPrepare\n";
    sched_->OnBulkPrepare(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                          ballot,
                          valid,
                          std::bind(&rrr::DeferredReply::reply, defer));
  });
}

//marker:ansh complete, basic skeleton, add rpc definition in rcc_rpc.rpc
void MultiPaxosServiceImpl::Heartbeat(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  Coroutine::CreateRun([&] () {
    sched_->OnHeartbeat(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                          ballot,
                          valid,
                          std::bind(&rrr::DeferredReply::reply, defer));
  });
}

void MultiPaxosServiceImpl::BulkPrepare2(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       MarshallDeputy* ret,
                                       rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  ret->SetMarshallable(std::make_shared<BulkPaxosCmd>());
  auto p = dynamic_pointer_cast<BulkPaxosCmd>(ret->sp_data_);
  //Log_info("The marshallable flag is %d", p->bypass_to_socket_);
  Coroutine::CreateRun([&] () {
    sched_->OnBulkPrepare2(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                          ballot,
                          valid,
                          p,
                          std::bind(&rrr::DeferredReply::reply, defer));
  });
}

void MultiPaxosServiceImpl::BulkAccept(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  Coroutine::CreateRun([&] () {
    sched_->OnBulkAccept(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                         ballot,
                         valid,
                        std::bind(&rrr::DeferredReply::reply, defer));
  });
}

void MultiPaxosServiceImpl::BulkDecide(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  // Log_info("BulkDecide RPC handler called");
  Coroutine::CreateRun([&] () {
    // Log_info("BulkDecide coroutine executing, calling OnBulkCommit");
    sched_->OnBulkCommit(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                         ballot,
                         valid,
                         std::bind(&rrr::DeferredReply::reply, defer));
    // Log_info("BulkDecide coroutine finished");
    //defer->reply();
  });
  // Log_info("BulkDecide RPC handler returning");
}

void MultiPaxosServiceImpl::SyncLog(const MarshallDeputy& md_cmd,
                                     i32* ballot,
                                     i32* valid,
                                     MarshallDeputy* ret,
                                     rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  ret->SetMarshallable(std::make_shared<SyncLogResponse>());
  auto response = dynamic_pointer_cast<SyncLogResponse>(ret->sp_data_);
  Coroutine::CreateRun([&] () {
    sched_->OnSyncLog(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                      ballot,
                      valid,
                      response,
                      std::bind(&rrr::DeferredReply::reply, defer));
  //  auto rpx = dynamic_pointer_cast<SyncLogResponse>(ret->sp_data_);
  //   auto xx = (int32_t)rpx->missing_slots.size();
  //   Log_info("received a OnSyncLog2,xxx: %d",xx);
  //   for(int i = 0; i < rpx->missing_slots.size(); i++){
  //      Log_info("yy2: %d", (int32_t)rpx->missing_slots[i].size());
  //      for(int j = 0; j < rpx->missing_slots[i].size(); j++){
  //         Log_info("yy2 a OnSyncLog2,xxx: %d",j);
  //      }
  //   }
    defer->reply();
  });

}

void MultiPaxosServiceImpl::SyncCommit(const MarshallDeputy& md_cmd,
                                     i32* ballot,
                                     i32* valid,
                                     rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  Coroutine::CreateRun([&] () {
    sched_->OnSyncCommit(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                         ballot,
                         valid,
                         std::bind(&rrr::DeferredReply::reply, defer));
    //defer->reply();
  });
}

void MultiPaxosServiceImpl::SyncNoOps(const MarshallDeputy& md_cmd,
                                      i32* ballot,
                                      i32* valid,
                                      rrr::DeferredReply* defer) {
  verify(sched_ != nullptr);
  Coroutine::CreateRun([&] () {
    sched_->OnSyncNoOps(const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                         ballot,
                         valid,
                         std::bind(&rrr::DeferredReply::reply, defer));
    //defer->reply();
  });
}

void MultiPaxosServiceImpl::ForwardToLearnerServer(const rrr::i32& par_id,
                                                   const uint64_t& slot, 
                                                   const ballot_t& ballot, /* slot and ballot from the leader */
                                                   const MarshallDeputy& cmd, 
                                                   uint64_t* ret_slot, ballot_t* ret_ballot, rrr::DeferredReply* defer) {
    verify(sched_ != nullptr);
    *ret_slot = slot;
    *ret_ballot = ballot;
    Coroutine::CreateRun([&] () {
      sched_->OnForwardToLearner(par_id, slot, ballot, const_cast<MarshallDeputy&>(cmd).sp_data_,
                               std::bind(&rrr::DeferredReply::reply, defer));
    });
}


} // namespace janus;
