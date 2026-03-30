#include "client_status.h"
#include "__dep__.h"

namespace janus {

ClientStatus::ClientStatus(unsigned int num_threads,
                           const std::map<int32_t, std::string>& txn_types)
    : sync_state_(rusty::make_box<rusty::Mutex<SyncState>>(SyncState{})),
      sync_cond_(rusty::make_box<rusty::Condvar>()),
      txn_info_(nullptr),
      num_threads_(num_threads) {
  pthread_rwlock_init(&collect_lock_, NULL);
  coo_threads_ = (pthread_t**)malloc(sizeof(pthread_t*) * num_threads_);
  for (unsigned int i = 0; i < num_threads_; i++) {
    coo_threads_[i] = nullptr;
  }
  txn_info_ = new std::map<int32_t, txn_info_t>[num_threads_];
  txn_info_switch_ = true;
  for (unsigned int i = 0; i < num_threads_; i++) {
    for (auto cit = txn_types.begin(); cit != txn_types.end(); cit++) {
      txn_info_[i][cit->first].init(cit->first);
    }
  }
  txn_names_ = txn_types;
}

ClientStatus::~ClientStatus() {
  pthread_rwlock_destroy(&collect_lock_);
  for (unsigned int i = 0; i < num_threads_; i++) {
    for (auto it = txn_info_[i].begin(); it != txn_info_[i].end(); it++) {
      it->second.destroy();
    }
    if (coo_threads_[i] != nullptr) {
      free(coo_threads_[i]);
    }
  }
  free(coo_threads_);
  delete[] txn_info_;
}

void ClientStatus::wait_for_start(unsigned int id) const {
  coo_threads_[id] = (pthread_t*)malloc(sizeof(pthread_t));
  *(coo_threads_[id]) = pthread_self();

  std::vector<rusty::Function<void()>> callbacks_to_invoke;
  {
    auto guard = sync_state_->lock().unwrap();
    guard->num_ready++;
    if (guard->num_ready == num_threads_) {
      guard->status = Status::READY;
      callbacks_to_invoke = std::move(guard->ready_block_defers);
      guard->ready_block_defers.clear();
    }
    // Wait until RUN or STOP
    guard = sync_cond_->wait_while(std::move(guard),
        [](SyncState& s) { return s.status != Status::RUN && s.status != Status::STOP; }).unwrap();
  }
  // Invoke callbacks outside the lock
  for (auto& cb : callbacks_to_invoke) {
    cb();
  }
}

void ClientStatus::wait_for_shutdown() const {
  auto guard = sync_state_->lock().unwrap();
  if (guard->status != Status::STOP) {
    guard->num_finish++;
    if (guard->num_finish == num_threads_) {
      guard->status = Status::FINISH;
    }
    guard = sync_cond_->wait_while(std::move(guard),
        [](SyncState& s) { return s.status != Status::STOP; }).unwrap();
  }
}

// Template specialization for collect_response is in the header
// (inline template method)

}  // namespace janus
