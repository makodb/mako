#pragma once

#include "../scheduler.h"
#include "../tx.h"
#include "luigi_entry.h"

#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace janus {

class SchedulerLuigi : public SchedulerClassic {
 public:
  SchedulerLuigi();
  virtual ~SchedulerLuigi();

  // Start background threads
  void Start();
  void Stop();

  // Entry point for a Luigi-style dispatch. The coordinator (external)
  // is expected to send a transaction with a future timestamp: send_time and
  // bound (deadline = send_time + bound).
  void LuigiDispatch(txnid_t tx_id,
                     std::shared_ptr<Marshallable> cmd,
                     uint64_t send_time,
                     uint32_t bound,
                     std::function<void(const TxnOutput&)> reply_cb);

 protected:
  // Threads
  void HoldReleaseTd();
  void ExecTd();

  // Helpers
  void InsertIntoHoldBuffer(std::shared_ptr<LuigiLogEntry> entry);

  // hold buffer: ordered by (deadline, txid)
  std::map<std::pair<uint64_t, txnid_t>, std::shared_ptr<LuigiLogEntry>>
      hold_buffer_;
  std::mutex hold_mtx_;
  std::condition_variable hold_cv_;

  // per-key last released deadline (index by key id). Initialized lazily.
  std::vector<uint64_t> last_released_deadlines_;
  std::mutex deadlines_mtx_;

  // threads
  std::thread* hold_thread_ = nullptr;
  std::thread* exec_thread_ = nullptr;
  std::atomic<bool> running_{false};
};

} // namespace janus
