#include "Transaction.hh"
#include <typeinfo>
#include <atomic>
#include <array>
#include <iostream>
#include "MassTrans.hh"
#include "deptran/s_main.h"
#include "benchmarks/sto/sync_util.hh"
#include "lib/common.h"
#include "benchmarks/benchmark_config.h"

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

std::function<int()> callback_ = nullptr;
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
__thread int TThread::the_num_erpc_server;
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
#if defined(SIMPLE_WORKLOAD)
TransactionTid::type __attribute__((aligned(128))) Transaction::_TID = 1;
#else
TransactionTid::type __attribute__((aligned(128))) Transaction::_TID = 2 * TransactionTid::increment_value;
#endif
   // reserve TransactionTid::increment_value for prepopulated

static void __attribute__((used)) check_static_assertions() {
    static_assert(sizeof(threadinfo_t) % 128 == 0, "threadinfo is 2-cache-line aligned");
}

void Transaction::initialize() {
    static_assert(tset_initial_capacity % tset_chunk == 0, "tset_initial_capacity not an even multiple of tset_chunk");
    hash_base_ = TransactionConfig::HASH_BASE_RESET_THRESHOLD;
    tset_size_ = 0;
    lrng_state_ = TransactionConfig::DEFAULT_RANDOM_SEED;
    for (unsigned chunk_index = 0; chunk_index != tset_initial_capacity / tset_chunk; ++chunk_index)
        tset_[chunk_index] = &tset0_[chunk_index * tset_chunk];
    for (unsigned chunk_index = tset_initial_capacity / tset_chunk; chunk_index != arraysize(tset_); ++chunk_index)
        tset_[chunk_index] = nullptr;
}

Transaction::~Transaction() {
    if (in_progress())
        silent_abort();
    TransItem* live_chunk = tset0_;
    for (unsigned chunk_index = 0; chunk_index != arraysize(tset_); ++chunk_index, live_chunk += tset_chunk)
        if (live_chunk != tset_[chunk_index])
            delete[] tset_[chunk_index];
}

void Transaction::refresh_tset_chunk() {
    assert(tset_size_ % tset_chunk == 0);
    assert(tset_size_ < tset_max_capacity);
    if (!tset_[tset_size_ / tset_chunk])
        tset_[tset_size_ / tset_chunk] = new TransItem[tset_chunk];
    tset_next_ = tset_[tset_size_ / tset_chunk];
}

void* Transaction::epoch_advancer(void*) {
    static int num_epoch_advancers = 0;
    if (fetch_and_add(&num_epoch_advancers, 1) != 0)
        std::cerr << "WARNING: more than one epoch_advancer thread\n";

    // Wait for system initialization before starting epoch advancement
    usleep(TransactionConfig::EPOCH_ADVANCE_DELAY_MICROSECONDS);
    while (global_epochs.run) {
        epoch_type current_global_epoch = global_epochs.global_epoch;
        epoch_type minimum_active_epoch = current_global_epoch;
        for (auto& thread_info : tinfo) {
            if (thread_info.epoch != 0 && signed_epoch_type(thread_info.epoch - minimum_active_epoch) < 0)
                minimum_active_epoch = thread_info.epoch;
        }
        global_epochs.global_epoch = std::max(current_global_epoch + 1, epoch_type(1));
        global_epochs.active_epoch = minimum_active_epoch;
        global_epochs.recent_tid = Transaction::_TID;

        if (epoch_advance_callback)
            epoch_advance_callback(global_epochs.global_epoch);

        usleep(TransactionConfig::EPOCH_ADVANCE_DELAY_MICROSECONDS);
    }
    fetch_and_add(&num_epoch_advancers, -1);
    return nullptr;
}

bool Transaction::preceding_duplicate_read(TransItem* needle) const {
    const TransItem* current_item = nullptr;
    for (unsigned transaction_index = 0; ; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        if (current_item == needle)
            return false;
        if (current_item->owner() == needle->owner() && current_item->key_ == needle->key_
            && current_item->has_read())
            return true;
    }
}

void Transaction::hard_check_opacity(TransItem* item, TransactionTid::type transaction_id) {
    // ignore opacity checks during commit; we're in the middle of checking
    // things anyway
    if (state_ == s_committing || state_ == s_committing_locked)
        return;

    // ignore if version hasn't changed
    if (item && item->has_read() && item->read_value<TransactionTid::type>() == transaction_id)
        return;

    // die on recursive opacity check; this is only possible for predicates
    if (unlikely(state_ == s_opacity_check)) {
        mark_abort_because(item, "recursive opacity check", transaction_id);
    abort:
        TXP_INCREMENT(txp_hco_abort);
        abort();
    }
    assert(state_ == s_in_progress);

    TXP_INCREMENT(txp_hco);
    if (TransactionTid::is_locked_elsewhere(transaction_id, threadid_)) {
        TXP_INCREMENT(txp_hco_lock);
        mark_abort_because(item, "locked", transaction_id);
        goto abort;
    }
    if (transaction_id & TransactionTid::nonopaque_bit)
        TXP_INCREMENT(txp_hco_invalid);

    state_ = s_opacity_check;
    start_tid_ = _TID;
    release_fence();
    TransItem* current_item = nullptr;
    for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        if (current_item->has_read()) {
            TXP_INCREMENT(txp_total_check_read);
            if (!current_item->owner()->check(*current_item, *this)
                && (!may_duplicate_items_ || !preceding_duplicate_read(current_item))) {
                mark_abort_because(item, "opacity check");
                goto abort;
            }
        } else if (current_item->has_predicate()) {
            TXP_INCREMENT(txp_total_check_predicate);
            if (!current_item->owner()->check_predicate(*current_item, *this, false)) {
                mark_abort_because(item, "opacity check_predicate");
                goto abort;
            }
        }
    }
    state_ = s_in_progress;
}

void Transaction::stop(bool committed, unsigned* writeset, unsigned nwriteset) {
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
            it = &tset_[tset_size_ / tset_chunk][tset_size_ % tset_chunk];
            for (unsigned tidx = tset_size_; tidx != first_write_; --tidx) {
                it = (tidx % tset_chunk ? it - 1 : &tset_[(tidx - 1) / tset_chunk][tset_chunk - 1]);
                if (it->needs_unlock())
                    it->owner()->unlock(*it);
            }
        }
        it = &tset_[tset_size_ / tset_chunk][tset_size_ % tset_chunk];
        for (unsigned tidx = tset_size_; tidx != first_write_; --tidx) {
            it = (tidx % tset_chunk ? it - 1 : &tset_[(tidx - 1) / tset_chunk][tset_chunk - 1]);
            if (it->has_write())
                it->owner()->cleanup(*it, committed);
        }
    }

after_unlock:
    // Execute transaction end callback and clean up thread state
    threadinfo_t& thread_info = tinfo[TThread::id()];
    if (thread_info.trans_end_callback) {
        thread_info.trans_end_callback();
        // Reset callback after execution to prevent accidental reuse
        thread_info.trans_end_callback = nullptr;
    }
    state_ = s_aborted + committed;
}

bool Transaction::shard_try_lock_last_writeset() {
    assert(TThread::id() == threadid_);

    // find the last TransItem with write operation
    TransItem* current_item = nullptr;
    if (tset_size_ == 0) return true;
    for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index) {
        auto chunk_base = tset_[transaction_index / tset_chunk];
        current_item = chunk_base + transaction_index % tset_chunk;
        if (current_item->has_write()) {
            if (!current_item->owner()->lock(*current_item, *this)) {
                return false;
            }
            current_item->__or_flags(TransItem::lock_bit);
            break;
        }
        if (transaction_index == 0) break;
    }
    return true;
}

int Transaction::shard_validate() {
    assert(TThread::id() == threadid_);

    TransItem* current_item = nullptr;
    if (tset_size_ == 0) return 0;
    for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index) {
        auto chunk_base = tset_[transaction_index / tset_chunk];
        current_item = chunk_base + transaction_index % tset_chunk;
        if (current_item->has_read()) {
            if (!current_item->owner()->check(*current_item, *this)
                && (!may_duplicate_items_ || !preceding_duplicate_read(current_item))) {
                return 1;
            }
        }
        if (transaction_index == 0) break;
    }
    return 0;
}

void Transaction::shard_serialize_util(uint32_t timestamp) {
    if (!BenchmarkConfig::getInstance().getIsReplicated()) {return;}
    
    constexpr int SIMPLE_WORKLOAD_BATCH_SIZE = 2;
    constexpr int NORMAL_WORKLOAD_BATCH_SIZE = 100;
    
    #if defined(SIMPLE_WORKLOAD)
        int small_batch_size = SIMPLE_WORKLOAD_BATCH_SIZE;
    #else
        int small_batch_size = NORMAL_WORKLOAD_BATCH_SIZE;
    #endif
    serialize_util(1 /* anything > 0 */, true, MAX_ARRAY_SIZE_IN_BYTES_SMALL, small_batch_size, timestamp);
}

uint8_t Transaction::get_current_term() const {
    if(callback_){
        if(!current_term_)
            current_term_ = callback_();
    }else{
        current_term_ = 0;
    }
    return current_term_;
}

void Transaction::shard_install(uint32_t timestamp) {
    assert(TThread::id() == threadid_);

    // Update max timestamp from readset
    TThread::txn->maxTimestampReadSet = MAX(TThread::txn->maxTimestampReadSet, timestamp);
    tid_unique_ = timestamp;

    // Update local_id to catch up with single timestamp
    int timestamp_delta = tid_unique_ - sync_util::sync_logger::local_replica_id;
    if (timestamp_delta > 0) {
        __sync_fetch_and_add(&sync_util::sync_logger::local_replica_id, timestamp_delta);
    }

    TransItem* current_item = nullptr;
    if (tset_size_ == 0) return;
    for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index) {
        auto chunk_base = tset_[transaction_index / tset_chunk];
        current_item = chunk_base + transaction_index % tset_chunk;
        if (current_item->has_write()) {
            current_item->owner()->install(*current_item, *this);
        }
        if (transaction_index == 0) break;
    }
}

void Transaction::shard_unlock(bool is_committed) {
    assert(TThread::id() == threadid_);

    TransItem* current_item = nullptr;
    if (tset_size_ == 0) return;
    
    // First pass: unlock all items that need unlocking
    for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index) {
        auto chunk_base = tset_[transaction_index / tset_chunk];
        current_item = chunk_base + transaction_index % tset_chunk;
        if (current_item->needs_unlock()) {
            current_item->owner()->unlock(*current_item);
        }
        if (transaction_index == 0) break;
    }
    
    // Second pass: cleanup all write items
    for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index) {
        auto chunk_base = tset_[transaction_index / tset_chunk];
        current_item = chunk_base + transaction_index % tset_chunk;
        if (current_item->has_write()) {
            current_item->owner()->cleanup(*current_item, is_committed);
        }
        if (transaction_index == 0) break;
    }
}

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

    unsigned writeset[tset_size_];
    unsigned nwriteset = 0;
    // Single watermark timestamp instead of vector
    uint32_t watermarkTimestamp = 0;
    writeset[0] = tset_size_;

    //phase1: Collect remote write operations for batch locking
    TransItem* current_item = nullptr;

    std::vector<int> remote_table_id_batch;
    std::vector<std::string> key_batch;
    std::vector<std::string> value_batch;

    for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        bool is_remote_operation = current_item->owner()->get_is_remote();
        if (current_item->has_write() && is_remote_operation) {
            std::string operation_key = "", operation_value = "";
            if (hasInsertOp(current_item)) {  // key_write_value_type
                operation_key = (*current_item).write_value<std::string>();
                versioned_str_struct *versioned_value = (*current_item).key<versioned_str_struct *>();
                operation_value = std::string(versioned_value->data(), versioned_value->length());
            } else {
                operation_key = current_item->extra;
                operation_value = (*current_item).template write_value<std::string>();
            }
            remote_table_id_batch.push_back(current_item->owner()->get_table_id());
            key_batch.push_back(operation_key);
            value_batch.push_back(operation_value);
        }
    }

    int remote_lock_result = TThread::sclient->remoteBatchLock(remote_table_id_batch, key_batch, value_batch);
    if (remote_lock_result > 0) {
        goto abort;
    }

    for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        bool is_remote_operation = current_item->owner()->get_is_remote();
        if (current_item->has_write()) {
            writeset[nwriteset++] = transaction_index;
#if !STO_SORT_WRITESET
            // Lock items immediately when not sorting writeset
            if (nwriteset >= 1) {
                first_write_ = writeset[0];
                state_ = s_committing_locked;
            }
            if (!current_item->owner()->lock(*current_item, *this)) {
                mark_abort_because(current_item, "commit lock");
                goto abort;
            }
            current_item->__or_flags(TransItem::lock_bit);
#endif
        }
        if (current_item->has_read()) {
            TXP_INCREMENT(txp_total_r);
        }
        else if (current_item->has_predicate()) {
            TXP_INCREMENT(txp_total_check_predicate);
            if (!current_item->owner()->check_predicate(*current_item, *this, true)) {
                mark_abort_because(current_item, "commit check_predicate");
                goto abort;
            }
        }
    }

    first_write_ = writeset[0];

#if STO_SORT_WRITESET
    std::sort(writeset, writeset + nwriteset, [&] (unsigned index_i, unsigned index_j) {
        TransItem* item_i = &tset_[index_i / tset_chunk][index_i % tset_chunk];
        TransItem* item_j = &tset_[index_j / tset_chunk][index_j % tset_chunk];
        return *item_i < *item_j;
    });

    if (nwriteset) {
        state_ = s_committing_locked;
        auto writeset_end = writeset + nwriteset;
        for (auto writeset_iterator = writeset; writeset_iterator != writeset_end; ) {
            TransItem* write_item = &tset_[*writeset_iterator / tset_chunk][*writeset_iterator % tset_chunk];
            if (!write_item->owner()->lock(*write_item, *this)) {
                mark_abort_because(write_item, "commit lock");
                goto abort;
            }
            write_item->__or_flags(TransItem::lock_bit);
            ++writeset_iterator;
        }
    }
#endif


#if CONSISTENCY_CHECK
    fence();
    commit_tid();
    fence();
#endif

    if (!no_paxos){
        // Update single timestamp system
        updateSingleTimestamp(); // Updates tid_unique_ internally
        // Merge with max timestamp from read set
        if (maxTimestampReadSet > tid_unique_) {
            tid_unique_ = maxTimestampReadSet;
        }

#if defined(TRACKING_ROLLBACK)
        if (get_current_term()==0) {
            rollbacks_tracker[mako::getCurrentTimeMillis()].push_back(tid_unique_);
        }
#endif
    }

    //phase2: Validate local read operations
    for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        bool is_remote_operation = current_item->owner()->get_is_remote();
        if (!is_remote_operation && current_item->has_read()) {
            TXP_INCREMENT(txp_total_check_read);
            if (!current_item->owner()->check(*current_item, *this) // this is just a version check
                && (!may_duplicate_items_ || !preceding_duplicate_read(current_item))) {
                mark_abort_because(current_item, "commit check");
                goto abort;
            }
        }
    }

    if (TThread::readset_shard_bits > 0) {
        // Single timestamp system: pass and receive single watermark
        int validation_result = TThread::sclient->remoteValidate(watermarkTimestamp);
        uint32_t current_watermark = sync_util::sync_logger::single_watermark_.load(memory_order_acquire);
        if(watermarkTimestamp > current_watermark) {
            // Update single watermark
            sync_util::sync_logger::single_watermark_.store(watermarkTimestamp, memory_order_release);
        }
        if (validation_result > 0) {
            goto abort;
        }
    }

    //phase3: Install write operations
#if STO_SORT_WRITESET
    for (unsigned transaction_index = first_write_; transaction_index != tset_size_; ++transaction_index) {
        TransItem* write_item = &tset_[transaction_index / tset_chunk][transaction_index % tset_chunk];
        if (write_item->has_write()) {
            TXP_INCREMENT(txp_total_w);
            write_item->owner()->install(*write_item, *this);
        }
    }
#else
    if (nwriteset) {
        auto writeset_end = writeset + nwriteset;

        // Update local_id to catch up with single timestamp
        int delta = tid_unique_ - sync_util::sync_logger::local_replica_id;
        if (delta > 0) {
            __sync_fetch_and_add(&sync_util::sync_logger::local_replica_id, delta);
        }

        for (auto idxit = writeset; idxit != writeset_end; ++idxit) {
            TransItem* write_item;
            if (likely(*idxit < tset_initial_capacity))
                write_item = &tset0_[*idxit];
            else
                write_item = &tset_[*idxit / tset_chunk][*idxit % tset_chunk];
            TXP_INCREMENT(txp_total_w);
            // to ensure invalid-bit to be reset in transPut for remote tables on the coordinator shard
            write_item->owner()->install(*write_item, *this);
        }
        if (TThread::writeset_shard_bits > 0||TThread::readset_shard_bits>0) {
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
#endif

    if (BenchmarkConfig::getInstance().getIsReplicated()) {
        if (!no_paxos) {
            constexpr int SIMPLE_WORKLOAD_LARGE_BATCH = 5;
            constexpr int NORMAL_WORKLOAD_LARGE_BATCH = 400;
            constexpr int MICRO_WORKLOAD_LARGE_BATCH = 3000;
            
            #if defined(SIMPLE_WORKLOAD)
                int large_batch_size = SIMPLE_WORKLOAD_LARGE_BATCH;
            #else
                int large_batch_size = NORMAL_WORKLOAD_LARGE_BATCH;
            #endif
        
            if (TThread::get_is_micro())
                large_batch_size = MICRO_WORKLOAD_LARGE_BATCH;
            serialize_util(nwriteset, false, MAX_ARRAY_SIZE_IN_BYTES, large_batch_size, tid_unique_);
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
    if (TThread::writeset_shard_bits > 0 || TThread::readset_shard_bits > 0) {
        TThread::sclient->remoteAbort();
    }
    return false;
}

// serialize transactions into log and then sent it out via Paxos
inline void Transaction::serialize_util(unsigned writeset_count, bool is_remote_operation, int max_bytes_size, int batch_size, uint32_t timestamp) const {
    if (writeset_count == 0) return;

    TransItem *current_item = nullptr;
    size_t write_position = 0;
    unsigned char *serialization_array = nullptr;

    static thread_local std::shared_ptr<StringAllocator> allocator_instance = std::shared_ptr<StringAllocator>(
            new StringAllocator(TThread::get_nshards(), max_bytes_size, batch_size));
    serialization_array = allocator_instance->getLogOnly(write_position);

    unsigned short int key_value_pair_count = 0;  // 2bytes, the count of K-V pairs
    unsigned short int current_table_id = 0; // 2 bytes

#if defined(TRACKING_LATENCY)
    constexpr uint32_t LATENCY_SAMPLE_INTERVAL = 1000;
    constexpr uint32_t LATENCY_TRACKING_PARTITION = 4;
    constexpr uint32_t LATENCY_START_TIME_MS = 5 * 1000;
    constexpr uint32_t LATENCY_END_TIME_MS = 15 * 1000;
    
    if (timestamp % LATENCY_SAMPLE_INTERVAL == 0 && TThread::getPartitionID() == LATENCY_TRACKING_PARTITION){
        uint32_t current_time = mako::getCurrentTimeMillis();
        if (current_time - start_time >= LATENCY_START_TIME_MS && current_time - start_time <= LATENCY_END_TIME_MS) {
            sample_transaction_tracker[timestamp] = mako::getCurrentTimeMillis();
        }
    }
#endif

    int current_epoch = get_current_term();
    // Single timestamp system: use same timestamp for all shards
    constexpr uint32_t TIMESTAMP_EPOCH_MULTIPLIER = 10;
    uint32_t combined_timestamp = current_epoch + timestamp * TIMESTAMP_EPOCH_MULTIPLIER;
    // Single timestamp system: no need to loop over shards
    allocator_instance->update_commit_id(combined_timestamp);
    // 1. copy current Commit ID (single timestamp)
    memcpy(serialization_array + write_position, &combined_timestamp, sizeof(uint32_t));
    write_position += sizeof(uint32_t);

    // 2. copy the count of K-V pairs
    write_position += sizeof(unsigned short int);
    size_t key_value_count_position = write_position;

    // 3. defer copying the len of K-V pairs
    write_position += sizeof(unsigned int);
    size_t key_value_length_position = write_position;

    unsigned short key_length = 0;
    unsigned short value_length = 0;

    for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        bool is_remote_item = current_item->owner()->get_is_remote();
        current_table_id = 0x0;
        if (!current_item->has_write() || is_remote_item) {
            continue;
        }
        key_value_pair_count++;

        // 4. copy the length of key and content of key.
        //    Note: we get the key from transItem.write_value, NOT transItem.key!
        //    Check the implementation: MassTrans => trans_write => Sto::new_item(this, val) and add_write
        //    Due to different implementation purposes, we use different ways to retrieve key and value
        std::string operation_key = "";
        if (hasInsertOp(current_item)) {
            operation_key = (*current_item).write_value<std::string>();
        } else {
            operation_key = current_item->extra;
        }
        key_length = operation_key.length();
        if (key_length == 0) {
            std::cout << "Error while reading Key [Slow Exit now]" << std::endl;
            // Note: Commented out exit(1) to prevent abrupt termination
        }

        memcpy(serialization_array + write_position, (char *) &key_length, sizeof(unsigned short));
        write_position += sizeof(unsigned short);

        memcpy(serialization_array + write_position, (char *) operation_key.data(), key_length);
        write_position += key_length;

        // 5. copy the length of value and content of value
        if (hasInsertOp(current_item)) {
            versioned_str_struct *versioned_value = (*current_item).key<versioned_str_struct *>();
            assert(versioned_value->length() > mako::EXTRA_BITS_FOR_VALUE);
            value_length = versioned_value->length() - mako::EXTRA_BITS_FOR_VALUE;
            memcpy(serialization_array + write_position, (char *) &value_length, sizeof(unsigned short));
            write_position += sizeof(unsigned short);

            memcpy(serialization_array + write_position, (char *) versioned_value->data(), value_length);
            write_position += value_length;
        } else {
            std::string operation_value = "";
            if (hasDeleteOp(current_item)){
                operation_value = "B"; // placeholder content for deleted item
                value_length = 1;
            }else{
                operation_value = (*current_item).template write_value<std::string>();
                assert(operation_value.length() > mako::EXTRA_BITS_FOR_VALUE);
                value_length = operation_value.length() - mako::EXTRA_BITS_FOR_VALUE;
            }
            memcpy(serialization_array + write_position, (char *) &value_length, sizeof(unsigned short));
            write_position += sizeof(unsigned short);

            memcpy(serialization_array + write_position, (char *) operation_value.data(), value_length);
            write_position += value_length;
        }

        // 6. copy table id
        current_table_id = current_item->owner()->get_table_id();
        if (hasDeleteOp(current_item)) {  // delete flag
            constexpr unsigned short DELETE_FLAG_MASK = (1 << 15); // Set the highest bit for delete operations
            current_table_id = current_table_id | DELETE_FLAG_MASK;
        }
        if (current_table_id == 0) {
            Warning("table_id can't be zero here");
        }
        memcpy(serialization_array + write_position, (char *) &current_table_id, sizeof(unsigned short));
        write_position += sizeof(unsigned short);
    }
    memcpy(serialization_array + key_value_count_position - sizeof(unsigned short int), (char *) &key_value_pair_count, sizeof(unsigned short int));
    unsigned int total_key_value_length = write_position - key_value_length_position;
    memcpy(serialization_array + key_value_length_position - sizeof(unsigned int), (char *) &total_key_value_length, sizeof(unsigned int));

    allocator_instance->update_ptr(write_position);
    size_t queue_position = 0;
    unsigned char *queue_log_buffer = allocator_instance->getLogOnly(queue_position);
    if(allocator_instance->checkPushRequired()) {
      assert(queue_position <= MAX_ARRAY_SIZE_IN_BYTES);
      if(queue_position != 0) {
          // 7. latest_commit_id: single timestamp*10+term
          memcpy(queue_log_buffer + queue_position, &allocator_instance->latest_commit_timestamp, sizeof(uint32_t));
          queue_position += sizeof(uint32_t);
          
          // 8. tracking purpose, the latency to commit a huge log
          uint32_t start_time_ms = mako::getCurrentTimeMillis();
          memcpy(queue_log_buffer + queue_position, &start_time_ms, sizeof(uint32_t));
          queue_position += sizeof(uint32_t);

          allocator_instance->update_ptr(queue_position);

        // Submit log to network consensus layer
        add_log_to_nc((char *)queue_log_buffer, queue_position, TThread::getPartitionID(), batch_size);

#ifndef DISABLE_DISK
        // Asynchronously persist to RocksDB
        auto& persistence = mako::RocksDBPersistence::getInstance();
        uint32_t shard_id = BenchmarkConfig::getInstance().getShardIndex();

        // Per-partition success/failure counters (max 64 partitions)
        static std::array<std::atomic<uint64_t>, 64> per_partition_success{};
        static std::array<std::atomic<uint64_t>, 64> per_partition_fail{};

        // Capture the timestamp and partition ID for the callback
        uint32_t persist_timestamp = allocator_instance->latest_commit_timestamp;
        int partition_id = TThread::getPartitionID();

        persistence.persistAsync((const char*)queue_log_buffer, queue_position, shard_id, partition_id,
            [persist_timestamp, partition_id](bool success) {
                constexpr uint64_t SUCCESS_LOG_INTERVAL = 100;
                if (success) {
                    // Update disk persistence timestamp for this partition
                    sync_util::sync_logger::updateDiskTimestamp(partition_id, persist_timestamp);

                    uint64_t success_count = per_partition_success[partition_id].fetch_add(1, std::memory_order_relaxed) + 1;
                    // Log every SUCCESS_LOG_INTERVAL successful persists
                    if (success_count % SUCCESS_LOG_INTERVAL == 0) {
                        std::cout << "[RocksDB Helper] par_id=" << partition_id
                                      << ", success=" << success_count
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
      allocator_instance->resetMemory();
    }
}

void Transaction::print_stats() {
    if (tset_size_ == 0) return;
    
    TransItem* current_item = nullptr;
    for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index) {
        auto chunk_base = tset_[transaction_index / tset_chunk];
        current_item = chunk_base + transaction_index % tset_chunk;
        versioned_str_struct *versioned_value = (*current_item).key<versioned_str_struct *>();
        std::string item_value = std::string(versioned_value->data(), versioned_value->length());
        std::string item_key = "";
        if (hasInsertOp(current_item)) {  // key_write_value_type
            item_key = (*current_item).write_value<std::string>();
        } else {
            item_key = current_item->extra;
        }
        Warning("print[obj:%p], has_write: %d, has_read: %d, has_lock: %d, has_insert: %d, has_delete: %d, invalidate: %d, key: %s, value: %s", 
                current_item, 
                current_item->has_write(), 
                current_item->has_read(), 
                current_item->needs_unlock(), 
                hasInsertOp(current_item), 
                hasDeleteOp(current_item), 
                current_item->has_flag(TransactionTid::user_bit), 
                item_key.c_str(), 
                item_value.c_str());
        if (transaction_index == 0) break;
    }
}

const char* Transaction::state_name(int state) {
    static const char* names[] = {"in-progress", "opacity-check", "committing", "committing-locked", "aborted", "committed"};
    if (unsigned(state) < arraysize(names))
        return names[state];
    else
        return "unknown-state";
}

void Transaction::print(std::ostream& output_stream) const {
    output_stream << "T0x" << (void*) this << " " << state_name(state_) << " [";
    const TransItem* current_item = nullptr;
    for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
        current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
        if (transaction_index)
            output_stream << " ";
        current_item->owner()->print(output_stream, *current_item);
    }
    output_stream << "]\n";
}

void Transaction::print() const {
    print(std::cerr);
}

void TObject::print(std::ostream& output_stream, const TransItem& item) const {
    output_stream << "{" << typeid(*this).name() << " " << (void*) this << "." << item.key<void*>();
    if (item.has_read())
        output_stream << " R" << item.read_value<void*>();
    if (item.has_write())
        output_stream << " =" << item.write_value<void*>();
    if (item.has_predicate())
        output_stream << " P" << item.predicate_value<void*>();
    output_stream << "}";
}

unsigned long long int TObject::get_table_id() const {
    unsigned long long int temp = 10012;
    return temp;
}

bool TObject::get_is_remote() const {
    exit(1);
    return false;
}

std::ostream& operator<<(std::ostream& output_stream, const Transaction& transaction) {
    transaction.print(output_stream);
    return output_stream;
}

std::ostream& operator<<(std::ostream& output_stream, const TestTransaction& test_transaction) {
    test_transaction.print(output_stream);
    return output_stream;
}

std::ostream& operator<<(std::ostream& output_stream, const TransactionGuard& transaction_guard) {
    transaction_guard.print(output_stream);
    return output_stream;
}
