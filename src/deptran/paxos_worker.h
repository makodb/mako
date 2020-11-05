#pragma once

#include "__dep__.h"
#include "coordinator.h"
#include "benchmark_control_rpc.h"
#include "frame.h"
#include "scheduler.h"
#include "communicator.h"
#include "config.h"
#include "./paxos/coordinator.h"
#include "concurrentqueue.h"

namespace janus {

inline void read_log(const char* log, int length, const char* custom){
        unsigned long long int cid = 0;
        memcpy(&cid, log, sizeof(unsigned long long int));
        Log_info("commit id %lld and length %d from %s", cid, length, custom);
}

class SubmitPool {
private:
  struct start_submit_pool_args {
    SubmitPool* subpool;
  };

  int n_;
  std::list<std::function<void()>*>* q_;
  pthread_cond_t not_empty_;
  pthread_mutex_t m_;
  pthread_mutex_t run_;
  pthread_t th_;
  bool should_stop_{false};

  static void* start_thread_pool(void* args) {
    start_submit_pool_args* t_args = (start_submit_pool_args *) args;
    t_args->subpool->run_thread();
    delete t_args;
    pthread_exit(nullptr);
    return nullptr;
  }
  void run_thread() {
    for (;;) {
      function<void()>* job = nullptr;
      Pthread_mutex_lock(&m_);
      while (q_->empty()) {
        Pthread_cond_wait(&not_empty_, &m_);
      }
      Pthread_mutex_lock(&run_);
      job = q_->front();
      q_->pop_front();
      Pthread_mutex_unlock(&m_);
      if (job == nullptr) {
        Pthread_mutex_unlock(&run_);
        break;
      }
      (*job)();
      delete job;
      Pthread_mutex_unlock(&run_);
    }
  }
  bool try_pop(std::function<void()>** t) {
    bool ret = false;
    if (!q_->empty()) {
        ret = true;
        *t = q_->front();
        q_->pop_front();
    }
    return ret;
  }

public:
  SubmitPool()
  : n_(1), th_(0), q_(new std::list<std::function<void()>*>), not_empty_(), m_(), run_() {
    verify(n_ >= 0);
    Pthread_mutex_init(&m_, nullptr);
    Pthread_mutex_init(&run_, nullptr);
    Pthread_cond_init(&not_empty_, nullptr);
    for (int i = 0; i < n_; i++) {
      start_submit_pool_args* args = new start_submit_pool_args();
      args->subpool = this;
      Pthread_create(&th_, nullptr, SubmitPool::start_thread_pool, args);
    }
  }
  SubmitPool(const SubmitPool&) = delete;
  SubmitPool& operator=(const SubmitPool&) = delete;
  ~SubmitPool() {
    should_stop_ = true;
    for (int i = 0; i < n_; i++) {
      Pthread_mutex_lock(&m_);
      q_->push_back(nullptr); //death pill
      Pthread_cond_signal(&not_empty_);
      Pthread_mutex_unlock(&m_);
    }
    for (int i = 0; i < n_; i++) {
      Pthread_join(th_, nullptr);
    }
    Log_debug("%s: enter in wait_for_all", __FUNCTION__);
    wait_for_all();
    Pthread_cond_destroy(&not_empty_);
    Pthread_mutex_destroy(&m_);
    Pthread_mutex_destroy(&run_);
    delete q_;
  }
  void wait_for_all() {
    for (int i = 0; i < n_; i++) {
      function<void()>* job;
      Pthread_mutex_lock(&m_);
      Pthread_mutex_lock(&run_);
      while (try_pop(&job)) {
        if (job != nullptr) {
          (*job)();
          delete job;
        }
      }
      Pthread_mutex_unlock(&m_);
      Pthread_mutex_unlock(&run_);
    }
  }
  int add(const std::function<void()>& f) {
    if (should_stop_) {
      return -1;
    }
    Pthread_mutex_lock(&m_);
    q_->push_back(new function<void()>(f));
    Pthread_cond_signal(&not_empty_);
    Pthread_mutex_unlock(&m_);
    return 0;
  }
};

class LogEntry : public Marshallable {
public:
  char* operation_ = nullptr;
  int length = 0;
  std::string log_entry;
  shared_ptr<char> operation_test;

  LogEntry() : Marshallable(MarshallDeputy::CONTAINER_CMD){
    bypass_to_socket_ = true;
  }
  virtual ~LogEntry() {
    //Log_info("oh lord jetson destroyed another one %d", length);
    //read_log(operation_test.get(), length, "while destroying");
    if (operation_ != nullptr) delete operation_;
    operation_ = nullptr;
    //free(operation_test.get());
  }
  virtual Marshal& ToMarshal(Marshal&) const override;
  virtual Marshal& FromMarshal(Marshal&) override;
  size_t EntitySize() override {
    return sizeof(int) + length;
  }
};

/*
inline rrr::Marshal& operator<<(rrr::Marshal &m, const LogEntry &cmd) {
  m << cmd.length;
  m << cmd.log_entry;
  return m;
}

inline rrr::Marshal& operator>>(rrr::Marshal &m, LogEntry &cmd) {
  m >> cmd.length;
  m >> cmd.log_entry;
  return m;
}
*/
class BulkPaxosCmd : public  Marshallable {
public:
  vector<slotid_t> slots{};
  vector<ballot_t> ballots{};
  vector<shared_ptr<MarshallDeputy>> cmds{};

  BulkPaxosCmd() : Marshallable(MarshallDeputy::CMD_BLK_PXS) {
    bypass_to_socket_ = true;
  }
  virtual ~BulkPaxosCmd() {
      slots.clear();
      ballots.clear();
      cmds.clear();
  }
  Marshal& ToMarshal(Marshal& m) const override {
      m << (int32_t) slots.size();
      for(auto i : slots){
          m << i;
      }
      m << (int32_t) ballots.size();
      for(auto i : ballots){
          m << i;
      }
      m << (int32_t) cmds.size();
      for (auto sp : cmds) {
        auto p = sp.get();
          m << *p;
      }
      return m;
  }

  Marshal& FromMarshal(Marshal& m) override {
      int32_t szs, szb, szc;
      m >> szs;
      for (int i = 0; i < szs; i++) {
          slotid_t x;
          m >> x;
          slots.push_back(x);
      }
      m >> szb;
      for (int i = 0; i < szs; i++) {
          ballot_t x;
          m >> x;
          ballots.push_back(x);
      }
      m >> szc;
      for (int i = 0; i < szc; i++) {
        auto x = new MarshallDeputy;
        m >> *x;
	      auto sp_md = shared_ptr<MarshallDeputy>(x);
	      cmds.push_back(sp_md);
      }
      return m;
  }

  size_t EntitySize() override {
    size_t sz = 0;
    sz += 3*sizeof(int32_t);
    for(int i = 0; i < slots.size(); i++){
      sz += sizeof(slotid_t);
      sz += sizeof(ballot_t);
      sz += cmds[i].get()->EntitySize();
    }
    return sz;
  }

  size_t WriteToFd(int fd) override {
    int32_t batch = slots.size();
    char *p = (char*)malloc(3*sizeof((int32_t)) + batch*(sizeof(slotid_t) + sizeof(ballot_t)));
    int wrt = 0;
    size_t sz = 0;
    wrt += memcpy(p + wrt, &batch, sizeof(int32_t));
    for(auto i : slots){
      wrt += memcpy(p + wrt, &i, sizeof(slotid_t));
    }
    wrt += memcpy(p + wrt, &batch, sizeof(int32_t));
    for(auto i : ballots){
      wrt += memcpy(p + wrt, &i, sizeof(ballot_t));
    }
    memcpy(p + wrt, &batch, sizeof(int32_t));
    sz += std::write(fd, p, wrt);
    for (auto cmdsp : cmds) {
      sz += cmdsp.get()->WriteToFd(fd);
    }
    verify(sz == EntitySize());
    return sz;
  }
};
class PaxosWorker {
private:
  inline void _Submit(shared_ptr<Marshallable>);
  inline void _BulkSubmit(shared_ptr<Marshallable>, int);

  rrr::Mutex finish_mutex{};
  rrr::CondVar finish_cond{};
  std::function<void(const char*, int)> callback_ = nullptr;
  vector<Coordinator*> created_coordinators_{};
  vector<shared_ptr<Coordinator>> created_coordinators_shrd{};
  struct timeval t1;
  struct timeval t2;

public:
  std::atomic<int> n_current{0};
  std::atomic<int> n_submit{0};
  std::atomic<int> n_tot{0};
  SubmitPool* submit_pool = nullptr;
  rrr::PollMgr* svr_poll_mgr_ = nullptr;
  vector<rrr::Service*> services_ = {};
  rrr::Server* rpc_server_ = nullptr;
  base::ThreadPool* thread_pool_g = nullptr;
  // for microbench
  std::atomic<int> submit_num{0};
  int tot_num = 0;
  int submit_tot_sec_ = 0;
  int submit_tot_usec_ = 0;

  rrr::PollMgr* svr_hb_poll_mgr_g = nullptr;
  ServerControlServiceImpl* scsi_ = nullptr;
  rrr::Server* hb_rpc_server_ = nullptr;
  base::ThreadPool* hb_thread_pool_g = nullptr;

  Config::SiteInfo* site_info_ = nullptr;
  Frame* rep_frame_ = nullptr;
  TxLogServer* rep_sched_ = nullptr;
  Communicator* rep_commo_ = nullptr;
  std::recursive_mutex mtx_worker_submit{};
  static moodycamel::ConcurrentQueue<shared_ptr<Coordinator>> coo_queue;
  static std::queue<shared_ptr<Coordinator>> coo_queue_nc;
  moodycamel::ConcurrentQueue<Marshallable*> replay_queue;
  vector<shared_ptr<Coordinator>> all_coords = vector<shared_ptr<Coordinator>>(1000000, nullptr);
  int bulk_writer = 0;
  int bulk_reader = 0;
  rrr::Mutex nc_submit_l_;
  const unsigned int cnt = bulkBatchCount;
  pthread_t bulkops_th_;
  pthread_t replay_th_;
  bool stop_flag = true;
  bool stop_replay_flag = true;

  void SetupHeartbeat();
  void InitQueueRead();
  void SetupBase();
  int  deq_from_coo(vector<shared_ptr<Coordinator>>&);
  void SetupService();
  void SetupCommo();
  void ShutDown();
  void Next(Marshallable&);
  void WaitForSubmit();
  void IncSubmit();
  void BulkSubmit(const vector<shared_ptr<Coordinator>>&);
  void AddAccept(shared_ptr<Coordinator>);
  void AddAcceptNc(shared_ptr<Coordinator>);
  void AddReplayEntry(Marshallable&);
  void submitJob(std::shared_ptr<Job>);
  static void* StartReadAccept(void*);
  static void* StartReplayRead(void*);
  static void* StartReadAcceptNc(void*);
  PaxosWorker();
  ~PaxosWorker();

  static const uint32_t CtrlPortDelta = 10000;
  void WaitForShutdown();
  bool IsLeader(uint32_t);
  bool IsPartition(uint32_t);

  void Submit(const char*, int, uint32_t);
  void register_apply_callback(std::function<void(const char*, int)>);
  rrr::PollMgr * GetPollMgr(){
      return svr_poll_mgr_;
  }
};

} // namespace janus
