#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "paxos/server.h"
#include "paxos/commo.h"
#include "rrr/misc/serializable.hpp"

import std;

namespace janus {
// Paxos worker thread
vector<shared_ptr<PaxosWorker>> pxs_workers_g = {};
// Learner worker thread on the server side
vector<shared_ptr<PaxosWorker>> ler_workers_g = {};

moodycamel::ConcurrentQueue<shared_ptr<Coordinator>> PaxosWorker::coo_queue;
// removed `coo_queue_nc` static definition
// — never read or written outside commented-out code; field also gone.

shared_ptr<ElectionState> es_pw = ElectionState::instance();

// Registry keys come from each payload's explicit MakoCommands membership.
static int volatile xx  = rrr::SerializableRegistry::reg<LogEntry>(LogEntry::static_kind());
static int volatile xxx = rrr::SerializableRegistry::reg<BulkPaxosCmd>(BulkPaxosCmd::static_kind());
static int volatile x4  = rrr::SerializableRegistry::reg<BulkPrepareLog>(BulkPrepareLog::static_kind());
static int volatile x5  = rrr::SerializableRegistry::reg<HeartBeatLog>(HeartBeatLog::static_kind());
static int volatile x6  = rrr::SerializableRegistry::reg<SyncLogRequest>(SyncLogRequest::static_kind());
static int volatile x7  = rrr::SerializableRegistry::reg<SyncLogResponse>(SyncLogResponse::static_kind());
static int volatile x8  = rrr::SerializableRegistry::reg<SyncNoOpRequest>(SyncNoOpRequest::static_kind());
static int volatile x9  = rrr::SerializableRegistry::reg<PaxosPrepCmd>(PaxosPrepCmd::static_kind());

static int shared_ptr_apprch = 1;

// LogEntry::save/load. Wire format
// byte-for-byte preserved from the legacy to_marshal/from_marshal
// pair. Encode: int length, then the operation_test bytes (as a
// length-prefixed std::string) when present, else log_entry.
// Decode: int length, then std::string into log_entry (mirrors the
// effective branch of the legacy from_marshal — the
// `false && shared_ptr_apprch` arm was unreachable).
void LogEntry::save(BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(length, ar);
  if (shared_ptr_apprch) {
    if (operation_test.get()) {
      rrr::Serialize_::serialize(std::string(operation_test.get(), length), ar);
    } else {
      rrr::Serialize_::serialize(log_entry, ar);
    }
  } else {
    rrr::Serialize_::serialize(log_entry, ar);
  }
}

void LogEntry::load(BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(length, ar);
  rrr::Deserialize_::deserialize(log_entry, ar);
}

void PaxosWorker::SetupBase() {
  auto config = Config::GetConfig();
  rep_frame_ = Frame::GetFrame(config->replica_proto_);
  rep_frame_->site_info_ = site_info_;
  rep_sched_ = rep_frame_->CreateScheduler();
  rep_sched_->loc_id_ = site_info_->locale_id;
  rep_sched_->partition_id_ = site_info_->partition_id_;
  this->tot_num = config->get_tot_req();
}


int PaxosWorker::Next(int slot_id, janus::Command md) {
  int status=-1;
  // if (site_info_->proc_name.compare("learner")==0){
  //   Log_info("receive a slot_id:{}",slot_id);
  // }
  const auto sp_log_entry = marshallable_cast<LogEntry>(md);
  verify(sp_log_entry.is_some());
  int len = sp_log_entry.unwrap()->length;

  //Log_info("apply a log, par_id:{}, epoch:{}, slot_id:{}, len:{},",site_info_->partition_id_, cur_epoch, slot_id, len);
  if (md.kind_ == LogEntry::static_kind()) {
    if (this->callback_par_id_return_ != nullptr) {
      // forward the cmd to the learner
      // we use p1 to forward requests to save leader's bandwidth
      // it's better to start p1 at the end
      if ((site_info_->proc_name.compare("p1")==0)) {
        // Forward commits to learner
        // Note: rep_commo_ should be initialized before workload starts
        // If workload starts before all sites connect, this will be skipped
        if (rep_commo_ != nullptr) {
          ((MultiPaxosCommo*)rep_commo_)->ForwardToLearner(site_info_->partition_id_,
                                         slot_id,
                                         cur_epoch,  // Use PaxosWorker's cur_epoch instead of coordinator
                                         md,
                                         [](uint64_t slot, ballot_t ballot) {
                                           //Log_info("received a ack from the learner, slot: {}, ballot: {}", slot, ballot);
                                         });
        }
      }

      const auto sp_log_entry = marshallable_cast<LogEntry>(md);
      verify(sp_log_entry.is_some());
      int len = sp_log_entry.unwrap()->length;
      if(sp_log_entry.unwrap()->length == 0){
	      Log_info("Recieved a zero length log");
      }
      //Log_info("Paxos commit a log, par_id:{}, len: {}, epoch:{}, slot_id:{}",site_info_->partition_id_, len, cur_epoch, slot_id);
      //Log_info("in Next, partition_id: {}, id: {}, proc_name: {}, role: {}, slot: {}", site_info_->partition_id_, site_info_->id, site_info_->proc_name.c_str(), site_info_->role, slot);                                 
      if (len > 0) {
         const char *log = sp_log_entry.unwrap()->log_entry.c_str() ;
         
         // Single timestamp system: get encoded value (timestamp * 10 + status)
         int encoded_value = callback_par_id_return_(log, 
                                                    len, 
                                                    site_info_->partition_id_,
                                                    slot_id,
                                                    un_replay_logs_);
         status = encoded_value % 10;  // Extract status from last digit
         uint32_t timestamp = encoded_value / 10;  // Extract timestamp
         //Log_info("XXXXX: partition_id: {}, id: {}, proc_name: {}, role: {}", site_info_->partition_id_, site_info_->id, site_info_->proc_name.c_str(), site_info_->role);                                 
         //Log_info("received a message: {}, status: {}, timestamp: {}", sp_log_entry->length, status, timestamp);
         // status: 1 => init, 2 => ending of paxos group, 3 => can't pass the safety check, 4 => complete replay
         //Log_info("par_id: {}, append a log into un_replay_logs, size: {}, status: {}, first[0]: {}, received: {}", 
         //         site_info_->partition_id_, un_replay_logs_.size(), status, latest_commit_id_v[0], sp_log_entry->length);
         if (status == janus::PaxosStatus::STATUS_SAFETY_FAIL) {
             char *dest = (char *)malloc(len) ;
             memcpy(dest, log, len) ;
             un_replay_logs_.push(std::make_tuple(timestamp, slot_id, status, len, (const char*)dest)) ;
             //un_replay_logs_.push(std::make_tuple(timestamp, slot_id, status, len, (const char*)log)) ;
         } else if (status == janus::PaxosStatus::STATUS_INIT) {
             std::cout << "this should never happen!!!" << std::endl;
         } else if (status == janus::PaxosStatus::STATUS_NOOPS) {
            Log_info("update the no-ops, par_id:{}, slot_id:{}",site_info_->partition_id_, slot_id);
            noops_received=true;
         }
      } else {
        // the ending signal
        const char *log = sp_log_entry.unwrap()->log_entry.c_str() ;
        int ending_status = callback_par_id_return_(log, len, site_info_->partition_id_, slot_id, un_replay_logs_) ;
      }
    } else {
      verify(0);
    }
  } else {
    verify(0);
  }

  if (n_current >= n_tot) {
    //Log_info("Current pair id {} loc id {} n_current and n_tot and accept size is {} {}", site_info_->partition_id_, site_info_->locale_id, (int)n_current, (int)n_tot);
    finish_cond.notify_all();
  }
  return status;
}

void PaxosWorker::SetupService() {
  std::string bind_addr = site_info_->GetBindAddress();
  svr_poll_thread_worker_ = rusty::Some(PollThread::create());

  // init rrr::Server first (before registering services)
  rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(svr_poll_thread_worker_.as_ref().unwrap().clone())));

  // Create and register services (ownership transferred to rpc_server_)
  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   svr_poll_thread_worker_.as_ref().unwrap());
    Log_info("[service]loc_id: {}, name: {}, proc: {}, id: {}",
      site_info_->locale_id, site_info_->name.c_str(), site_info_->proc_name.c_str(), site_info_->id);
    for (auto& svc : services) {
      rpc_server_->reg_service_proxy(std::move(svc));
    }
  }

  // start rpc server
  Log_debug("starting server at {}", bind_addr.c_str());
  std::cout << "starting server at " << bind_addr.c_str() << std::endl;
  int ret = rpc_server_->start(reinterpret_cast<const int8_t*>(bind_addr.c_str()));
  if (ret != 0) {
    Log_fatal("server launch failed.");
    std::cout << "server launch failed.\n";
  }

  Log_info("Server {} ready at {}",
           site_info_->name.c_str(),
           bind_addr.c_str());
}

void PaxosWorker::SetupCommo() {
  if (rep_frame_) {
    // Use clone() to preserve svr_poll_thread_worker_ for later use by GetPollThread()
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_.clone());
    if (rep_commo_) {
      rep_commo_->loc_id_ = site_info_->locale_id;
    }
    rep_sched_->commo_ = rep_commo_;
  }
  // removed commented-out
  // `submit_pool = new SubmitPool();` — `SubmitPool` class deleted.
}

void PaxosWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  svr_hb_poll_thread_worker_g = rusty::Some(PollThread::create());
  hb_rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(svr_hb_poll_thread_worker_g.as_ref().unwrap().clone())));

  // Create shared status and pass clone to service
  server_status_ = rusty::Some(rusty::Arc<ServerStatus>::make());
  hb_rpc_server_->reg_service_typed(rusty::make_box<ServerControlServiceImpl>(server_status_.as_ref().unwrap().clone(), timeout));

  auto port = site_info_->port + CtrlPortDelta;
  std::string addr_port = std::string("0.0.0.0:") +
                          std::to_string(port);
  hb_rpc_server_->start(reinterpret_cast<const int8_t*>(addr_port.c_str()));
  if (hb_rpc_server_ != nullptr) {
    // Log_info("notify ready to control script for {}", bind_addr.c_str());
    server_status_.as_ref().unwrap()->set_ready();
  }
  Log_info("heartbeat setup for {} on {}",
           site_info_->name.c_str(), addr_port.c_str());
}

void PaxosWorker::WaitForShutdown() {
  // removed `if (submit_pool != nullptr)
  // { delete submit_pool; submit_pool = nullptr; }` — field always
  // nullptr (never assigned non-null), and the `SubmitPool` class
  // is gone.
  if (hb_rpc_server_ != nullptr) {
//    scsi_->server_heart_beat();
    hb_rpc_server_->wait_for_shutdown();
    delete hb_rpc_server_;  // Server destructor cleans up owned scsi_
    // svr_hb_poll_thread_worker_g automatically released by shared_ptr
    // Arc auto-releases on destruction

    // removed `for_each_service(...) { if
    // (auto* s = dynamic_cast<ClassicServiceImpl*>(...)) { auto&
    // recorder = s->recorder_; if (recorder) { Log_info(...) } } }`
    // block — `Service::recorder_` field is always nullptr (now
    // gone); the `if (recorder)` branch was unreachable.
  }
}

void PaxosWorker::ShutDown() {
  Log_info("site {} shutting down, n_current: {}, n_tot: {}", site_info_->name.c_str(), (int)n_current, (int)n_tot);
  verify(rpc_server_ != nullptr);
  // Services are now owned by rpc_server_ and will be deleted with it
  delete rpc_server_;
  rpc_server_ = nullptr;
  // Arc auto-releases on destruction
  for (auto c : created_coordinators_) {
    delete c;
  }
  if (rep_sched_ != nullptr) {
    delete rep_sched_;
  }
}

void PaxosWorker::IncSubmit(){	
	n_tot++;
}

void PaxosWorker::BulkSubmit(const vector<shared_ptr<Coordinator>>& entries){
    // Fill-then-wrap: build the batch locally, wrap once complete.
    BulkPaxosCmd bulk_cmd;
    election_state_lock.lock();
    ballot_t send_epoch = this->cur_epoch;
    election_state_lock.unlock();
    bulk_cmd.leader_id = es_pw->machine_id;
    for(auto coo : entries){
        auto mpc = dynamic_pointer_cast<CoordinatorMultiPaxos>(coo);
        bulk_cmd.slots.push_back(mpc.get()->slot_id_);
        bulk_cmd.ballots.push_back(send_epoch);
        // CoordinatorMultiPaxos::cmd_ is now Command;
        // null check via has_value, copy via Command's copy ctor.
        verify(mpc->cmd_.has_value());
        bulk_cmd.cmds.push_back(rusty::Arc<janus::Command>::make(mpc.get()->cmd_));
    }
    _BulkSubmit(rusty::Arc<BulkPaxosCmd>::make(std::move(bulk_cmd)), entries.size());
}

inline void PaxosWorker::_BulkSubmit(const janus::Command& sp_m, int cnt = 0){
    auto coord = shared_ptr<Coordinator>(rep_frame_->CreateBulkCoordinator(Config::GetConfig(), 0));
    coord.get()->par_id_ = site_info_->partition_id_;
    coord.get()->loc_id_ = site_info_->locale_id;

    coord.get()->BulkSubmit(sp_m, [this, cnt]() {
      this->n_current += cnt;
      if(this->n_current >= this->n_tot)this->finish_cond.notify_all();
    });
}

// removed `PaxosWorker::SendBulkPrepare` and
// `PaxosWorker::SendHeartBeat` — both became dead in Phase 4e-24 when
// `send_bulk_prep` and `electionMonitor` (their only callers) went
// away.  The `BroadcastBulkPrepare` / `BroadcastHeartBeat` commo
// methods + `MultiPaxosService::Heartbeat` / `BulkPrepare` service
// handlers are also dead now but left for a follow-up sweep that
// also clears the rcc_rpc.proto definitions.

int PaxosWorker::SendSyncLog(shared_ptr<SyncLogRequest> sync_log_req){
  ballot_t received_epoch = -1;
  auto coord = rep_frame_->CreateBulkCoordinator(Config::GetConfig(), 0);
  coord->par_id_ = site_info_->partition_id_;
  coord->loc_id_ = site_info_->locale_id;
  bool done = false;
  auto es_pww = es_pw;
  vector<rusty::Arc<SyncLogResponse>> responses;
  // @unsafe { boundary copy: shared_ptr<SyncLogRequest> param -> Arc envelope }
  auto sp_quorum = coord->commo_->BroadcastSyncLog(site_info_->partition_id_,
                                                   rusty::Arc<SyncLogRequest>::make(*sync_log_req),
                                                   [&received_epoch, &done, es_pww, &responses](shared_ptr<janus::Command> md,
                                                                                    ballot_t ballot,
                                                                                    int resp_type) {
    if(!resp_type)
      es_pww->step_down(ballot);
    else{
      if(!done){
        rusty::Option<rusty::Arc<SyncLogResponse>> x{rusty::None};
        if (md != nullptr) {
          x = marshallable_cast<SyncLogResponse>(*md);
        }
        // last use — unwrap() intentionally moves the Arc out.
        responses.emplace_back(x.unwrap());
      } else{
        return;
      }
    }
  });
  sp_quorum->wait();
  done = true;
  if (sp_quorum->yes()) {
    map<pair<int,slotid_t>, rusty::Arc<janus::Command>> commited_slots;
    for(int i = 0; i < responses.size(); i++){
      for(int j = 0; j < responses[i]->sync_data.size(); j++){
        const auto bp_cmd =
            marshallable_cast<BulkPaxosCmd>(*responses[i]->sync_data[j]);
        for(int k = 0; k < bp_cmd.unwrap()->slots.size(); k++){
          commited_slots.insert_or_assign(
              make_pair(j, bp_cmd.unwrap()->slots[k]),
              bp_cmd.unwrap()->cmds[k].clone());
        }
      }
    }
    Log_info("Responses size is {}", responses.size());
    for(int i = 0; i < responses.size(); i++){
      for(int j = 0; j < responses[i]->missing_slots.size(); j++){
        auto ps_j = dynamic_cast<PaxosServer*>(pxs_workers_g[j]->rep_sched_);
        for(int k = 0; k < responses[i]->missing_slots[j].size(); k++){
          auto inst = ps_j->GetInstance(responses[i]->missing_slots[j][k]);
          // PaxosData::committed_cmd_ is Command;
          // direct copy via Command's copy ctor.
          if(inst->committed_cmd_.has_value()){
	    //Log_info("The slots are for partition {} slot {}", j, responses[i]->missing_slots[j][k]);
            commited_slots.insert_or_assign(make_pair(j, responses[i]->missing_slots[j][k]), rusty::Arc<janus::Command>::make(inst->committed_cmd_));
          }
        }
      }
    }

    // Fill-then-wrap: build the batches locally, wrap each once
    // complete at the BroadcastSyncCommit call below.
    vector<BulkPaxosCmd> sync_cmds;
    sync_cmds.reserve(pxs_workers_g.size());
    for(int i = 0; i < pxs_workers_g.size(); i++){
      sync_cmds.emplace_back();
      sync_cmds.back().leader_id = es_pw->machine_id;
    }
    for(auto const& x : commited_slots){
      sync_cmds[x.first.first].slots.push_back(x.first.second);
      sync_cmds[x.first.first].cmds.push_back(x.second.clone());
      sync_cmds[x.first.first].ballots.push_back(sync_log_req->epoch);
    }
    vector<shared_ptr<PaxosAcceptQuorumEvent>> events;
    for(int i = 0; i < pxs_workers_g.size(); i++){
      if(sync_cmds[i].ballots.size() == 0)
        continue;
      //Log_info("Should receive some uncommitted slots here {}", i);
      //for(int kk = 0; kk < sync_cmds[i]->slots.size(); kk++)
      //      std::cout << sync_cmds[i]->slots[kk] << " ";
      //std::cout << std::endl;
      auto pw = pxs_workers_g[i];
      auto sp_quorum = pw->rep_commo_->BroadcastSyncCommit(i,
                                                           rusty::Arc<BulkPaxosCmd>::make(std::move(sync_cmds[i])),
                                                           [es_pww](ballot_t ballot, int valid){
          if(!valid){
            es_pww->step_down(ballot);
          }
      });
      events.push_back(sp_quorum);
      //sp_quorum->wait();
    }
    for(int i = 0; i < events.size(); i++){
      events[i]->wait();
    }
    return -1;
  }
  return received_epoch;
}

// removed `PaxosWorker::SendSyncNoOpLog` —
// became dead in Phase 4e-24 when `send_no_ops_to_all_workers` (its
// only caller) went away.  The `BroadcastSyncNoOps` commo method and
// `MultiPaxosService::SyncNoOps` service handler / `OnSyncNoOps`
// server-side impl are also dead now but left for the same follow-up
// sweep as the other dead Broadcast* paths.

void PaxosWorker::AddAccept(shared_ptr<Coordinator> coord) {
  //Log_info("current batch cnt {}", cnt);
  PaxosWorker::coo_queue.enqueue(coord);
}

int PaxosWorker::deq_from_coo(vector<shared_ptr<Coordinator>>& current){
  int qcnt = PaxosWorker::coo_queue.try_dequeue_bulk(&current[0], cnt);
  return qcnt;
}


void* PaxosWorker::StartReadAccept(void* arg){
  PaxosWorker* pw = (PaxosWorker*)arg;
  //std::vector<shared_ptr<Coordinator>> current(pw->cnt, nullptr);
  int sent = 0;
  while (!pw->stop_flag) {
    std::vector<shared_ptr<Coordinator>> current(pw->cnt, nullptr);
    int cnt = pw->deq_from_coo(current);
    if(cnt <= 0)continue;
    std::vector<shared_ptr<Coordinator>> sub(current.begin(), current.begin() + cnt);
    //Log_debug("Pushing coordinators for bulk accept coordinators here having size {} {} {} {}", (int)sub.size(), pw->n_current.load(), pw->n_tot.load(),pw->site_info_->locale_id);
    auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([&pw, sub]() {
      pw->BulkSubmit(sub);
    }));
    auto arc_job_base = rusty::Arc<Job>(arc_job);
    pw->GetPollThread()->add(arc_job_base);
    sent += cnt;
    if(sent % 2 == 0)Log_info("Total submits {}", sent);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pthread_exit(nullptr);
  return nullptr;
}

// removed `AddAcceptNc` and
// `StartReadAcceptNc` — the NC-batching path was driven by a
// pthread launched from `InitQueueRead`'s commented-out
// `Pthread_create(&bulkops_th_, ..., StartReadAcceptNc, ...)` line.
// No surviving call site started this thread, so the producer
// (`AddAcceptNc`) and consumer (`StartReadAcceptNc`) were both dead.
// Companion `all_coords[]`, `bulk_writer`, `bulk_reader`, and
// `bulkops_th_` fields are also removed in this phase.

void PaxosWorker::submitJob(rusty::Arc<Job> arc_job){
	GetPollThread()->add(arc_job);
}

void PaxosWorker::WaitForNoops() {
  while(1){
    if(noops_received) break;
    sleep(0);
  }
}
void PaxosWorker::WaitForSubmit() {
  /*while(true){
	sleep(1);
        Log_info("wait for task, amount: {} - n_tot: {}, n_current: {}", (int)n_tot-(int)n_current, (int)n_tot, (int)n_current);
  }*/
  {
    std::unique_lock<std::mutex> lock(finish_mutex);
    while (n_current < n_tot) {
      Log_info("wait for task, amount: {} - n_tot: {}, n_current: {}", (int)n_tot-(int)n_current, (int)n_tot, (int)n_current);
      finish_cond.wait(lock);
    }
  }
  Log_debug("finish task.");
}

void PaxosWorker::InitQueueRead(){
  if(IsLeader(site_info_->partition_id_)){
    stop_flag = false;
    // removed commented-out
    // `Pthread_create(&bulkops_th_, ..., StartReadAcceptNc, this)` /
    // `pthread_detach(bulkops_th_)` — both target and field gone.
  }
}

// removed `AddReplayEntry` and
// `StartReplayRead` — `StartReplayRead` was launched from a
// commented-out `Pthread_create(&replay_th_, ..., StartReplayRead,
// this)` line in the `PaxosWorker()` constructor; no surviving call
// site started the thread.  The companion `replay_queue`,
// `stop_replay_flag`, and `replay_th_` fields are also removed in
// this phase.

PaxosWorker::PaxosWorker() {
  // removed `stop_replay_flag = true;` plus
  // the commented-out `Pthread_create(&replay_th_, ..., StartReplayRead,
  // this)` / `pthread_detach(replay_th_)` lines — the replay thread
  // never ran; the field went away with the dead method.
}

PaxosWorker::~PaxosWorker() {
  Log_info("Ending worker with n_tot {} and n_current {}", (int)n_tot, (int)n_current);
  stop_flag = true;
  // removed `stop_replay_flag = true;` —
  // the field went away with the dead `StartReplayRead`.

  // Shutdown PollThreads if we own them
  if (svr_poll_thread_worker_.is_some()) {
    svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
  }
}

void PaxosWorker::Submit(const char* log_entry, int length, uint32_t par_id) { // this is the starting point on the client side
  // Fill-then-wrap: build the entry locally, wrap once complete.
  LogEntry cmd;
  // Use std::string for payload to avoid mismatched allocation/deallocation
  cmd.log_entry = std::string(log_entry, length);
  cmd.length = length;
  _Submit(rusty::Arc<LogEntry>::make(std::move(cmd)));
}

inline void PaxosWorker::_Submit(const janus::Command& sp_m) {
  static cooid_t cid{1};
  static id_t id{1};
  verify(rep_frame_ != nullptr);
  auto coord = rep_frame_->CreateCoordinator(cid++,
                                             Config::GetConfig(),
                                             0,
                                             rusty::None,
                                             id++,
                                             nullptr);
  coord->par_id_ = site_info_->partition_id_;
  coord->loc_id_ = site_info_->locale_id;
  //marker:ansh slot_hint not being used anymore.
  slotid_t x = ((PaxosServer*)rep_sched_)->get_open_slot();
  coord->set_slot(x);
  coord->assignCmd(sp_m);
  if(stop_flag != true) {
    auto sp_coo = shared_ptr<Coordinator>(coord);
    vector<shared_ptr<Coordinator>> curr2;
    curr2.push_back(sp_coo);
    auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([this, curr2]() {
      this->BulkSubmit(curr2);
    }));
    auto arc_job_base = rusty::Arc<Job>(arc_job);
    submitJob(arc_job_base);
  } else{
    coord->Submit(sp_m);
  }
}

bool PaxosWorker::IsLeader(uint32_t par_id) {
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);
  return rep_frame_->site_info_->partition_id_ == par_id &&
         rep_frame_->site_info_->locale_id == 0;
}

bool PaxosWorker::IsPartition(uint32_t par_id) {
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);
  return rep_frame_->site_info_->partition_id_ == par_id;
}

void PaxosWorker::register_apply_callback(std::function<void(const char*, int)> cb) {
  this->callback_ = cb;
  verify(rep_sched_ != nullptr);
  rep_sched_->RegLearnerAction(std::bind(&PaxosWorker::Next,
                                         this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));
}

void PaxosWorker::register_apply_callback_par_id(std::function<void(const char *&, int, int)> cb) {
    this->callback_par_id_ = cb;
    verify(rep_sched_ != nullptr);
    rep_sched_->RegLearnerAction(std::bind(&PaxosWorker::Next,  // the commit entry
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
}

void PaxosWorker::register_apply_callback_par_id_return(std::function<int(const char *&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)> cb) {
    this->callback_par_id_return_ = cb;
    verify(rep_sched_ != nullptr);
    rep_sched_->RegLearnerAction(std::bind(&PaxosWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
}

} // namespace janus
