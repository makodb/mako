#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../scheduler.h"
#include "../mongodb_kv_table_handler.h"
#include "../mongodb_connection_thread_pool.h"
#include "../communicator.h"

namespace janus {

class MongodbServer : public TxLogServer {
  
#ifdef AWS
  const int mongodb_connection_ = 2000; // maximum connection maybe limited by ulimit, increase ulimit may solve connection limit problem. 2000 is designed for 0.66s latency 3000 clients open loop
#endif
#ifndef AWS
  const int mongodb_connection_ = 80; // seems maximum connextion between 95 * 5 and 100 * 5 at local
#endif
  shared_ptr<MongodbConnectionThreadPool> mongodb_;
  std::thread execution_thread;

  static void ExecutionHandler(MongodbServer* svr, shared_ptr<MongodbConnectionThreadPool>& db) {
    Log_info("Enter ExecutionHandler");
    while (true) { // This is not hot loop
      Log_info("db->MongodbFinishedEmpty() is %d", db->MongodbFinishedEmpty());
      while (!db->MongodbFinishedEmpty()) {
        shared_ptr<Marshallable> cmd = db->MongodbFinishedPop();
        if (cmd == nullptr) {
          Log_info("Exit ExecutionHandler for nullptr");
          break;
        }
        svr->RuleWitnessGC(cmd);
#ifdef MONGODB_DEBUG
        Log_info("%.2f After RuleWitnessGC <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
        svr->app_next_(0, cmd); // MongoDB doesn't use slot IDs, pass 0
#ifdef MONGODB_DEBUG
        Log_info("%.2f After app_next_ <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
      }
      Log_info("before Reactor::create_sp_event<TimeoutEvent>(5 * 1000)");
      // auto sp_e = Reactor::create_sp_event<TimeoutEvent>(5 * 1000);
      // sp_e->wait();
      auto sp_e = Reactor::create_sp_event<NeverEvent>();
      sp_e->wait(5);
      Log_info("After Reactor::create_sp_event<TimeoutEvent>(5 * 1000) wait");
    }
    Log_info("Exit ExecutionHandler");
  }

 public:

  void Setup() override {
    SimpleRWCommand::SetZeroTime();
    mongodb_ = make_shared<MongodbConnectionThreadPool>(loc_id_ == 0 ? mongodb_connection_ : 0);
    // Fiber::create_run([&]() { 
    //   ExecutionHandler(this, mongodb_); 
    // });
    // execution_thread = std::thread(ExecutionHandler, this, std::ref(mongodb_));
  }
  bool IsLeader() override {
    return loc_id_ == 0;
  }
  void Submit(const shared_ptr<Marshallable>& cmd) {
#ifdef MONGODB_DEBUG
    Log_info("%.2f Submit <%d, %d> loc_id %d", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second, loc_id_);
#endif
    WAN_WAIT
    verify(cmd->kind_ == TpcCommitCommand::static_kind());
    auto commit_cmd = marshallable_cast<TpcCommitCommand>(cmd);
    verify(commit_cmd != nullptr);
    auto vec_piece_data = marshallable_cast<VecPieceData>(commit_cmd->cmd_);
    verify(vec_piece_data != nullptr);
    shared_ptr<TxPieceData> cmd_content =
        *(vec_piece_data->sp_vec_piece_data_->begin());
    cmd_content->mongodb_finished = Reactor::create_sp_event<ThreadSafeIntEvent>();
#ifdef MONGODB_DEBUG
    Log_info("%.2f Before MongodbRequest <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
    mongodb_->MongodbRequest(cmd);
#ifdef MONGODB_DEBUG
    Log_info("%.2f Before cmd_content->mongodb_finished->wait() <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
//     cmd_content->mongodb_finished->set(1);
// #ifdef MONGODB_DEBUG
//     Log_info("%.2f xxxxx <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
// #endif
    cmd_content->mongodb_finished->wait();
#ifdef MONGODB_DEBUG
    Log_info("%.2f After cmd_content->mongodb_finished->wait() <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
    WAN_WAIT
#ifdef MONGODB_DEBUG
    Log_info("%.2f Before RuleWitnessGC <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
    RuleWitnessGC(cmd);
#ifdef MONGODB_DEBUG
    Log_info("%.2f After RuleWitnessGC <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
    app_next_(0, cmd); // MongoDB doesn't use slot IDs, pass 0
#ifdef MONGODB_DEBUG
    Log_info("%.2f After app_next_ <%d, %d>", SimpleRWCommand::GetMsTimeElaps(), SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second);
#endif
  }
  ~MongodbServer() {
    mongodb_->Close();
    // execution_thread.join();
  }
};
}
