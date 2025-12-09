#pragma once

#include "__dep__.h"
#include "scheduler.h"
#include "tx.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace janus {

struct LuigiLogEntry {
  uint64_t local_deadline_ = 0; // proposed timestamp / deadline
  uint64_t agreed_deadline_ = 0; // agreed timestamp after negotiation
  std::atomic<uint32_t> exec_status_{0};
  std::atomic<uint32_t> agree_status_{0};

  txnid_t tid_ = 0; // transaction id
  std::shared_ptr<Marshallable> cmd_ = nullptr; // command payload

  std::function<void(const TxnOutput&)> reply_cb_ = nullptr;

  // keys touched locally on this leader (key ids are application specific)
  std::vector<uint32_t> local_keys_;

  // bookkeeping
  uint64_t send_time_ = 0;
  uint32_t owd_ = 0;

  // small constructor
  LuigiLogEntry(txnid_t tid = 0) : tid_(tid) {}
};

} // namespace janus
