#include "paxos_worker.h"
#include "service.h"
#include "chrono"

namespace janus {


moodycamel::ConcurrentQueue<shared_ptr<Coordinator>> PaxosWorker::coo_queue;
std::queue<shared_ptr<Coordinator>> PaxosWorker::coo_queue_nc;

static int volatile xx =
    MarshallDeputy::RegInitializer(MarshallDeputy::CONTAINER_CMD,
                                   []() -> Marshallable* {
                                     return new LogEntry;
                                   });
static int volatile xxx =
      MarshallDeputy::RegInitializer(MarshallDeputy::CMD_BLK_PXS,
                                     []() -> Marshallable* {
                                       return new BulkPaxosCmd;
                                     });

static int shared_ptr_apprch = 1;
Marshal& LogEntry::ToMarshal(Marshal& m) const {
  m << length;
  if(shared_ptr_apprch){
	  m << std::string(operation_test.get(), length);
  } else{
	  m << log_entry;
  }
  return m;
};

Marshal& LogEntry::FromMarshal(Marshal& m) {
  m >> length;
  if(shared_ptr_apprch){
	  std::string str;
	  m >> str;
	  operation_test = shared_ptr<char>(new char[length]);
	  memcpy(operation_test.get(), str.c_str(), length);
  }else{
 	 m >> log_entry;
  }
  return m;
};

void PaxosWorker::SetupBase() {
  auto config = Config::GetConfig();
  rep_frame_ = Frame::GetFrame(config->replica_proto_);
  rep_frame_->site_info_ = site_info_;
  rep_sched_ = rep_frame_->CreateScheduler();
  rep_sched_->loc_id_ = site_info_->locale_id;
  rep_sched_->partition_id_ = site_info_->partition_id_;
  this->tot_num = config->get_tot_req();
}

void PaxosWorker::Next(Marshallable& cmd) {
  //return;
  if (cmd.kind_ == MarshallDeputy::CONTAINER_CMD) {
    if (this->callback_ != nullptr) {
      //auto& sp_log_entry = dynamic_cast<LogEntry&>(cmd);
      if(!shared_ptr_apprch){
	      //callback_(sp_log_entry.log_entry.c_str(), sp_log_entry.length);
      }else{
	      //callback_(sp_log_entry.operation_test.get(), sp_log_entry.length);
      }
    } else {
      verify(0);
    }
  } else {
    verify(0);
  }
  
  //if (n_current > n_tot) {
    n_current++;
    if(site_info_->locale_id == 0){
	    //if((int)n_current%10000 == 0)Log_info("current commits are progressing, current %d", (int)n_current);
    }
    if (n_current >= n_tot) {
      //Log_info("Current pair id %d loc id %d n_current and n_tot and accept size is %d %d", site_info_->partition_id_, site_info_->locale_id, (int)n_current, (int)n_tot);
      finish_cond.bcast();
    }
  //}
}

void PaxosWorker::SetupService() {
  std::string bind_addr = site_info_->GetBindAddress();
  int n_io_threads = 1;
  svr_poll_mgr_ = new rrr::PollMgr(n_io_threads);
  if (rep_frame_ != nullptr) {
    services_ = rep_frame_->CreateRpcServices(site_info_->id,
                                              rep_sched_,
                                              svr_poll_mgr_,
                                              scsi_);
  }
  uint32_t num_threads = 1;
  thread_pool_g = new base::ThreadPool(num_threads);

  // init rrr::Server
  rpc_server_ = new rrr::Server(svr_poll_mgr_, thread_pool_g);

  // reg services
  for (auto service : services_) {
    rpc_server_->reg(service);
  }

  // start rpc server
  Log_debug("starting server at %s", bind_addr.c_str());
  int ret = rpc_server_->start(bind_addr.c_str());
  if (ret != 0) {
    Log_fatal("server launch failed.");
  }

  Log_info("Server %s ready at %s",
           site_info_->name.c_str(),
           bind_addr.c_str());
}

void PaxosWorker::SetupCommo() {
  if (rep_frame_) {
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_mgr_);
    if (rep_commo_) {
      rep_commo_->loc_id_ = site_info_->locale_id;
    }
    rep_sched_->commo_ = rep_commo_;
  }
  if (IsLeader(site_info_->partition_id_))
    submit_pool = new SubmitPool();
}

void PaxosWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  scsi_ = new ServerControlServiceImpl(timeout);
  int n_io_threads = 1;
  svr_hb_poll_mgr_g = new rrr::PollMgr(n_io_threads);
  hb_thread_pool_g = new rrr::ThreadPool(1);
  hb_rpc_server_ = new rrr::Server(svr_hb_poll_mgr_g, hb_thread_pool_g);
  hb_rpc_server_->reg(scsi_);

  auto port = site_info_->port + CtrlPortDelta;
  std::string addr_port = std::string("0.0.0.0:") +
                          std::to_string(port);
  hb_rpc_server_->start(addr_port.c_str());
  if (hb_rpc_server_ != nullptr) {
    // Log_info("notify ready to control script for %s", bind_addr.c_str());
    scsi_->set_ready();
  }
  Log_info("heartbeat setup for %s on %s",
           site_info_->name.c_str(), addr_port.c_str());
}

void PaxosWorker::WaitForShutdown() {
  if (submit_pool != nullptr) {
    delete submit_pool;
    submit_pool = nullptr;
  }
  if (hb_rpc_server_ != nullptr) {
//    scsi_->server_heart_beat();
    scsi_->wait_for_shutdown();
    delete hb_rpc_server_;
    delete scsi_;
    svr_hb_poll_mgr_g->release();
    hb_thread_pool_g->release();

    for (auto service : services_) {
      if (DepTranServiceImpl* s = dynamic_cast<DepTranServiceImpl*>(service)) {
        auto& recorder = s->recorder_;
        if (recorder) {
          auto n_flush_avg_ = recorder->stat_cnt_.peek().avg_;
          auto sz_flush_avg_ = recorder->stat_sz_.peek().avg_;
          Log::info("Log to disk, average log per flush: %lld,"
                    " average size per flush: %lld",
                    n_flush_avg_, sz_flush_avg_);
        }
      }
    }
  }
}

void PaxosWorker::ShutDown() {
  Log_info("site %s deleting services, num: %d %d %d %d", site_info_->name.c_str(), services_.size(), 0, (int)n_current, (int)n_tot);
  verify(rpc_server_ != nullptr);
  delete rpc_server_;
  rpc_server_ = nullptr;
  for (auto service : services_) {
    delete service;
  }
  thread_pool_g->release();
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
    //Log_debug("Obtaining bulk submit of size %d through coro", (int)entries.size());
    //Log_debug("Current n_submit and n_current is %d %d", (int)n_submit, (int)n_current);
    auto sp_cmd = make_shared<BulkPaxosCmd>();
    Log_debug("Current reference count before submit : %d", sp_cmd.use_count());
    for(auto coo : entries){
        auto mpc = dynamic_pointer_cast<CoordinatorMultiPaxos>(coo);
        sp_cmd->slots.push_back(mpc.get()->slot_id_);
        sp_cmd->ballots.push_back(mpc.get()->curr_ballot_);
        verify(mpc->cmd_ != nullptr);
        //auto x = dynamic_pointer_cast<LogEntry>(mpc->cmd_);
        //read_log(x.get()->operation_test.get(), x.get()->length, "BulkSubmit");
        MarshallDeputy* md =  new MarshallDeputy(mpc.get()->cmd_);
        sp_cmd->cmds.push_back(shared_ptr<MarshallDeputy>(md));
        //auto x = dynamic_pointer_cast<LogEntry>(md->sp_data_);
       // read_log(x.get()->operation_test.get(), x.get()->length, "BulkSubmit");
    }
    auto sp_m = dynamic_pointer_cast<Marshallable>(sp_cmd);
    //return;
    //return;
    //n_current += (int)entries.size();
    //n_submit -= (int)entries.size();
    //Log_info("Current pair id %d n_current and n_tot is %d %d", site_info_->partition_id_, (int)n_current, (int)n_tot);
    _BulkSubmit(sp_m);
    Log_debug("Current reference count after submit: %d", sp_cmd.use_count());
}

inline void PaxosWorker::_BulkSubmit(shared_ptr<Marshallable> sp_m){
    auto coord = shared_ptr<Coordinator>(rep_frame_->CreateBulkCoordinator(Config::GetConfig(), 0));
    coord.get()->par_id_ = site_info_->partition_id_;
    coord.get()->loc_id_ = site_info_->locale_id;
    coord.get()->BulkSubmit(sp_m);
}

void PaxosWorker::AddAccept(shared_ptr<Coordinator> coord) {
  //Log_info("current batch cnt %d", cnt);
  nc_submit_l_.lock();
  PaxosWorker::coo_queue_nc.push(coord);
  nc_submit_l_.unlock();
}

int PaxosWorker::deq_from_coo(vector<shared_ptr<Coordinator>>& current){
  int qcnt = PaxosWorker::coo_queue.try_dequeue_bulk(&current[0], cnt);
  return qcnt;
}

void* PaxosWorker::StartReadAccept(void* arg){
  PaxosWorker* pw = (PaxosWorker*)arg;
  //std::vector<shared_ptr<Coordinator>> current(pw->cnt, nullptr);
  while (!pw->stop_flag) {
    std::vector<shared_ptr<Coordinator>> current(pw->cnt, nullptr);
    int cnt = pw->deq_from_coo(current);
    if(cnt <= 0)continue;
    std::vector<shared_ptr<Coordinator>> sub(current.begin(), current.begin() + cnt);
    //Log_debug("Pushing coordinators for bulk accept coordinators here having size %d %d %d %d", (int)sub.size(), pw->n_current.load(), pw->n_tot.load(),pw->site_info_->locale_id);
    auto sp_job = std::make_shared<OneTimeJob>([&pw, sub]() {
      pw->BulkSubmit(sub);
    });
    pw->GetPollMgr()->add(sp_job);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pthread_exit(nullptr);
  return nullptr;
}

void PaxosWorker::AddAcceptNc(shared_ptr<Coordinator> coord) {
  nc_submit_l_.lock();
  PaxosWorker::coo_queue_nc.push(coord);
  nc_submit_l_.unlock();
}

void* PaxosWorker::StartReadAcceptNc(void* arg){
  PaxosWorker* pw = (PaxosWorker*)arg;
  while (!pw->stop_flag) {
    std::vector<shared_ptr<Coordinator>> current;
    int cur_req = pw->cnt;
    nc_submit_l_.lock();
    while(!PaxosWorker::coo_queue_nc.empty() && cur_req > 0){
      auto x = PaxosWorker::coo_queue_nc.top();
      PaxosWorker::coo_queue_nc.pop()
      current.push_back(x);
      cur_req--;
    }
    nc_submit_l_.unlock();
    int cnt = current.size();
    if(cnt == 0)continue;
    //Log_debug("Pushing coordinators for bulk accept coordinators here having size %d %d %d %d", (int)sub.size(), pw->n_current.load(), pw->n_tot.load(),pw->site_info_->locale_id);
    auto sp_job = std::make_shared<OneTimeJob>([&pw, current]() {
      pw->BulkSubmit(current);
    });
    pw->GetPollMgr()->add(sp_job);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pthread_exit(nullptr);
  return nullptr;
}

void PaxosWorker::WaitForSubmit() {
  while (n_current < n_tot) {
    finish_mutex.lock();
    //Log_info("wait for task, amount: %d", (int)n_tot-(int)n_current);
    finish_cond.wait(finish_mutex);
    finish_mutex.unlock();
  }
  Log_debug("finish task.");
}

void PaxosWorker::InitQueueRead(){
  if(IsLeader(site_info_->partition_id_)){
    stop_flag = false;
    Pthread_create(&bulkops_th_, nullptr, PaxosWorker::StartReadAcceptNc, this);
    pthread_detach(bulkops_th_);
  }
}

void PaxosWorker::AddReplayEntry(Marshallable& entry){
  Marshallable *p = &entry;
  replay_queue.enqueue(p);
}

void* PaxosWorker::StartReplayRead(void* arg){
  PaxosWorker* pw = (PaxosWorker*)arg;
  while(!pw->stop_replay_flag){
    Marshallable* p;
    auto res = pw->replay_queue.try_dequeue(p);
    if(!res)continue;
    pw->Next(*p);
  }
}

PaxosWorker::PaxosWorker() {
  stop_replay_flag = true;
  Pthread_create(&replay_th_, nullptr, PaxosWorker::StartReplayRead, this);
  pthread_detach(replay_th_);
}

PaxosWorker::~PaxosWorker() {
  Log_debug("Ending worker with n_tot %d and n_current %d", (int)n_tot, (int)n_current);
  stop_flag = true;
  stop_replay_flag = true;
}


void PaxosWorker::Submit(const char* log_entry, int length, uint32_t par_id) {
  //Log_info("Entering PaxosWorker::Submit  here\n");
  if (!IsLeader(par_id)) return;
  //read_log(log_entry, length, "silo");
  auto sp_cmd = make_shared<LogEntry>();
  if(!shared_ptr_apprch){
	  sp_cmd->log_entry = string(log_entry,length);
  }else{
    //sp_cmd->operation_ = (char*)string(log_entry,length).c_str();
    // sp_cmd->operation_test = shared_ptr<char>((char*)string(log_entry,length).c_str());
	  sp_cmd->operation_test = shared_ptr<char>((char*)malloc(length));
    memcpy(sp_cmd->operation_test.get(), log_entry, length);
  }
  //Log_info("PaxosWorker::Submit Log=%s",operation_);
  sp_cmd->length = length;
  auto sp_m = dynamic_pointer_cast<Marshallable>(sp_cmd);
  _Submit(sp_m);
  free((char*)log_entry);
}

inline void PaxosWorker::_Submit(shared_ptr<Marshallable> sp_m) {
  mtx_worker_submit.lock();	
  // finish_mutex.lock();
  //n_current++;
  //n_submit--;
  //n_tot++;
  // finish_mutex.unlock();
  static cooid_t cid{1};
  static id_t id{1};
  verify(rep_frame_ != nullptr);
  auto coord = rep_frame_->CreateCoordinator(cid++,
                                                     Config::GetConfig(),
                                                     0,
                                                     nullptr,
                                                     id++,
                                                     nullptr);
  mtx_worker_submit.unlock();
  coord->par_id_ = site_info_->partition_id_;
  coord->loc_id_ = site_info_->locale_id;
  //created_coordinators_.push_back(coord);
  //coord->cmd_ = sp_m;
  coord->assignCmd(sp_m);
  if(stop_flag != true) {
    auto sp_coo = shared_ptr<Coordinator>(coord);
    //created_coordinators_shrd.push_back(sp_coo);
    AddAcceptNc(sp_coo);
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
                                         std::placeholders::_1));
}

} // namespace janus
