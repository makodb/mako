// @unsafe: entire file uses STO with complex template instantiations, mutable fields, and interior mutability
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <limits>

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
__thread int TThread::nshards;
__thread int TThread::shard_index;
__thread int TThread::pid;
__thread int TThread::the_mode;
__thread int TThread::the_num_rpc_server;
__thread int TThread::the_is_micro;
__thread int TThread::the_counter;
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
    1, 0, TransactionTid::increment_value, true
};
__thread Transaction *TThread::txn = nullptr;
__thread mako::ShardClient *TThread::sclient = nullptr;
__thread HashWrapper *TThread::tprops = nullptr;
std::function<void(threadinfo_t::epoch_type)> Transaction::epoch_advance_callback;
namespace {
thread_local bool local_transaction_cleanup_in_progress = false;

class commit_validation_gate_scope {
public:
    explicit commit_validation_gate_scope(
        const Transaction::commit_validation_gate* gate) noexcept
        : gate_(gate) {
        assert(gate_ == nullptr ||
               ((gate_->enter == nullptr) == (gate_->leave == nullptr)));
        assert(gate_ == nullptr || gate_->accept_ordered == nullptr ||
               (gate_->acquire_after_validation && gate_->enter == nullptr &&
                gate_->leave == nullptr));
    }

    void acquire() noexcept {
        assert(gate_ != nullptr);
        assert(!held_);
        if (gate_->enter != nullptr)
            gate_->enter(gate_->context);
        held_ = true;
    }

    void release() noexcept {
        if (!held_)
            return;
        held_ = false;
        if (gate_->leave != nullptr)
            gate_->leave(gate_->context);
    }

    bool held() const noexcept {
        return held_;
    }

    ~commit_validation_gate_scope() {
        release();
    }

private:
    const Transaction::commit_validation_gate* gate_;
    bool held_ = false;
};

#if defined(MAKO_LOCAL_TEST_HOOKS)
thread_local Transaction::test_commit_observer local_test_commit_observer =
    nullptr;
thread_local void* local_test_commit_observer_context = nullptr;
thread_local bool local_test_fail_next_cleanup = false;

struct test_cleanup_failure {};
#endif
}  // namespace

// @safe: reads the calling worker's cleanup-progress witness.
bool Transaction::cleanup_in_progress() noexcept {
    return local_transaction_cleanup_in_progress;
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
// @unsafe: stores a caller-borrowed callback and opaque context in thread-local
// state; their lifetime must cover every observed synchronous commit callback.
void Transaction::set_test_commit_observer(
    test_commit_observer observer, void* context) noexcept {
    local_test_commit_observer_context = context;
    local_test_commit_observer = observer;
}

// @safe: clears the thread-local borrowed observer before its context expires.
void Transaction::clear_test_commit_observer() noexcept {
    local_test_commit_observer = nullptr;
    local_test_commit_observer_context = nullptr;
}

// @safe: reads only this worker's trivially initialized thread-local pointer.
bool Transaction::test_commit_observer_registered() noexcept {
    return local_test_commit_observer != nullptr;
}

// @unsafe: synchronously calls a non-owning test callback while transaction
// write locks may be held. The callback is noexcept and must not allocate.
void Transaction::notify_test_commit_observer(
    test_commit_phase phase, uint32_t mako_timestamp) noexcept {
    const auto observer = local_test_commit_observer;
    if (observer != nullptr)
        observer(local_test_commit_observer_context, phase, mako_timestamp);
}

// @safe: arms one thread-local, one-shot branch at Transaction::stop entry.
void Transaction::test_fail_next_cleanup() noexcept {
    local_test_fail_next_cleanup = true;
}

// @safe: clears only a failure that stop() has not consumed yet.
bool Transaction::test_cancel_fail_next_cleanup() noexcept {
    const bool was_armed = local_test_fail_next_cleanup;
    local_test_fail_next_cleanup = false;
    return was_armed;
}
#endif
#if defined(SIMPLE_WORKLOAD)
std::atomic<TransactionTid::type> __attribute__((aligned(128)))
    Transaction::_TID{1};
#else
std::atomic<TransactionTid::type> __attribute__((aligned(128)))
    Transaction::_TID{2 * TransactionTid::increment_value};
#endif
   // reserve TransactionTid::increment_value for prepopulated

namespace {
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "packed cache ordering requires a lock-free u64 CAS");

// @safe: pure extraction from the immutable packed scalar argument.
constexpr uint64_t cache_order_timestamp(uint64_t state) noexcept {
    return (state >> Transaction::cache_order_timestamp_shift) &
        Transaction::cache_order_field_mask;
}

// @safe: pure extraction from the immutable packed scalar argument.
constexpr uint64_t cache_order_sequence(uint64_t state) noexcept {
    return state & Transaction::cache_order_field_mask;
}

// @safe: pure checked-layout replacement; callers validate the 29-bit value.
constexpr uint64_t with_cache_order_timestamp(
    uint64_t state, uint64_t timestamp) noexcept {
    return (state & ~(Transaction::cache_order_field_mask <<
                      Transaction::cache_order_timestamp_shift)) |
        (timestamp << Transaction::cache_order_timestamp_shift);
}

// @safe: pure checked-layout replacement; callers validate the 29-bit value.
constexpr uint64_t with_cache_order_sequence(
    uint64_t state, uint64_t sequence) noexcept {
    return (state & ~Transaction::cache_order_field_mask) | sequence;
}
}  // namespace

// @safe: atomically allocates from the process-wide packed Mako clock while
// preserving the cache sequence and general-certification state.
bool Transaction::try_allocate_mako_timestamp(uint32_t& result) noexcept {
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t timestamp = cache_order_timestamp(current);
        if (timestamp == 0 || timestamp > max_mako_timestamp) {
            result = 0;
            return false;
        }
        const uint64_t desired =
            with_cache_order_timestamp(current, timestamp + 1);
        if (state.compare_exchange_weak(current, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            result = static_cast<uint32_t>(timestamp);
            return true;
        }
    }
}

// @safe: the caller owns the packed general lock, so no restricted allocator
// can change the dense field. Timestamp-only allocators may still update the
// same word; the CAS loop preserves those independent changes.
bool Transaction::try_allocate_locked_cache_sequence(
    uint64_t& sequence) noexcept {
    sequence = 0;
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    for (;;) {
        assert((current & cache_order_general_lock) != 0);
        const uint64_t previous = cache_order_sequence(current);
        if (previous >= max_mako_timestamp)
            return false;
        const uint64_t desired =
            with_cache_order_sequence(current, previous + 1);
        if (state.compare_exchange_weak(current, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            sequence = previous + 1;
            return true;
        }
    }
}

// @safe: acquire the cache-only general certification bit. Restricted updates
// perform only a load while it is owned. Every caller already holds its full
// STO write set, so waiting here cannot create a lock-order cycle.
void Transaction::enter_cache_order_general() noexcept {
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    for (;;) {
        if ((current & cache_order_general_lock) != 0) {
            relax_fence();
            current = state.load(std::memory_order_acquire);
            continue;
        }
        if (state.compare_exchange_weak(
                current, current | cache_order_general_lock,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}

// @safe: release general certification while preserving any timestamp-only
// allocations which raced during validation. The five-bit epoch is diagnostic
// only; successful cache commits also change the non-wrapping packed fields.
void Transaction::leave_cache_order_general() noexcept {
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    for (;;) {
        assert((current & cache_order_general_lock) != 0);
        const uint64_t epoch =
            ((current & cache_order_epoch_mask) >> cache_order_epoch_shift) + 1;
        const uint64_t desired =
            (current & ~(cache_order_general_lock | cache_order_epoch_mask)) |
            ((epoch & UINT64_C(31)) << cache_order_epoch_shift);
        if (state.compare_exchange_weak(current, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
            return;
    }
}

// @safe: an Acquire RMW reads the immediately preceding modification in the
// packed word's total order and joins a preceding writer's release sequence.
uint64_t Transaction::order_cache_validation_prefix() noexcept {
    return sync_util::sync_logger::cache_order_state.fetch_add(
        UINT64_C(0), std::memory_order_acquire);
}

// @safe: diagnostic/cold snapshot of the process-wide packed state.
uint64_t Transaction::cache_order_snapshot() noexcept {
    return sync_util::sync_logger::cache_order_state.load(
        std::memory_order_acquire);
}

// @safe: namespace admission excludes cache terminals while this CAS resets
// only the dense field. Timestamp-only process users remain concurrent.
bool Transaction::reseed_cache_order_sequence(uint64_t sequence) noexcept {
    if (sequence > max_mako_timestamp)
        return false;
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    for (;;) {
        if ((current & cache_order_general_lock) != 0)
            return false;
        const uint64_t desired = with_cache_order_sequence(current, sequence);
        if (state.compare_exchange_weak(current, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
            return true;
    }
}

// @safe: assigns one checked nonzero Mako timestamp to this transaction
bool Transaction::try_assign_mako_timestamp(uint32_t& result) const noexcept {
    assert(state_ == s_committing_locked || state_ == s_committing);
    if (tid_unique_) {
        result = tid_unique_;
        return true;
    }
    if (!try_allocate_mako_timestamp(result))
        return false;
    tid_unique_ = result;
    return true;
}

// @safe: atomically catches the process-wide Mako clock up to an observation
void Transaction::observe_mako_timestamp(uint32_t observed) noexcept {
    const uint32_t desired = observed < max_mako_timestamp
        ? observed + 1
        : max_mako_timestamp + 1;
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    while (cache_order_timestamp(current) != 0 &&
           cache_order_timestamp(current) < desired) {
        const uint64_t next = with_cache_order_timestamp(current, desired);
        if (state.compare_exchange_weak(current, next,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
            return;
    }
}

// @safe: atomically advances the process-wide Mako logical clock
bool Transaction::advance_mako_timestamp_past(uint32_t observed) noexcept {
    // Store observed + 1, then leave room for that value to be minted while
    // advancing the counter once more. Zero and max_mako_timestamp + 1 are
    // permanent sentinels, so recovery cannot revive an exhausted clock.
    if (observed == 0 || observed >= max_mako_timestamp)
        return false;

    const uint32_t desired = observed + 1;
    auto& state = sync_util::sync_logger::cache_order_state;
    uint64_t current = state.load(std::memory_order_acquire);
    while (cache_order_timestamp(current) != 0 &&
           cache_order_timestamp(current) < desired) {
        const uint64_t next = with_cache_order_timestamp(current, desired);
        if (state.compare_exchange_weak(current, next,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
            return true;
    }
    const uint64_t timestamp = cache_order_timestamp(current);
    return timestamp != 0 && timestamp <= max_mako_timestamp;
}

static void __attribute__((used)) check_static_assertions() {
    static_assert(std::atomic<threadinfo_t::epoch_type>::is_always_lock_free,
                  "the epoch protocol requires lock-free 64-bit atomics");
    static_assert(std::atomic<TransactionTid::type>::is_always_lock_free,
                  "the transaction clock requires lock-free 64-bit atomics");
    static_assert(sizeof(threadinfo_t) % 128 == 0,
                  "threadinfo occupies isolated 128-byte cache slots");
}

// @safe
void Transaction::initialize() {
    static_assert(tset_initial_capacity % tset_chunk == 0, "tset_initial_capacity not an even multiple of tset_chunk");
    hash_base_ = 32768;
    tset_size_ = 0;
    lrng_state_ = 12897;
    for (unsigned i = 0; i != tset_initial_capacity / tset_chunk; ++i)
        tset_[i] = &tset0_[i * tset_chunk];
    for (unsigned i = tset_initial_capacity / tset_chunk; i != arraysize(tset_); ++i)
        tset_[i] = nullptr;
}

Transaction::~Transaction() {
    if (in_progress())
        silent_abort();
    TransItem* live = tset0_;
    for (unsigned i = 0; i != arraysize(tset_); ++i, live += tset_chunk)
        if (live != tset_[i])
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

// @unsafe: uses fetch_and_add, usleep, and global epoch manipulation
void* Transaction::epoch_advancer(void*) {
    static int num_epoch_advancers = 0;
    if (fetch_and_add(&num_epoch_advancers, 1) != 0)
        std::cerr << "WARNING: more than one epoch_advancer thread\n";

    // don't bother epoch'ing til things have picked up
    usleep(100000);
    while (global_epochs.run.load(std::memory_order_acquire)) {
        const epoch_type g =
            global_epochs.global_epoch.load(std::memory_order_seq_cst);
        epoch_type e = g;
        for (auto& t : tinfo) {
            const epoch_type participant_epoch =
                t.epoch.load(std::memory_order_seq_cst);
            if (participant_epoch != 0
                && signed_epoch_type(participant_epoch - e) < 0)
                e = participant_epoch;
        }
        const epoch_type next_epoch = std::max(g + 1, epoch_type(1));
        global_epochs.global_epoch.store(next_epoch,
                                         std::memory_order_seq_cst);
        global_epochs.active_epoch.store(e, std::memory_order_seq_cst);
        global_epochs.recent_tid.store(
            Transaction::_TID.load(std::memory_order_acquire),
            std::memory_order_relaxed);

        if (epoch_advance_callback)
            epoch_advance_callback(next_epoch);

        usleep(100000);
    }
    fetch_and_add(&num_epoch_advancers, -1);
    return NULL;
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
    start_tid_ = _TID.load(std::memory_order_acquire);
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
void Transaction::stop(bool committed, unsigned* writeset, unsigned nwriteset) {
    // This marker is a production safety witness, not only test machinery.
    // It is deliberately cleared only after native cleanup publishes terminal
    // state. Any exception leaves it set and forbids cleanup re-entry.
    local_transaction_cleanup_in_progress = true;
#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (local_test_fail_next_cleanup) {
        local_test_fail_next_cleanup = false;
        throw test_cleanup_failure{};
    }
#endif
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
        // in participant, we never invoke try_commit,
        // and no good way to set state_ = s_committing_locked; as try_commit do
        // so, we skip it blindly for participant
        if ((TThread::mode() == 1 && nwriteset>0) || state_ == s_committing_locked) {
            for (unsigned tidx = tset_size_; tidx != first_write_; ) {
                --tidx;
                if (likely(tidx < tset_initial_capacity))
                    it = &tset0_[tidx];
                else
                    it = &tset_[tidx / tset_chunk][tidx % tset_chunk];
                if (it->needs_unlock())
                    it->owner()->unlock(*it);
            }
        }
        for (unsigned tidx = tset_size_; tidx != first_write_; ) {
            --tidx;
            if (likely(tidx < tset_initial_capacity))
                it = &tset0_[tidx];
            else
                it = &tset_[tidx / tset_chunk][tidx % tset_chunk];
            if (it->has_write())
                it->owner()->cleanup(*it, committed);
        }
    }

after_unlock:
    // TODO: this will probably mess up with nested transactions
    threadinfo_t& thr = tinfo[TThread::id()];
    if (thr.trans_end_callback)
        thr.trans_end_callback();
    // XXX should reset trans_end_callback after calling it...
    state_ = s_aborted + committed;
    local_transaction_cleanup_in_progress = false;
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

// @unsafe: invokes TObject::install on transaction-owned items
void Transaction::shard_install(uint32_t timestamp) {
    assert(TThread::id() == threadid_);

    // Update max timestamp from readset
    TThread::txn->maxTimestampReadSet = MAX(TThread::txn->maxTimestampReadSet, timestamp);
    tid_unique_ = timestamp;

    // Floor the process-wide Mako clock past the chosen timestamp.
    observe_mako_timestamp(tid_unique_);

    TransItem* it = nullptr;
    if (tset_size_ == 0) return;
    for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
        auto base = tset_[tidx / tset_chunk];
        it = base + tidx % tset_chunk;
        if (it->has_write()) {
            it->owner()->install(*it, *this);
        }
        if (tidx == 0) break;
    }
}

// @unsafe: calls TObject::unlock and TObject::cleanup
void Transaction::shard_unlock(bool committed) {
    assert(TThread::id() == threadid_);

    TransItem* it = nullptr;
    if (tset_size_ == 0) return;
    for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
        auto base = tset_[tidx / tset_chunk];
        it = base + tidx % tset_chunk;
        if (it->needs_unlock()) {
            it->owner()->unlock(*it);
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

// @unsafe: decodes the three MassTrans write layouts from one borrowed
// TransItem. The returned spans remain owned by the transaction/tree and are
// valid only while the item cannot be mutated or cleaned up.
bool Transaction::export_local_canonical_write(
    const TransItem& item, canonical_write_view* write_out) const noexcept {
    if (write_out == nullptr)
        return false;
    *write_out = canonical_write_view{};
    assert(TThread::id() == threadid_);
    assert(state_ == s_in_progress || state_ == s_committing ||
           state_ == s_committing_locked);

    if (!item.has_write() || item.owner()->get_is_remote())
        return false;
    const bool is_insert = hasInsertOp(&item);
    const bool is_delete = hasDeleteOp(&item);
    if (is_insert && is_delete)
        return false;

    canonical_write_view view{};
    view.table_id = item.owner()->get_table_id();
    if (is_insert) {
        const std::string& key = item.write_value<std::string>();
        versioned_str_struct* row = item.key<versioned_str_struct*>();
        if (row == nullptr || row->length() < mako::EXTRA_BITS_FOR_VALUE)
            return false;
        view.op = canonical_write_view::operation::put;
        view.key = key.data();
        view.key_length = key.size();
        view.value = row->data();
        view.value_length = static_cast<size_t>(row->length()) -
                            mako::EXTRA_BITS_FOR_VALUE;
    } else {
        view.key = item.extra.data();
        view.key_length = item.extra.size();
        if (is_delete) {
            view.op = canonical_write_view::operation::remove;
            view.value = nullptr;
            view.value_length = 0;
        } else {
            const std::string& value = item.write_value<std::string>();
            if (value.size() < mako::EXTRA_BITS_FOR_VALUE)
                return false;
            view.op = canonical_write_view::operation::put;
            view.value = value.data();
            view.value_length = value.size() - mako::EXTRA_BITS_FOR_VALUE;
        }
    }
    *write_out = view;
    return true;
}

// @unsafe: visits the final MassTrans write set without copying. The
// representation matches serialize_util(); insert-then-delete is a net-empty
// mutation and is intentionally omitted.
bool Transaction::visit_local_canonical_writes(
    canonical_write_visitor visitor, void* context,
    uint32_t* count_out) const noexcept {
    if (count_out == nullptr)
        return false;
    *count_out = 0;
    assert(TThread::id() == threadid_);
    assert(state_ == s_in_progress || state_ == s_committing ||
           state_ == s_committing_locked);

    const TransItem* item = nullptr;
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        item = (tidx % tset_chunk ? item + 1
                                  : tset_[tidx / tset_chunk]);
        if (!item->has_write() || item->owner()->get_is_remote())
            continue;
        if (hasInsertOp(item) && hasDeleteOp(item))
            continue;

        canonical_write_view view{};
        if (!export_local_canonical_write(*item, &view))
            return false;

        if (*count_out == std::numeric_limits<uint32_t>::max())
            return false;
        if (visitor != nullptr && !visitor(context, view))
            return false;
        ++*count_out;
    }
    return true;
}

// @unsafe: walks transaction-owned items whose lifetime is protected by the
// active transaction. No borrowed key or value bytes escape this inspection.
bool Transaction::can_order_record_after_validation() const noexcept {
    assert(TThread::id() == threadid_);
    assert(state_ == s_in_progress);

    unsigned local_writes = 0;
    const TransItem* item = nullptr;
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        item = (tidx % tset_chunk ? item + 1
                                  : tset_[tidx / tset_chunk]);
        if (item->owner()->get_is_remote() || item->has_predicate())
            return false;
        if (hasInsertOp(item) || hasDeleteOp(item))
            return false;
        if (item->has_read() && !item->has_write())
            return false;
        if (item->has_write()) {
            if (++local_writes != 1)
                return false;
        } else if (!item->has_read()) {
            // Reject bookkeeping-only shapes which this proof does not know
            // how to classify. The ordinary early gate remains available.
            return false;
        }
    }
    return local_writes == 1;
}

// @unsafe: complex commit protocol with remote operations, locking, and validation
bool Transaction::try_commit(bool no_paxos,
                             post_validation_hook hook,
                             void* hook_context,
                             preinstall_failure* failure,
                             const commit_validation_gate* validation_gate) {
    assert(TThread::id() == threadid_);
    assert(validation_gate == nullptr || hook != nullptr);
    commit_validation_gate_scope validation_gate_scope(validation_gate);
    const bool requested_gate_after_validation =
        validation_gate != nullptr &&
        validation_gate->acquire_after_validation;
    // The Paxos path assigns and merges its timestamp before phase-two
    // validation. Falling back to the ordinary early gate preserves that
    // timestamp/gate order even if a future caller requests the restricted
    // local optimization on the wrong commit protocol.
    assert(!requested_gate_after_validation || no_paxos);
    const bool acquire_gate_after_validation =
        requested_gate_after_validation && no_paxos;
    bool ordered_hook_already_accepted = false;
    if (failure)
        *failure = preinstall_failure::none;
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

    unsigned writeset[tset_size_];
    unsigned nwriteset = 0;
    // Single watermark timestamp instead of vector
    uint32_t watermarkTimestamp = 0;
    writeset[0] = tset_size_;
#if defined(MAKO_LOCAL_TEST_HOOKS)
    const bool has_test_commit_observer =
        test_commit_observer_registered();
    unsigned installed_write_count = 0;
#endif

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
                val = std::string(vvx->data(), vvx->length());
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

#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (nwriteset != 0 && has_test_commit_observer) {
        notify_test_commit_observer(test_commit_phase::writeset_locked,
                                    0 /* not allocated yet */);
    }
#endif

    if (validation_gate != nullptr && nwriteset != 0 &&
        !acquire_gate_after_validation)
        validation_gate_scope.acquire();

    // The general path allocates the cache record's Mako logical timestamp
    // after the entire write set is locked but before validating the read set.
    // A failed transaction may consume a harmless timestamp gap. No cache log
    // position has been assigned yet, so validation failure needs no
    // cancellation slot. The restricted update path allocates below, after
    // validation, so it consumes neither timestamp nor gate turn on conflict.
    if (!acquire_gate_after_validation && nwriteset != 0 &&
        (hook != nullptr
#if defined(MAKO_LOCAL_TEST_HOOKS)
         || has_test_commit_observer
#endif
        )) {
        uint32_t timestamp = 0;
        if (!try_assign_mako_timestamp(timestamp)) {
            if (failure)
                *failure = preinstall_failure::timestamp_exhausted;
            goto abort;
        }
    }

#if CONSISTENCY_CHECK
    // The cache hook now carries Mako's logical timestamp; Silo's independent
    // version clock retains its legacy consistency-check behavior.
    fence();
    if (!commit_tid_)
        commit_tid();
    fence();
#endif

    if (!no_paxos){
        // Update single timestamp system
        if (!updateSingleTimestamp()) {
            if (failure)
                *failure = preinstall_failure::timestamp_exhausted;
            goto abort;
        }
        // Merge with max timestamp from read set. Legacy or corrupt values
        // outside the checked base domain must not reach u32 term encoding.
        if (maxTimestampReadSet > max_mako_timestamp) {
            if (failure)
                *failure = preinstall_failure::timestamp_exhausted;
            goto abort;
        }
        if (maxTimestampReadSet > tid_unique_) {
            tid_unique_ = maxTimestampReadSet;
        }

#if defined(TRACKING_ROLLBACK)
        if (get_current_term()==0) {
            rollbacks_tracker[mako::getCurrentTimeMillis()].push_back(tid_unique_);
        }
#endif
    }

#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (!acquire_gate_after_validation && nwriteset != 0 &&
        has_test_commit_observer) {
        assert(tid_unique_ != 0);
        notify_test_commit_observer(
            test_commit_phase::mako_timestamp_allocated, tid_unique_);
    }
#endif

    //phase2
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_remote();
        // Predicate checks normally happen while phase1 discovers and locks
        // writes. An ordered durability commit repeats them after entering its
        // validation gate so range anti-dependencies cannot slip between the
        // gate's timestamp order and the final point-read validation.
        if (validation_gate != nullptr && !isRemote &&
            !it->has_read() && it->has_predicate()) {
            TXP_INCREMENT(txp_total_check_predicate);
            if (!it->owner()->check_predicate(*it, *this, true)) {
                mark_abort_because(it, "ordered commit check_predicate");
                goto abort;
            }
        }
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

    // The restricted one-local-update profile has no observation outside its
    // complete write lock. It can therefore do ordinary validation in
    // parallel and serialize only the timestamp/log-position pair. General
    // transactions retain the early gate above so anti-dependencies and range
    // predicates keep their established order.
    if (acquire_gate_after_validation && nwriteset != 0) {
        uint32_t timestamp = 0;
        if (validation_gate->accept_ordered != nullptr) {
            const ordered_accept_result accepted =
                validation_gate->accept_ordered(validation_gate->context,
                                                 &timestamp);
            if (accepted != ordered_accept_result::accepted) {
                if (failure) {
                    *failure = accepted ==
                            ordered_accept_result::timestamp_exhausted
                        ? preinstall_failure::timestamp_exhausted
                        : preinstall_failure::hook_rejected;
                }
                goto abort;
            }
            assert(timestamp != 0 && timestamp <= max_mako_timestamp);
            assert(tid_unique_ == 0);
            tid_unique_ = timestamp;
            ordered_hook_already_accepted = true;
        } else {
            validation_gate_scope.acquire();
            if (!try_assign_mako_timestamp(timestamp)) {
                if (failure)
                    *failure = preinstall_failure::timestamp_exhausted;
                goto abort;
            }
        }
    }

#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (acquire_gate_after_validation && nwriteset != 0 &&
        has_test_commit_observer) {
        assert(tid_unique_ != 0);
        notify_test_commit_observer(
            test_commit_phase::mako_timestamp_allocated, tid_unique_);
    }
#endif

#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (nwriteset != 0 && has_test_commit_observer) {
        notify_test_commit_observer(
            test_commit_phase::local_validation_complete, tid_unique_);
    }
#endif

    // Every validation has succeeded and no write is visible yet. The hook
    // attaches preallocated storage to the ordered cache log. Rejection is a
    // definite abort because phase3 has not begun.
    if (!ordered_hook_already_accepted && hook != nullptr && nwriteset != 0 &&
        !hook(hook_context, tid_unique_)) {
        if (failure)
            *failure = preinstall_failure::hook_rejected;
        goto abort;
    }
    // The ordered record is now bound. Later transactions retain their own
    // write locks while waiting, so releasing here preserves anti-dependency
    // validation order without serializing record bytes or phase3 installs.
    validation_gate_scope.release();
    if (validation_gate != nullptr && nwriteset != 0 &&
        validation_gate->after_leave != nullptr &&
        !validation_gate->after_leave(validation_gate->context, tid_unique_)) {
        if (failure)
            *failure = preinstall_failure::hook_rejected;
        goto abort;
    }

#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (nwriteset != 0 && has_test_commit_observer) {
        notify_test_commit_observer(test_commit_phase::preinstall_accepted,
                                    tid_unique_);
    }
#endif

    // A remote/read-set maximum may have raised the chosen timestamp above
    // this coordinator's local ticket. Floor the next-to-return clock before
    // either phase-3 layout installs the write set.
    if (nwriteset)
        observe_mako_timestamp(tid_unique_);

    //phase3
#if STO_SORT_WRITESET
    for (unsigned tidx = first_write_; tidx != tset_size_; ++tidx) {
        it = &tset_[tidx / tset_chunk][tidx % tset_chunk];
        if (it->has_write()) {
            TXP_INCREMENT(txp_total_w);
            it->owner()->install(*it, *this);
#if defined(MAKO_LOCAL_TEST_HOOKS)
            ++installed_write_count;
            if (installed_write_count == 1 && nwriteset > 1 &&
                has_test_commit_observer) {
                notify_test_commit_observer(
                    test_commit_phase::first_write_installed, tid_unique_);
            }
#endif
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
            it->owner()->install(*it, *this);
#if defined(MAKO_LOCAL_TEST_HOOKS)
            ++installed_write_count;
            if (installed_write_count == 1 && nwriteset > 1 &&
                has_test_commit_observer) {
                notify_test_commit_observer(
                    test_commit_phase::first_write_installed, tid_unique_);
            }
#endif
        }
        if (TThread::writeset_shard_bits > 0||TThread::readset_shard_bits>0) {
            if (TThread::sclient == nullptr) {
                if (!no_paxos) {
                    Warning("Missing ShardClient for remoteInstall in paxos path; aborting transaction");
                    goto abort;
                }
            } else {
#if defined(FAIL_NEW_VERSION)
            int retry_c = 0;
            while (1) {
                try {
                    retry_c += 1;
                    TThread::sclient->remoteInstall(tid_unique_);
                    break;
                } catch (int n) {
			break;
                    if (n==1002) { 
                        // There is a timeout on partial INSTALL, we retry instead of abort for correctness.
                        // Mako can't solve "blocking" issue in 2PC.
                        //std::cout<<"timeout in remoteInstall; retry attempts: " << retry_c <<std::endl;
                        if (!TThread::sclient->isBlocking) {
                            break;
                        }
                    }
                }
            }
#else
            TThread::sclient->remoteInstall(tid_unique_);
#endif
            }
        }
    }
#endif

#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (nwriteset != 0 && has_test_commit_observer) {
        notify_test_commit_observer(test_commit_phase::all_writes_installed,
                                    tid_unique_);
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

    stop(true, writeset, nwriteset);
    // if (TThread::writeset_shard_bits > 0) {
    //     TThread::sclient->remoteUnLock();
    // }
    return true;

abort:
    TXP_INCREMENT(txp_commit_time_aborts);
    stop(false, nullptr, 0);
    // On an ordinary abort, keep the gate until stop has released every write
    // lock. If stop unwinds, the scope destructor still retires the turn so a
    // quarantined worker cannot strand the database-wide record pipeline.
    validation_gate_scope.release();
    if ((TThread::writeset_shard_bits > 0 || TThread::readset_shard_bits > 0) && TThread::sclient != nullptr) {
        TThread::sclient->remoteAbort();
    }
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
            assert(vvx->length() > mako::EXTRA_BITS_FOR_VALUE);
            len_of_V = vvx->length() - mako::EXTRA_BITS_FOR_VALUE;
            memcpy(array + w, (char *) &len_of_V, sizeof(unsigned short));
            w += sizeof(unsigned short);

            memcpy(array + w, (char *) vvx->data(), len_of_V);
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
        std::string val;
        value->copy_value_atomic(val);
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
