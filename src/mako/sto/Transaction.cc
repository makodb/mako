// @unsafe: entire file uses STO with complex template instantiations, mutable fields, and interior mutability
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <mutex>
#include <thread>

#include "Transaction.hh"
#include "MassTrans.hh"
#include "deptran/s_main.h"
#include "sto/sync_util.hh"
#include "lib/common.h"
#include "benchmarks/benchmark_config.h"

import std;

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

std::function<int()> callback_ = nullptr;
// @safe
void register_sync_util(std::function<int()> cb) {
    callback_ = cb;
}

Transaction::testing_type Transaction::testing;
threadinfo_t Transaction::tinfo[MAX_THREADS];
__thread int TThread::the_id;
__thread int TThread::assigned_stable_id = -1;
std::atomic<int> TThread::next_stable_id{0};
__thread int TThread::nshards;
__thread int TThread::shard_index;
__thread int TThread::pid;
__thread int TThread::the_mode;
__thread int TThread::the_num_rpc_server;
__thread int TThread::the_is_micro;
__thread uint32_t TThread::the_counter;
__thread int TThread::the_role;
__thread int TThread::warehouses;
__thread int TThread::the_debug_bit;
__thread bool TThread::transget_without_throw;
__thread bool TThread::transget_without_stable;
__thread bool TThread::is_worker_leader;
__thread unsigned int TThread::trans_nosend_abort;
__thread bool TThread::in_loading_phase;
__thread int TThread::increment_id;
__thread int TThread::skipBeforeRemoteNewOrder;
__thread bool TThread::isHomeWarehouse;
__thread bool TThread::isRemoteShard;
__thread int TThread::skipBeforeRemotePayment;
__thread unsigned int TThread::readset_shard_bits;
__thread unsigned int TThread::writeset_shard_bits;
Transaction::epoch_state __attribute__((aligned(128))) Transaction::global_epochs = {
    1, 0, TransactionTid::increment_value, false
};
__thread Transaction *TThread::txn = nullptr;
__thread mako::ShardClient *TThread::sclient = nullptr;
__thread HashWrapper *TThread::tprops = nullptr;
Transaction::epoch_advance_callback_type Transaction::epoch_advance_callback;
#if defined(SIMPLE_WORKLOAD)
std::atomic<TransactionTid::type> __attribute__((aligned(128)))
Transaction::_TID{1};
#else
std::atomic<TransactionTid::type> __attribute__((aligned(128)))
Transaction::_TID{2 * TransactionTid::increment_value};
#endif
   // reserve TransactionTid::increment_value for prepopulated

static void __attribute__((used)) check_static_assertions() {
    static_assert(sizeof(threadinfo_t) % 128 == 0, "threadinfo is 2-cache-line aligned");
    static_assert(std::atomic<threadinfo_t::epoch_type>::is_always_lock_free,
                  "Transaction epochs must be lock-free");
    static_assert(std::atomic<TransactionTid::type>::is_always_lock_free,
                  "Transaction TIDs must be lock-free");
}

// @safe: atomically allocates from Mako's process-wide logical clock
bool Transaction::try_allocate_mako_timestamp(uint32_t& result) noexcept {
    auto& clock = sync_util::sync_logger::local_replica_id;
    uint32_t current = clock.load(std::memory_order_relaxed);
    while (current != 0 && current <= max_mako_timestamp) {
        const uint32_t next = current + 1;
        if (clock.compare_exchange_weak(current, next,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
            result = current;
            return true;
        }
    }
    result = 0;
    return false;
}

// @safe: atomically allocates after a transaction's read dependency
bool Transaction::try_allocate_mako_timestamp_after(
    uint32_t lower_bound, uint32_t& result) noexcept {
    result = 0;
    if (lower_bound >= max_mako_timestamp)
        return false;

    const uint32_t minimum = lower_bound + 1;
    auto& clock = sync_util::sync_logger::local_replica_id;
    uint32_t current = clock.load(std::memory_order_relaxed);
    while (current != 0 && current <= max_mako_timestamp) {
        const uint32_t candidate = std::max(current, minimum);
        if (clock.compare_exchange_weak(current, candidate + 1,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
            result = candidate;
            return true;
        }
    }
    return false;
}

// @safe: atomically catches the logical clock up past an observation
void Transaction::observe_mako_timestamp(uint32_t observed) noexcept {
    const uint32_t desired = observed < max_mako_timestamp
        ? observed + 1
        : max_mako_timestamp + 1;
    auto& clock = sync_util::sync_logger::local_replica_id;
    uint32_t current = clock.load(std::memory_order_relaxed);
    while (current != 0 && current < desired &&
           !clock.compare_exchange_weak(current, desired,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    }
}

// @safe
void Transaction::initialize() {
    static_assert(tset_initial_capacity % tset_chunk == 0, "tset_initial_capacity not an even multiple of tset_chunk");
    hash_base_ = 32768;
    tset_size_ = 0;
    lrng_state_ = 12897;
    participant_phase_ = p_none;
    for (unsigned i = 0; i != tset_initial_capacity / tset_chunk; ++i)
        tset_[i] = &tset0_[i * tset_chunk];
    for (unsigned i = tset_initial_capacity / tset_chunk; i != arraysize(tset_); ++i)
        tset_[i] = nullptr;
}

Transaction::~Transaction() {
    if (in_progress())
        silent_abort();
    // The initial chunks alias tset0_. Every later non-null chunk was
    // allocated by refresh_tset_chunk(). Indexing avoids pointer arithmetic
    // beyond the end of tset0_, which is undefined even without dereference.
    for (unsigned i = tset_initial_capacity / tset_chunk;
         i != arraysize(tset_); ++i)
        delete[] tset_[i];
}

// @safe
void Transaction::refresh_tset_chunk() {
    assert(tset_size_ % tset_chunk == 0);
    assert(tset_size_ < tset_max_capacity);
    if (!tset_[tset_size_ / tset_chunk])
        tset_[tset_size_ / tset_chunk] = new TransItem[tset_chunk];
    tset_next_ = tset_[tset_size_ / tset_chunk];
}

namespace {

std::mutex epoch_advance_callback_mutex;

class epoch_advancer_owner {
public:
    using entry_type = void (*)();

    explicit epoch_advancer_owner(entry_type entry) {
        Transaction::global_epochs.run.store(true, std::memory_order_release);
        try {
            worker_ = std::thread(entry);
        } catch (...) {
            Transaction::global_epochs.run.store(false,
                                                  std::memory_order_release);
            throw;
        }
    }

    epoch_advancer_owner(const epoch_advancer_owner&) = delete;
    epoch_advancer_owner& operator=(const epoch_advancer_owner&) = delete;

    ~epoch_advancer_owner() {
        Transaction::global_epochs.run.store(false, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
    }

private:
    std::thread worker_;
};

} // namespace

void Transaction::set_epoch_advance_callback(
    epoch_advance_callback_type callback) {
    std::lock_guard<std::mutex> lock(epoch_advance_callback_mutex);
    epoch_advance_callback = std::move(callback);
}

// @safe: C++ guarantees thread-safe, one-shot initialization of local statics
void Transaction::start_epoch_advancer() {
    static epoch_advancer_owner owner(&Transaction::epoch_advancer_loop);
    (void)owner;
}

// @unsafe: uses usleep and global epoch manipulation
void Transaction::epoch_advancer_loop() {

    // don't bother epoch'ing til things have picked up
    usleep(100000);
    while (global_epochs.run.load(std::memory_order_acquire)) {
        epoch_type g = global_epochs.global_epoch.load(std::memory_order_relaxed);
        epoch_type e = g;
        for (auto& t : tinfo) {
            const epoch_type thread_epoch =
                t.epoch.load(std::memory_order_acquire);
            if (thread_epoch != 0 && signed_epoch_type(thread_epoch - e) < 0)
                e = thread_epoch;
        }
        const epoch_type next_epoch = std::max(g + 1, epoch_type(1));
        global_epochs.global_epoch.store(next_epoch, std::memory_order_release);
        global_epochs.active_epoch.store(e, std::memory_order_release);
        global_epochs.recent_tid.store(
            Transaction::_TID.load(std::memory_order_relaxed),
            std::memory_order_relaxed);

        epoch_advance_callback_type callback;
        {
            std::lock_guard<std::mutex> lock(epoch_advance_callback_mutex);
            callback = epoch_advance_callback;
        }
        if (callback)
            callback(next_epoch);

        usleep(100000);
    }
}

// @safe
bool Transaction::preceding_duplicate_read(TransItem* needle) const {
    const TransItem* it = nullptr;
    for (unsigned tidx = 0; ; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        if (it == needle)
            return false;
        if (it->owner() == needle->owner() && it->key_ == needle->key_
            && it->has_read())
            return true;
    }
}

// @unsafe: uses TransItem::read_value, release_fence, and complex transaction validation
void Transaction::hard_check_opacity(TransItem* item, TransactionTid::type t) {
    // ignore opacity checks during commit; we're in the middle of checking
    // things anyway
    if (state_ == s_committing || state_ == s_committing_locked)
        return;

    // ignore if version hasn't changed
    if (item && item->has_read() && item->read_value<TransactionTid::type>() == t)
        return;

    // die on recursive opacity check; this is only possible for predicates
    if (unlikely(state_ == s_opacity_check)) {
        mark_abort_because(item, "recursive opacity check", t);
    abort:
        TXP_INCREMENT(txp_hco_abort);
        abort();
    }
    assert(state_ == s_in_progress);

    TXP_INCREMENT(txp_hco);
    if (TransactionTid::is_locked_elsewhere(t, threadid_)) {
        TXP_INCREMENT(txp_hco_lock);
        mark_abort_because(item, "locked", t);
        goto abort;
    }
    if (t & TransactionTid::nonopaque_bit)
        TXP_INCREMENT(txp_hco_invalid);

    state_ = s_opacity_check;
    start_tid_ = _TID.load(std::memory_order_relaxed);
    release_fence();
    TransItem* it = nullptr;
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        if (it->has_read()) {
            TXP_INCREMENT(txp_total_check_read);
            if (!it->owner()->check(*it, *this)
                && (!may_duplicate_items_ || !preceding_duplicate_read(it))) {
                mark_abort_because(item, "opacity check");
                goto abort;
            }
        } else if (it->has_predicate()) {
            TXP_INCREMENT(txp_total_check_predicate);
            if (!it->owner()->check_predicate(*it, *this, false)) {
                mark_abort_because(item, "opacity check_predicate");
                goto abort;
            }
        }
    }
    state_ = s_in_progress;
}

// @unsafe: manipulates transaction items with unlock and cleanup operations
void Transaction::stop(bool committed, unsigned* writeset,
                       unsigned nwriteset) noexcept {
    if (!committed) {
        TXP_INCREMENT(txp_total_aborts);
#if STO_DEBUG_ABORTS
        if (local_random() <= uint32_t(0xFFFFFFFF * STO_DEBUG_ABORTS_FRACTION)) {
            std::ostringstream buf;
            buf << "$" << (threadid_ < 10 ? "0" : "") << threadid_
                << " abort " << state_name(state_);
            if (abort_reason_)
                buf << " " << abort_reason_;
            if (abort_item_)
                buf << " " << *abort_item_;
            if (abort_version_)
                buf << " V" << TVersion(abort_version_);
            buf << '\n';
            std::cerr << buf.str();
        }
#endif
    }

    TXP_ACCOUNT(txp_max_transbuffer, buf_.buffer_size());
    TXP_ACCOUNT(txp_total_transbuffer, buf_.buffer_size());

    TransItem* it;
    if (!any_writes_)
        goto after_unlock;

    if (committed && !STO_SORT_WRITESET) {
        for (unsigned* idxit = writeset + nwriteset; idxit != writeset; ) {
            --idxit;
            if (*idxit < tset_initial_capacity)
                it = &tset0_[*idxit];
            else
                it = &tset_[*idxit / tset_chunk][*idxit % tset_chunk];
            if (it->needs_unlock())
                it->owner()->unlock(*it);
        }
        for (unsigned* idxit = writeset + nwriteset; idxit != writeset; ) {
            --idxit;
            if (*idxit < tset_initial_capacity)
                it = &tset0_[*idxit];
            else
                it = &tset_[*idxit / tset_chunk][*idxit % tset_chunk];
            if (it->has_write()) // always true unless a user turns it off in install()/check()
                it->owner()->cleanup(*it, committed);
        }
    } else {
        // Participants do not run try_commit(), so they do not retain the
        // coordinator's writeset array/count. On abort, inspect every staged
        // item and release whichever locks the participant acquired.
        if ((TThread::mode() == 1 && !committed) ||
            state_ == s_committing_locked) {
            for (unsigned tidx = tset_size_; tidx != first_write_; --tidx) {
                const unsigned index = tidx - 1;
                it = &tset_[index / tset_chunk][index % tset_chunk];
                if (it->needs_unlock())
                    it->owner()->unlock(*it);
            }
        }
        for (unsigned tidx = tset_size_; tidx != first_write_; --tidx) {
            const unsigned index = tidx - 1;
            it = &tset_[index / tset_chunk][index % tset_chunk];
            if (it->has_write())
                it->owner()->cleanup(*it, committed);
        }
    }

after_unlock:
    finish(committed);
}

// Finish a transaction whose item-specific unlock and cleanup work is done.
// Participant shard_unlock() uses this directly because it owns that cleanup
// loop rather than the coordinator writeset layout consumed by stop().
void Transaction::finish(bool committed) noexcept {
    assert(state_ < s_aborted);
    // TODO: this will probably mess up with nested transactions
    threadinfo_t& thr = tinfo[TThread::id()];
    if (thr.trans_end_callback)
        thr.trans_end_callback();
    // XXX should reset trans_end_callback after calling it...
    state_ = s_aborted + committed;
    participant_phase_ = p_none;
}

// @safe
bool Transaction::shard_try_lock_last_writeset() {
    assert(TThread::id() == threadid_);

    // find the last TransItem
    TransItem* it = nullptr;
    if (tset_size_ == 0) return true;
    for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
        auto base = tset_[tidx / tset_chunk];
        it = base + tidx % tset_chunk;
        if (it->has_write()) {
            if (!it->owner()->lock(*it, *this)) {
                return false;
            }
            it->__or_flags(TransItem::lock_bit);
            state_ = s_committing_locked;
            break;
        }
        if (tidx == 0) break;
    }
    return true;
}

// @safe
int Transaction::shard_validate() {
    //print_stats();
    assert(TThread::id() == threadid_);

    TransItem* it = nullptr;
    if (tset_size_ == 0) return 0;
    for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
        auto base = tset_[tidx / tset_chunk];
        it = base + tidx % tset_chunk;
        if (it->has_read()) {
            if (!it->owner()->check(*it, *this)
                && (!may_duplicate_items_ || !preceding_duplicate_read(it))) {
                return 1;
            }
        }
        if (tidx == 0) break;
    }
    return 0;
}

// @unsafe: calls unsafe serialize_util function
void Transaction::shard_serialize_util(uint32_t timestamp) {
    if (!BenchmarkConfig::getInstance().getIsReplicated()) {return ;}
    #if defined(SIMPLE_WORKLOAD)
        int small_batch_num=2;
    #else
        int small_batch_num=100;
    #endif
    serialize_util(1 /* anything > 0 */, true, MAX_ARRAY_SIZE_IN_BYTES_SMALL, small_batch_num, timestamp);
}

// @safe
uint8_t Transaction::get_current_term() const {
    if(callback_ != nullptr){
        if(!current_term_)
            current_term_ = callback_();
    }else{
        current_term_ = 0;
    }
    return current_term_;
}

// @unsafe: uses __sync_fetch_and_add and TObject::install
void Transaction::shard_install(uint32_t timestamp) noexcept {
    assert(TThread::id() == threadid_);
    assert(state_ < s_aborted);

    // Receipt of INSTALL is the participant's irreversible 2PC commit
    // decision. Record it before publishing any item so teardown cannot run
    // abort cleanup over an already-installed MassTrans row.
    if (participant_phase_ != p_collecting) {
        Warning("STO participant INSTALL phase violation: phase=%u state=%u "
                "writes=%d items=%u thread=%d",
                static_cast<unsigned>(participant_phase_),
                static_cast<unsigned>(state_), any_writes_, tset_size_,
                TThread::id());
        std::terminate();
    }
    if (any_writes_ && state_ != s_committing_locked) {
        Warning("STO participant INSTALL lock violation: phase=%u state=%u "
                "writes=%d items=%u thread=%d",
                static_cast<unsigned>(participant_phase_),
                static_cast<unsigned>(state_), any_writes_, tset_size_,
                TThread::id());
        std::terminate();
    }
    participant_phase_ = p_installing;

    // Update max timestamp from readset
    TThread::txn->maxTimestampReadSet = MAX(TThread::txn->maxTimestampReadSet, timestamp);
    tid_unique_ = timestamp;

    // Floor the process-wide clock past the installed timestamp.
    observe_mako_timestamp(tid_unique_);

    TransItem* it = nullptr;
    try {
        if (tset_size_ != 0) {
            for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
                auto base = tset_[tidx / tset_chunk];
                it = base + tidx % tset_chunk;
                if (it->has_write()) {
                    it->owner()->install(*it, *this);
                }
                if (tidx == 0) break;
            }
        }
    } catch (...) {
        // INSTALL is the irreversible 2PC decision. There is no sound local
        // rollback after a prefix has been published, and the generic TObject
        // interface does not provide a resumable install operation. Fail-stop
        // with locks retained rather than expose a partial commit as success.
        Warning("STO participant INSTALL publication failed: phase=%u "
                "state=%u writes=%d items=%u thread=%d",
                static_cast<unsigned>(participant_phase_),
                static_cast<unsigned>(state_), any_writes_, tset_size_,
                TThread::id());
        std::terminate();
    }
    participant_phase_ = p_installed;
}

// @unsafe: calls TObject::unlock and TObject::cleanup
void Transaction::shard_unlock(bool committed) noexcept {
    assert(TThread::id() == threadid_);
    assert(state_ < s_aborted);
    if (participant_phase_ == p_installing) {
        Warning("STO participant cleanup entered during INSTALL: state=%u "
                "writes=%d items=%u thread=%d",
                static_cast<unsigned>(state_), any_writes_, tset_size_,
                TThread::id());
        std::terminate();
    }

    // An install decision cannot subsequently be downgraded to abort, and a
    // caller cannot claim commit before INSTALL. Keep both protocol checks in
    // optimized builds, where assert() is absent.
    if (participant_phase_ == p_installed)
        committed = true;
    else if (committed) {
        Warning("STO participant committed cleanup without INSTALL: phase=%u "
                "state=%u writes=%d items=%u thread=%d",
                static_cast<unsigned>(participant_phase_),
                static_cast<unsigned>(state_), any_writes_, tset_size_,
                TThread::id());
        std::terminate();
    }

    TransItem* it = nullptr;
    if (tset_size_ != 0) {
        for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
            auto base = tset_[tidx / tset_chunk];
            it = base + tidx % tset_chunk;
            if (it->needs_unlock()) {
                it->owner()->unlock(*it);
                it->clear_needs_unlock();
            }
            if (tidx == 0) break;
        }
        for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
            auto base = tset_[tidx / tset_chunk];
            it = base + tidx % tset_chunk;
            if (it->has_write()) {
                it->owner()->cleanup(*it, committed);
            }
            if (tidx == 0) break;
        }
    }
    finish(committed);
}

// @unsafe: complex commit protocol with remote operations, locking, and validation
bool Transaction::try_commit(bool no_paxos) {
    assert(TThread::id() == threadid_);
#if ASSERT_TX_SIZE
    if (tset_size_ > TX_SIZE_LIMIT) {
        std::cerr << "transSet_ size at " << tset_size_
            << ", abort." << std::endl;
        assert(false);
    }
#endif
    TXP_ACCOUNT(txp_max_set, tset_size_);
    TXP_ACCOUNT(txp_total_n, tset_size_);

    assert(state_ == s_in_progress || state_ >= s_aborted);
    if (state_ >= s_aborted)
        return state_ > s_aborted;

    if (any_nonopaque_)
        TXP_INCREMENT(txp_commit_time_nonopaque);
#if !CONSISTENCY_CHECK
    // commit immediately if read-only transaction with opacity
    if (!any_writes_ && !any_nonopaque_) {
        stop(true, nullptr, 0);
        return true;
    }
#endif

    state_ = s_committing;

    unsigned writeset[tset_size_ ? tset_size_ : 1];
    unsigned nwriteset = 0;
    bool timestamp_exhausted = false;
    bool irreversible_decision = false;
    bool needs_mako_timestamp = false;
    // Single watermark timestamp instead of vector
    uint32_t watermarkTimestamp = 0;
    writeset[0] = tset_size_;

    //phase1
    TransItem* it = nullptr;

    std::vector<int> remote_table_id_batch;
    std::vector<std::string> key_batch;
    std::vector<std::string> value_batch;

    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_remote();
        if (it->has_write() && isRemote) {
            std::string key = "", val = "";
            if (hasInsertOp(it)) {  // key_write_value_type
                key = (*it).write_value<std::string>();
                versioned_str_struct *vvx = (*it).key<versioned_str_struct *>();
                const auto snapshot = vvx->read_value();
                val.assign(snapshot.data(), snapshot.length());
            } else {
                key = it->extra;
                val = (*it).template write_value<std::string>();
            }
            remote_table_id_batch.push_back(it->owner()->get_table_id());
            key_batch.push_back(key);
            value_batch.push_back(val);
        }
    }

    if (!remote_table_id_batch.empty()) {
        if (TThread::sclient == nullptr) {
            if (!no_paxos) {
                Warning("Missing ShardClient for remoteBatchLock in paxos path; aborting transaction");
                goto abort;
            }
            // Replay/no-paxos path may not have an initialized ShardClient.
            // Skip remote lock RPCs and continue applying local effects.
        } else {
            int ret = TThread::sclient->remoteBatchLock(remote_table_id_batch, key_batch, value_batch);
            if (ret > 0) {
                goto abort;
            }
        }
    }

    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_remote();
        if (it->has_write()) {
            writeset[nwriteset++] = tidx;
#if !STO_SORT_WRITESET
            //   nwriteset >= 1 should make more sense, not == 1
            if (nwriteset >= 1) {
                first_write_ = writeset[0];
                state_ = s_committing_locked;
            }
            if (!it->owner()->lock(*it, *this)) {
                mark_abort_because(it, "commit lock");
                goto abort;
            }
            it->__or_flags(TransItem::lock_bit);
#endif
        }
        if (it->has_read()) {
            TXP_INCREMENT(txp_total_r);
        }
        else if (it->has_predicate()) {
            TXP_INCREMENT(txp_total_check_predicate);
            if (!it->owner()->check_predicate(*it, *this, true)) {
                mark_abort_because(it, "commit check_predicate");
                goto abort;
            }
        }
    }

    first_write_ = writeset[0];

#if STO_SORT_WRITESET
    std::sort(writeset, writeset + nwriteset, [&] (unsigned i, unsigned j) {
        TransItem* ti = &tset_[i / tset_chunk][i % tset_chunk];
        TransItem* tj = &tset_[j / tset_chunk][j % tset_chunk];
        return *ti < *tj;
    });

    if (nwriteset) {
        state_ = s_committing_locked;
        auto writeset_end = writeset + nwriteset;
        for (auto it = writeset; it != writeset_end; ) {
            TransItem* me = &tset_[*it / tset_chunk][*it % tset_chunk];
            if (!me->owner()->lock(*me, *this)) {
                mark_abort_because(me, "commit lock");
                goto abort;
            }
            me->__or_flags(TransItem::lock_bit);
            ++it;
        }
    }
#endif


#if CONSISTENCY_CHECK
    fence();
    commit_tid();
    fence();
#endif

    needs_mako_timestamp =
        BenchmarkConfig::getInstance().getIsReplicated() ||
        TThread::writeset_shard_bits != 0 ||
        TThread::readset_shard_bits != 0 || maxTimestampReadSet != 0;
    if (!no_paxos && needs_mako_timestamp) {
        // Update single timestamp system
        if (!updateSingleTimestamp(timestamp_exhausted))
            goto abort;
        // A dependent commit must be strictly newer than every version it
        // observed. Reserving through the shared clock also prevents two
        // local coordinators from independently selecting read_max + 1.
        if (maxTimestampReadSet >= tid_unique_ &&
            !try_allocate_mako_timestamp_after(maxTimestampReadSet,
                                               tid_unique_)) {
            // Persist the exhausted sentinel even when the dependency itself,
            // rather than the old clock value, reached the wire-format limit.
            Transaction::observe_mako_timestamp(maxTimestampReadSet);
            timestamp_exhausted = true;
            goto abort;
        }

#if defined(TRACKING_ROLLBACK)
        if (get_current_term()==0) {
            rollbacks_tracker[mako::getCurrentTimeMillis()].push_back(tid_unique_);
        }
#endif
    }

    //phase2
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_remote();
        if (!isRemote && it->has_read()) {
            TXP_INCREMENT(txp_total_check_read);
            if (!it->owner()->check(*it, *this) // this is just a version check
                && (!may_duplicate_items_ || !preceding_duplicate_read(it))) {
                mark_abort_because(it, "commit check");
                goto abort;
            }
        }
    }

    if (TThread::readset_shard_bits > 0) {
        if (TThread::sclient == nullptr) {
            if (!no_paxos) {
                Warning("Missing ShardClient for remoteValidate in paxos path; aborting transaction");
                goto abort;
            }
        } else {
            // Single timestamp system: pass and receive single watermark
            int ret=TThread::sclient->remoteValidate(watermarkTimestamp);
            uint32_t currentWatermark = sync_util::sync_logger::single_watermark_.load(memory_order_acquire);
            if(watermarkTimestamp > currentWatermark) {
                // Update single watermark
                sync_util::sync_logger::single_watermark_.store(watermarkTimestamp, memory_order_release);
            }
            if (ret > 0) {
                goto abort;
            }
        }
    }

    // A remote/read-set maximum may have raised the selected timestamp above
    // this coordinator's ticket.  Floor the next-to-return clock before either
    // phase-3 write-set layout installs data.
    if (nwriteset)
        observe_mako_timestamp(tid_unique_);

    // Phase 3 begins the irreversible commit decision. No exception or
    // ordinary abort may run rollback cleanup after any item can be visible.
    try {
    irreversible_decision = nwriteset != 0;

    //phase3
#if STO_SORT_WRITESET
    for (unsigned tidx = first_write_; tidx != tset_size_; ++tidx) {
        it = &tset_[tidx / tset_chunk][tidx % tset_chunk];
        if (it->has_write()) {
            TXP_INCREMENT(txp_total_w);
            try {
                it->owner()->install(*it, *this);
            } catch (...) {
                // Publishing any prefix makes rollback unsound. Match the
                // participant INSTALL policy and fail-stop with locks held.
                std::terminate();
            }
        }
    }
#else
    if (nwriteset) {
        auto writeset_end = writeset + nwriteset;

        for (auto idxit = writeset; idxit != writeset_end; ++idxit) {
            if (likely(*idxit < tset_initial_capacity))
                it = &tset0_[*idxit];
            else
                it = &tset_[*idxit / tset_chunk][*idxit % tset_chunk];
            TXP_INCREMENT(txp_total_w);
            // to ensure invalid-bit to be reset in transPut for remote tables on the coordinator shard
            try {
                it->owner()->install(*it, *this);
            } catch (...) {
                // Publishing any prefix makes rollback unsound. Match the
                // participant INSTALL policy and fail-stop with locks held.
                std::terminate();
            }
        }
        if (TThread::writeset_shard_bits > 0||TThread::readset_shard_bits>0) {
            if (TThread::sclient == nullptr) {
                if (!no_paxos) {
                    Warning("Missing ShardClient after commit decision; failing stop");
                    goto abort;
                }
            } else {
            // Any non-success after INSTALL is indeterminate: some remote
            // participants may already have committed. Fail-stop instead of
            // returning a false abort and inviting an unsafe retry.
            if (TThread::sclient->remoteInstall(tid_unique_) !=
                mako::ErrorCode::SUCCESS)
                std::terminate();
            }
        }
    }
#endif

    if (BenchmarkConfig::getInstance().getIsReplicated()) {
        if (!no_paxos) {
            #if defined(SIMPLE_WORKLOAD)
                int large_batch_num=5;
            #else
                int large_batch_num=400;
            #endif

            if (TThread::get_is_micro())
                large_batch_num=3000;

            // Allow runtime override via MAKO_BATCH_SIZE env var (for batch sweep experiments)
            static int env_batch_override = []() {
                const char* env = std::getenv("MAKO_BATCH_SIZE");
                return env ? std::atoi(env) : 0;
            }();
            if (env_batch_override > 0)
                large_batch_num = env_batch_override;

            serialize_util(nwriteset, false, MAX_ARRAY_SIZE_IN_BYTES, large_batch_num, tid_unique_);
        }
    }

    } catch (...) {
        if (irreversible_decision)
            std::terminate();
        throw;
    }

    stop(true, writeset, nwriteset);
    irreversible_decision = false;
    // if (TThread::writeset_shard_bits > 0) {
    //     TThread::sclient->remoteUnLock();
    // }
    return true;

abort:
    if (irreversible_decision)
        std::terminate();
    TXP_INCREMENT(txp_commit_time_aborts);
    stop(false, nullptr, 0);
    if ((TThread::writeset_shard_bits > 0 || TThread::readset_shard_bits > 0) && TThread::sclient != nullptr) {
        TThread::sclient->remoteAbort();
    }
    if (timestamp_exhausted)
        throw TimestampExhausted();
    return false;
}

// serialize transactions into log and then sent it out via Paxos
// @unsafe: performs low-level memory operations with memcpy and raw pointers
inline void Transaction::serialize_util(unsigned nwriteset, bool on_remote, int max_bytes_size, int batch_size, uint32_t timestamp) const {
    if (nwriteset == 0) return;

    TransItem *it = nullptr;
    size_t w = 0;
    unsigned char *array = NULL;

    static thread_local std::shared_ptr<StringAllocator> instance = std::shared_ptr<StringAllocator>(
            new StringAllocator(TThread::get_nshards(), max_bytes_size, batch_size));
    array = instance->getLogOnly(w);

    unsigned short int _count = 0;  // 2bytes, the count of K-V pairs
    unsigned short int table_id = 0; // 2 bytes

#if defined(TRACKING_LATENCY)
    if (timestamp%1000==0&&TThread::getGlobalPartitionID()==4){
        uint32_t cur_time = mako::getCurrentTimeMillis();
        if (cur_time - start_time>= 5*1000 && cur_time - start_time <= 15*1000){ // time duration: [5,15]
            sample_transaction_tracker[timestamp] = mako::getCurrentTimeMillis() ;
        }
    }
#endif

    int epoch = get_current_term();
    // Single timestamp system: use same timestamp for all shards
    uint32_t tmp = epoch + timestamp * 10;
    // Single timestamp system: no need to loop over shards
    instance->update_commit_id(tmp);
    // 1. copy current Commit ID (single timestamp)
    // memcpy(array + w, &instance->latest_commit_timestamp, sizeof(uint32_t));
    memcpy(array + w, &tmp, sizeof(uint32_t));
    w+= sizeof(uint32_t);

    // 2. copy the count of K-V pairs
    w += sizeof(unsigned short int);
    size_t w_tmp_c = w;

    // 3. defer copying the len of K-V pairs
    w += sizeof(unsigned int);
    size_t w_tmp = w;

    unsigned short len_of_K = 0;
    unsigned short len_of_V = 0;

    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_remote();
        table_id = 0x0;
        if (!it->has_write() || isRemote) {
            continue;
        }
        _count++;

        // 4. copy the length of key and content of key.
        //    please note, it's not a typo, we have to get the key from transItem.write_value, NOT transItem.key!
        //    check the implementation: MassTrans => trans_write => Sto::new_item(this, val) and add_write
        //    also, due to different implementation purpose, we have to use different ways to retrieve key and value
        std::string kkx = "";
        if (hasInsertOp(it)) {
            kkx = (*it).write_value<std::string>();
        } else {
            kkx = it->extra;
        }
        len_of_K = kkx.length();
        if (len_of_K == 0) {
            std::cout << "Error while read Key [Slow Exit now]" << std::endl;
            //exit(1);
        }

        memcpy(array + w, (char *) &len_of_K, sizeof(unsigned short));
        w += sizeof(unsigned short);

        memcpy(array + w, (char *) kkx.data(), len_of_K);
        w += len_of_K;

        // 5. copy the length of value and content of value
        if (hasInsertOp(it)) {
            versioned_str_struct *vvx = (*it).key<versioned_str_struct *>();
            const auto snapshot = vvx->read_value();
            assert(snapshot.length() >= mako::EXTRA_BITS_FOR_VALUE);
            len_of_V = snapshot.length() - mako::EXTRA_BITS_FOR_VALUE;
            memcpy(array + w, (char *) &len_of_V, sizeof(unsigned short));
            w += sizeof(unsigned short);

            memcpy(array + w, (char *) snapshot.data(), len_of_V);
            w += len_of_V;
        } else {
            std::string vvx = "";
            if (hasDeleteOp(it)){
                vvx = "B"; // no one cares the content for a deleted item
                len_of_V = 1;
            }else{
                vvx = (*it).template write_value<std::string>();
                assert(vvx.length() > mako::EXTRA_BITS_FOR_VALUE);
                len_of_V = vvx.length() - mako::EXTRA_BITS_FOR_VALUE;
            }
            memcpy(array + w, (char *) &len_of_V, sizeof(unsigned short));
            w += sizeof(unsigned short);

            memcpy(array + w, (char *) vvx.data(), len_of_V);
            w += len_of_V;
        }

        // 6. copy table id
        table_id = it->owner()->get_table_id();
        if (hasDeleteOp(it)) {  // delete flag
            table_id = table_id | (1 << 15); // 1 << ((sizeof(unsigned short)*8)-1) = 1 << 15
        }
        if (table_id == 0) {
            Warning("table_id can't be a zero here");
        }
        memcpy(array + w, (char *) &table_id, sizeof(unsigned short));
        w += sizeof(unsigned short);
    }
    memcpy(array + w_tmp_c - sizeof(unsigned short int), (char *) &_count, sizeof(unsigned short int));
    unsigned int len_of_KV = w - w_tmp;
    memcpy(array + w_tmp - sizeof(unsigned int), (char *) &len_of_KV, sizeof(unsigned int));

    instance->update_ptr(w);
    size_t pos = 0;
    unsigned char *queueLog = instance->getLogOnly (pos);
    if(instance->checkPushRequired()) {
      assert(pos <= MAX_ARRAY_SIZE_IN_BYTES) ;
      if(pos!=0) {
          // 7. latest_commit_id: single timestamp*10+term
          memcpy (queueLog + pos, &instance->latest_commit_timestamp, sizeof(uint32_t));
          pos += sizeof(uint32_t);
          
          // 8. tracking purpose, the latency to commit a huge log
          uint32_t st_time = mako::getCurrentTimeMillis();
          memcpy (queueLog + pos, &st_time, sizeof(uint32_t));
          pos += sizeof(uint32_t);

          instance->update_ptr(pos);

          /*
          // Use local partition ID for Paxos workers
          int outstanding = get_outstanding_logs(TThread::getLocalPartitionID()) ;
          if (outstanding>20){
           usleep(10*1000); // wait 1 Paxos log time
          }

          while ((TThread::sclient == NULL) || !TThread::sclient->stopped) {
            if (outstanding>20) {
                usleep(50);
            } else {
                break;
            }
            outstanding = get_outstanding_logs(TThread::getLocalPartitionID()) ;
          }
          Warning("outstanding request: %d, par_id: %d", outstanding, TThread::getLocalPartitionID());

          // deal with logs from its corresponding threads
          if (TThread::in_loading_phase){
            Warning("add a log to nc, par_id:%d,", TThread::getLocalPartitionID());
            usleep(10*1000);
          }*/

        // FIX me: merge the logs from helper threads instead of a separate log
        // Use local partition ID for Paxos workers (they are indexed 0 to warehouses-1 per shard)
        add_log_to_nc((char *)queueLog, pos, TThread::getLocalPartitionID(), batch_size); // the partitionID for the helper thread

#ifndef DISABLE_DISK
        // Asynchronously persist to RocksDB
        auto& persistence = mako::RocksDBPersistence::getInstance();
        uint32_t shard_id = BenchmarkConfig::getInstance().getShardIndex();

        // Per-partition success/failure counters (max 64 partitions)
        static std::array<std::atomic<uint64_t>, 64> per_partition_success{};
        static std::array<std::atomic<uint64_t>, 64> per_partition_fail{};

        // Capture the timestamp and partition ID for the callback
        uint32_t persist_timestamp = instance->latest_commit_timestamp;
        // Use local partition ID for RocksDB persistence (per-shard storage)
        int partition_id = TThread::getLocalPartitionID();

        persistence.persistAsync((const char*)queueLog, pos, shard_id, partition_id,
            [persist_timestamp, partition_id](bool success) {
                if (success) {
                    // Update disk persistence timestamp for this partition
                    sync_util::sync_logger::updateDiskTimestamp(partition_id, persist_timestamp);

                    uint64_t count = per_partition_success[partition_id].fetch_add(1, std::memory_order_relaxed) + 1;
                    // Log every successful persist
                    if (count % 100 == 0) {
                        std::cout << "[RocksDB Helper] par_id=" << partition_id
                                      << ", success=" << count
                                      << ", failed=" << per_partition_fail[partition_id].load()
                                      << std::endl;
                    }
                } else {
                    uint64_t fail_count = per_partition_fail[partition_id].fetch_add(1, std::memory_order_relaxed) + 1;
                    std::cerr << "[RocksDB Helper] Persist FAILED: par_id=" << partition_id
                              << ", total_failures=" << fail_count << std::endl;
                }
            });
#endif
      }
      instance->resetMemory();
    }
}

// @unsafe: uses TransItem::key template method and string operations
void Transaction::print_stats() {
    if (tset_size_ == 0) return;
    TransItem* it = nullptr;
    if (tset_size_ == 0) return;
    for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
        auto base = tset_[tidx / tset_chunk];
        it = base + tidx % tset_chunk;
        versioned_str_struct *value = (*it).key<versioned_str_struct *>();
        const auto snapshot = value->read_value();
        std::string val(snapshot.data(), snapshot.length());
        std::string key = "";
        if (hasInsertOp(it)) {  // key_write_value_type
            key = (*it).write_value<std::string>();
        } else {
            key = it->extra;
        }
        Warning("print[obj:%p], has_write: %d, has_read: %d, has_lock: %d, has_insert: %d, has_delete: %d, invalidate: %d, key: %s, value: %s", it, it->has_write(), it->has_read(), it->needs_unlock(), hasInsertOp(it), hasDeleteOp(it), it->has_flag(TransactionTid::user_bit), key.c_str(), val.c_str());
        if (tidx == 0) break;
    }
}

// @safe
const char* Transaction::state_name(int state) {
    static const char* names[] = {"in-progress", "opacity-check", "committing", "committing-locked", "aborted", "committed"};
    if (unsigned(state) < arraysize(names))
        return names[state];
    else
        return "unknown-state";
}

// @unsafe: calls TObject::print with pointer dereference
void Transaction::print(std::ostream& w) const {
    w << "T0x" << (void*) this << " " << state_name(state_) << " [";
    const TransItem* it = nullptr;
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        if (tidx)
            w << " ";
        it->owner()->print(w, *it);
    }
    w << "]\n";
}

// @safe
void Transaction::print() const {
    print(std::cerr);
}

// @unsafe: uses TransItem template methods key, read_value, write_value, predicate_value
void TObject::print(std::ostream& w, const TransItem& item) const {
    w << "{" << typeid(*this).name() << " " << (void*) this << "." << item.key<void*>();
    if (item.has_read())
        w << " R" << item.read_value<void*>();
    if (item.has_write())
        w << " =" << item.write_value<void*>();
    if (item.has_predicate())
        w << " P" << item.predicate_value<void*>();
    w << "}";
}

unsigned long long int TObject::get_table_id() const {
    unsigned long long int temp = 10012;
    return temp;
}

bool TObject::get_is_remote() const {
    exit(1);
    return false;
}

std::ostream& operator<<(std::ostream& w, const Transaction& txn) {
    txn.print(w);
    return w;
}

std::ostream& operator<<(std::ostream& w, const TestTransaction& txn) {
    txn.print(w);
    return w;
}

std::ostream& operator<<(std::ostream& w, const TransactionGuard& txn) {
    txn.print(w);
    return w;
}
