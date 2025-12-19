#include "coordinator.h"
#include "../RW_command.h"
#include "../bench/rw/workload.h"
#include "server.h"

namespace janus {

MongodbServer* CoordinatorMongodb::Server() {
  return (MongodbServer*)(commo_->rep_sched_);
}

void CoordinatorMongodb::Submit(shared_ptr<Marshallable>& cmd,
                                const function<void()>& func,
                                const function<void()>& exe_callback) {
  Server()->Submit(cmd);
  commo()->BroadcastCommit(par_id_, cmd);
  func();
  exe_callback();
  // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  // if (parsed_cmd.type_ == RW_BENCHMARK_R_TXN || parsed_cmd.type_ == RW_BENCHMARK_R_TXN_0) {
  //   // ((MongodbServer *)(commo_->rep_sched_))->Read(parsed_cmd.key_);
  //   client_worker_->MongodbRead(parsed_cmd.key_);
  // } else if (parsed_cmd.type_ == RW_BENCHMARK_W_TXN || parsed_cmd.type_ == RW_BENCHMARK_W_TXN_0) {
  //   // ((MongodbServer *)(commo_->rep_sched_))->Write(parsed_cmd.key_, parsed_cmd.value_);
  //   client_worker_->MongodbWrite(parsed_cmd.key_, parsed_cmd.value_);
  // } else {
  //   verify(0);
  // }
  // commo_->rep_sched_->RuleWitnessGC(cmd);
  // commo_->rep_sched_->app_next_(*cmd);
}


}