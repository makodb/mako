#include "marshal-value.h"
#include "coordinator.h"
#include "frame.h"
#include "constants.h"
#include "sharding.h"
#include "workload.h"

/**
 * What shoud we do to change this to asynchronous?
 * 1. Fisrt we need to have a queue to hold all transaction requests.
 * 2. pop next request, send start request for each piece until there is no
 *available left.
 *          in the callback, send the next piece of start request.
 *          if responses to all start requests are collected.
 *              send the finish request
 *                  in callback, remove it from queue.
 *
 *
 */

namespace janus {
std::mutex Coordinator::_dbg_txid_lock_{};
std::unordered_set<txid_t> Coordinator::_dbg_txid_set_{};

Coordinator::Coordinator(uint32_t coo_id,
                         int32_t benchmark,
                         rusty::Option<rusty::Arc<ClientStatus>> client_status,
                         uint32_t thread_id) : coo_id_(coo_id),
                                               benchmark_(benchmark),
                                               client_status_(std::move(client_status)),
                                               thread_id_(thread_id),
                                               mtx_() {
  uint64_t k = coo_id_;
  k <<= 32;
  k++;
  this->next_pie_id_.store(k);
  this->next_txn_id_.store(k);
  // removed `recorder_ = NULL;` — field gone.
  retry_wait_ = Config::GetConfig()->retry_wait();
  txn_timeout_ = Config::GetConfig()->get_txn_timeout();

	struct timespec begin, end;
	//clock_gettime(CLOCK_MONOTONIC, &begin);
  
	// removed `site_prepare_`,
	// `site_commit_`, and `site_abort_` `.resize(addrs.size(), 0)`
	// calls (and the dead `get_all_site_addr` it fed) — the three
	// counter vectors are gone.
	// removed `site_piece_.resize(addrs.size(), 0);`
	// — the `Coordinator::site_piece_` vector had no readers (only the
	// sibling `site_prepare_`/`site_commit_`/`site_abort_` are
	// incremented and inspected); the field went away in the same
	// commit.

	/*clock_gettime(CLOCK_MONOTONIC, &end);
	Log_info("time of 2nd part of CreateCoordinator: {}", end.tv_nsec-begin.tv_nsec);*/
}

Coordinator::~Coordinator() {
//  dropped commented-out
//  `for (i = 0; i < site_prepare_.size(); i++)` debug Log_debug
//  block — `site_*` counter vectors are gone.

  // removed `if (recorder_) delete recorder_;`
  // — field is gone; was always nullptr anyway.
#ifdef TXN_STAT

  for (auto& it : txn_stats_) {
        Log_info("TXN: {}", it.first);
        it.second.output();
      }
#endif /* ifdef TXN_STAT */

  // debug;
  mtx_.lock();
  mtx_.unlock();
// TODO (shuai) destroy all the rpc clients and proxies.
}

} // namespace janus
