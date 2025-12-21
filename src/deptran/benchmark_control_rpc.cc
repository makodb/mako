#include "__dep__.h"
#include "command.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "benchmark_control_rpc.h"
#include "client_worker.h"


extern vector<unique_ptr<janus::ClientWorker>> client_workers_g;

namespace janus {
vector<ServerControlServiceImpl *> ServerControlServiceImpl::scsi_s{};

const char S_RES_KEY_N_SCC[] = "scc";
const char S_RES_KEY_N_ASK[] = "ask";
const char S_RES_KEY_START_GRAPH[] = "start_graph";
const char S_RES_KEY_COMMIT_GRAPH[] = "commit_graph";
const char S_RES_KEY_ASK_GRAPH[] = "ask_graph";

const std::string ServerControlServiceImpl::STAT_SZ_SCC = "scc";
const std::string ServerControlServiceImpl::STAT_N_ASK = "ask";
const std::string ServerControlServiceImpl::STAT_SZ_GRAPH_START = "start_graph";
const std::string ServerControlServiceImpl::STAT_SZ_GRAPH_COMMIT = "commit_graph";
const std::string ServerControlServiceImpl::STAT_SZ_GRAPH_ASK = "ask_graph";
const std::string ServerControlServiceImpl::STAT_RO6_SZ_VECTOR = "ack_start_vector";

void ServerControlServiceImpl::shutdown_wrapper(int sig) {
  for (auto s : scsi_s) {
    s->do_shutdown();
  }
}

void ServerControlServiceImpl::set_sig_handler() {
  struct sigaction sact;
  sigemptyset(&sact.sa_mask);
  sact.sa_flags = 0;
  sact.sa_handler = shutdown_wrapper;
  sigaction(SIGALRM, &sact, NULL);
  sig_handler_set_ = true;
}

void ServerControlServiceImpl::do_shutdown() {
  Log_info("Shutdown Server Control Service");
  {
    auto guard = status_->lock().unwrap();
    guard->status = Status::STOP;
  }
  status_cond_->notify_all();
}

void ServerControlServiceImpl::server_shutdown(rrr::DeferredReply defer) {
  do_shutdown();
  defer.reply();
}

void ServerControlServiceImpl::server_ready(rrr::i32 *res, rrr::DeferredReply defer) {
  {
    auto guard = status_->lock().unwrap();
    *res = (guard->status == Status::RUN) ? 1 : 0;
  }
  defer.reply();
}

void ServerControlServiceImpl::do_statistics(const char *key,
                                             int64_t value_delta) {
  auto guard = stats_->lock().unwrap();
  auto it = guard->statistics.find(key);
  if (it == guard->statistics.end())
    guard->statistics[key] = (ValueTimesPair) {value_delta, 1};
  else {
    it->second.value += value_delta;
    it->second.times++;
  }
}

void ServerControlServiceImpl::server_heart_beat(rrr::DeferredReply defer) {
  if (!sig_handler_set_)
    set_sig_handler();
  alarm(timeout_);
  defer.reply();
}

void ServerControlServiceImpl::server_heart_beat_with_data(ServerResponse *res, rrr::DeferredReply defer) {
  res->cpu_util = rrr::CPUInfo::cpu_stat()[0];
  if (recorder_) {
    AvgStat r_cnt = recorder_->stat_cnt_.reset();
    AvgStat r_sz = recorder_->stat_sz_.reset();
    res->r_cnt_sum = r_cnt.sum_;
    res->r_cnt_num = r_cnt.n_stat_;
    res->r_sz_sum = r_sz.sum_;
    res->r_sz_num = r_sz.n_stat_;
  } else {
    res->r_cnt_sum = 0;
    res->r_cnt_num = 0;
    res->r_sz_sum = 0;
    res->r_sz_num = 0;
  }
  if (!sig_handler_set_)
    set_sig_handler();
  alarm(timeout_);

  auto guard = stats_->lock().unwrap();
  for (auto it = guard->statistics.begin(); it != guard->statistics.end(); it++) {
    res->statistics[std::string(it->first)] = it->second;
  }

  for (auto &pair : guard->stats) {
    auto &name = pair.first;
    auto &stat = pair.second;
    auto ss = stat->reset();
    verify(ss.sum_ >= 0);
    verify(ss.n_stat_ >= 0);
    Log_info("stat name: %s, value: %lld, times: %lld",
             name.c_str(), ss.sum_, ss.n_stat_);
    res->statistics[name].value = ss.sum_;
    res->statistics[name].times = ss.n_stat_;
  }

  defer.reply();
}

ServerControlServiceImpl::ServerControlServiceImpl(unsigned int timeout,
                                                   Recorder *recorder) :
        recorder_(recorder),
        stats_(rusty::make_box<rusty::Mutex<Stats>>(Stats{})),
        status_(rusty::make_box<rusty::Mutex<StatusState>>(StatusState{})),
        status_cond_(rusty::make_box<rusty::Condvar>()),
        timeout_(timeout),
        sig_handler_set_(false) {

  scsi_s.push_back(this);
}

ServerControlServiceImpl::~ServerControlServiceImpl() {
}

void ServerControlServiceImpl::set_ready() {
  {
    auto guard = status_->lock().unwrap();
    guard->status = Status::RUN;
  }
  rrr::CPUInfo::cpu_stat();
}

void ServerControlServiceImpl::wait_for_shutdown() {
  Log_debug("%s", __FUNCTION__);
  auto guard = status_->lock().unwrap();
  guard = status_cond_->wait_while(std::move(guard),
      [](StatusState& s) { return s.status != Status::STOP; }).unwrap();
  Log_debug("exit %s", __FUNCTION__);
}

bool operator<(const struct timespec &lhs, const struct timespec &rhs) {
  if (lhs.tv_sec < rhs.tv_sec)
    return true;
  else if (lhs.tv_sec == rhs.tv_sec)
    if (lhs.tv_nsec < rhs.tv_nsec)
      return true;
    else
      return false;
  else
    return false;
}

void clock_gettime(struct timespec *time) {
#ifdef __APPLE__ // OS X
  clock_serv_t cclock;
  mach_timespec_t mts;
  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);
  time->tv_sec = mts.tv_sec;
  time->tv_nsec = mts.tv_nsec;
#else
  ::clock_gettime(CLOCK_REALTIME, time);
#endif
}

double timespec2ms(struct timespec time) {
  return time.tv_sec * 1000.0 + time.tv_nsec / 1000000.0;
}

void ClientControlServiceImpl::client_shutdown(rrr::DeferredReply defer) {
  Log_info("Shutdown Client Control Service");
  {
    auto guard = status_->lock().unwrap();
    guard->status = Status::STOP;
  }
  status_cond_->notify_all();
  defer.reply();
}

void ClientControlServiceImpl::client_force_stop(rrr::DeferredReply defer) {
  int i = 0;
  for (; i < num_threads_; i++)
    if (coo_threads_[i] != NULL)
      pthread_kill(*(coo_threads_[i]), SIGALRM);
  defer.reply();
}

void ClientControlServiceImpl::client_response(const DepId& dep_id, ClientResponse *res, rrr::DeferredReply defer) {
  std::lock_guard<std::recursive_mutex> guard(mtx_);
  {
    auto status_guard = status_->lock().unwrap();
    res->is_finish = (status_guard->status == Status::FINISH) ? 1 : 0;
  }

  pthread_rwlock_wrlock(&collect_lock_);
  before_last_time_ = last_time_;
  clock_gettime(&last_time_);
  res->run_sec = (rrr::i64) (last_time_.tv_sec - start_time_.tv_sec);
  res->run_nsec = (rrr::i64) (last_time_.tv_nsec - start_time_.tv_nsec);

  res->period_sec = (rrr::i64) (last_time_.tv_sec - before_last_time_.tv_sec);
  res->period_nsec = (rrr::i64) (last_time_.tv_nsec - before_last_time_.tv_nsec);

  txn_info_switch_ = !txn_info_switch_;

  for (int i = 0; i < num_threads_; i++) {
    for (auto it = txn_info_[i].begin();
         it != txn_info_[i].end(); it++) {
      res->txn_info[it->first].start_txn += it->second.start_txn;
      res->txn_info[it->first].total_txn += it->second.total_txn;
      res->txn_info[it->first].total_try += it->second.total_try;
      res->txn_info[it->first].commit_txn += it->second.commit_txn;
      res->txn_info[it->first].num_exhausted += it->second.retries_exhausted;
      res->txn_info[it->first].interval_latency.insert(
              res->txn_info[it->first].interval_latency.end(),
              it->second.interval_latency.begin(),
              it->second.interval_latency.end());
      it->second.interval_latency.clear();
    }
  }
#ifdef LOG_LEVEL_AS_DEBUG
  LogClientResponse(res);
#endif
  pthread_rwlock_unlock(&collect_lock_);


  int use = 0;
  if (txn_info_switch_)
    use = 1;
  for (int i = 0; i < num_threads_; i++) {
    for (std::map<int32_t, txn_info_t>::iterator it = txn_info_[i].begin();
         it != txn_info_[i].end(); it++) {
      res->txn_info[it->first].this_latency.insert(
              res->txn_info[it->first].this_latency.end(),
              it->second.interval_latencies[use]->begin(), it->second.interval_latencies[use]->end());
      res->txn_info[it->first].last_latency.insert(
              res->txn_info[it->first].last_latency.end(),
              it->second.last_interval_latencies[use]->begin(), it->second.last_interval_latencies[use]->end());
      res->txn_info[it->first].attempt_latency.insert(
              res->txn_info[it->first].attempt_latency.end(),
              it->second.interval_attempt_latencies[use]->begin(),
              it->second.interval_attempt_latencies[use]->end());

      res->txn_info[it->first].num_try.insert(
              res->txn_info[it->first].num_try.end(),
              it->second.num_try[use]->begin(), it->second.num_try[use]->end());

      it->second.retries_exhausted = 0;
      it->second.num_try[use]->clear();
      it->second.interval_latencies[use]->clear();
      it->second.last_interval_latencies[use]->clear();
      it->second.interval_attempt_latencies[use]->clear();
    }
  }
  defer.reply();
}

void ClientControlServiceImpl::client_ready_block(rrr::i32 *res,
                                                  rrr::DeferredReply defer) {
  *res = 1;
  bool reply = false;
  {
    auto guard = status_->lock().unwrap();
    if (guard->status == Status::READY) {
      reply = true;
    } else {
      // Store callback for later reply
      guard->ready_block_defers.emplace_back([defer = std::move(defer)]() mutable { defer.reply(); });
    }
  }
  if (reply) {
    defer.reply();
  }
}

void ClientControlServiceImpl::client_ready(rrr::i32 *res, rrr::DeferredReply defer) {
  {
    auto guard = status_->lock().unwrap();
    *res = (guard->status == Status::READY) ? 1 : 0;
  }
  defer.reply();
}

void ClientControlServiceImpl::client_start(rrr::DeferredReply defer) {
  {
    auto guard = status_->lock().unwrap();
    guard->status = Status::RUN;
  }
  status_cond_->notify_all();
  clock_gettime(&start_time_);
  last_time_ = start_time_;
  before_last_time_ = start_time_;
  defer.reply();
}

void ClientControlServiceImpl::wait_for_start(unsigned int id) {
  coo_threads_[id] = (pthread_t *) malloc(sizeof(pthread_t));
  *(coo_threads_[id]) = pthread_self();

  std::vector<rusty::Function<void()>> callbacks_to_invoke;
  {
    auto guard = status_->lock().unwrap();
    guard->num_ready++;
    if (guard->num_ready == num_threads_) {
      guard->status = Status::READY;
      callbacks_to_invoke = std::move(guard->ready_block_defers);
      guard->ready_block_defers.clear();
    }
    // Wait until RUN or STOP
    guard = status_cond_->wait_while(std::move(guard),
        [](StatusState& s) { return s.status != Status::RUN && s.status != Status::STOP; }).unwrap();
  }
  // Invoke callbacks outside the lock
  for (auto& cb : callbacks_to_invoke) {
    cb();
  }
}

void ClientControlServiceImpl::wait_for_shutdown() {
  auto guard = status_->lock().unwrap();
  if (guard->status != Status::STOP) {
    guard->num_finish++;
    if (guard->num_finish == num_threads_)
      guard->status = Status::FINISH;
    guard = status_cond_->wait_while(std::move(guard),
        [](StatusState& s) { return s.status != Status::STOP; }).unwrap();
  }
}

void ClientControlServiceImpl::client_get_txn_names(std::map<i32, std::string> *txn_names, rrr::DeferredReply defer) {
  *txn_names = txn_names_;
  defer.reply();
}

ClientControlServiceImpl::ClientControlServiceImpl(unsigned int num_threads,
                                                   const std::map<int32_t, std::string> &txn_types)
        : status_(rusty::make_box<rusty::Mutex<StatusState>>(StatusState{})),
          status_cond_(rusty::make_box<rusty::Condvar>()),
          txn_info_(NULL), num_threads_(num_threads) {
  pthread_rwlock_init(&collect_lock_, NULL);
  coo_threads_ = (pthread_t **) malloc(sizeof(pthread_t * ) * num_threads_);
  txn_info_ = new std::map<int32_t, txn_info_t>[num_threads_];
  txn_info_switch_ = true;
  for (int i = 0; i < num_threads_; i++) {
    for (std::map<int32_t, std::string>::const_iterator cit = txn_types.begin();
         cit != txn_types.end(); cit++) {
      txn_info_[i][cit->first].init(cit->first);
    }
  }
  txn_names_ = txn_types;
}

ClientControlServiceImpl::~ClientControlServiceImpl() {
  pthread_rwlock_destroy(&collect_lock_);
  int i = 0;
  for (; i < num_threads_; i++) {
    for (std::map<int32_t, txn_info_t>::iterator it = txn_info_[i].begin();
         it != txn_info_[i].end(); it++)
      it->second.destroy();
    if (coo_threads_[i] != NULL)
      free(coo_threads_[i]);
  }
  free(coo_threads_);
  delete[] txn_info_;
}

const int MAX_LAT_LOG = 25;

void ClientControlServiceImpl::LogClientResponse(ClientResponse *res) {
  Log_debug("__%s__", __FUNCTION__);
  Log_debug("run_sec: %ld", res->run_sec);
  Log_debug("run_nsec: %ld", res->run_nsec);
  Log_debug("period_sec: %ld", res->period_sec);
  Log_debug("period_nsec: %ld", res->period_nsec);

  for (int i = 0; i < num_threads_; i++) {
    for (std::map<int32_t, txn_info_t>::iterator it = txn_info_[i].begin();
         it != txn_info_[i].end(); it++) {

      Log_debug("%d: start_txn: %d", it->first, res->txn_info[it->first].start_txn);
      Log_debug("%d: total_txn: %d", it->first, res->txn_info[it->first].total_txn);
      Log_debug("%d: total_try: %d", it->first, res->txn_info[it->first].total_try);
      Log_debug("%d: commit_txn: %d", it->first, res->txn_info[it->first].commit_txn);

      char output[1024];
      output[0] = '\0';
      Log_debug("%d: interval_latency: ", it->first);
      auto &interval_lat = res->txn_info[it->first].interval_latency;
      size_t cnt = 0;
      int n = 0;
      for (auto lat_it = interval_lat.begin(); lat_it != interval_lat.end() && n < MAX_LAT_LOG; ++lat_it, ++n) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%0.6f, ", *lat_it);
        if (strlen(buf) + cnt < sizeof(output)) {
          cnt += strlen(buf);
        } else {
          Log_debug("%s", output);
          output[0] = '\0';
          cnt = strlen(buf);
        }
        strcat(output, buf);
      }
      if (strlen(output) > 0) {
        Log_debug("%s", output);
      }
    }
  }
  Log_debug("__End %s__", __FUNCTION__);
}

void ClientControlServiceImpl::DispatchTxn(
    const TxDispatchRequest& req, TxReply* txn_reply, rrr::DeferredReply defer) {
  // TODO: fix -- we dont need to do this everytime.
  std::vector<ClientWorker*> locale0_workers;
  for (auto& worker : client_workers_g) {
    Log_debug("%s worker %d; site %d; locale %d", __FUNCTION__, worker->id, worker->my_site_.id, worker->my_site_.locale_id);
    if (worker->my_site_.locale_id == 0)
      locale0_workers.push_back(worker.get());
  }
  verify(locale0_workers.size() > 0);
  auto worker = locale0_workers[rrr::RandomGenerator::rand(0, locale0_workers.size()-1)];
  Log_info("%s: from coo %d; site %d", __FUNCTION__, req.id, worker->my_site_.id);
  verify(worker->my_site_.locale_id == 0);
  TxRequest request;
  size_t i = 0;
  for (auto &v : req.input) {
    request.input_[i] = v;
    i++;
  }
  request.n_try_ = 0;
  request.tx_type_ = req.tx_type;
  worker->AcceptForwardedRequest(std::move(request), txn_reply, std::move(defer));
}
}
