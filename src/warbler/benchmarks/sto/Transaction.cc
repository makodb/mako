#include "Transaction.hh"
#include <typeinfo>
#include "MassTrans.hh"
#include "deptran/s_main.h"
#include "benchmarks/sto/sync_util.hh"
#include "lib/common.h"

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
__thread srolis::ShardClient *TThread::sclient = nullptr;
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

    // don't bother epoch'ing til things have picked up
    usleep(100000);
    while (global_epochs.run) {
        epoch_type g = global_epochs.global_epoch;
        epoch_type e = g;
        for (auto& t : tinfo) {
            if (t.epoch != 0 && signed_epoch_type(t.epoch - e) < 0)
                e = t.epoch;
        }
        global_epochs.global_epoch = std::max(g + 1, epoch_type(1));
        global_epochs.active_epoch = e;
        global_epochs.recent_tid = Transaction::_TID;

        if (epoch_advance_callback)
            epoch_advance_callback(global_epochs.global_epoch);

        usleep(100000);
    }
    fetch_and_add(&num_epoch_advancers, -1);
    return NULL;
}

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
    start_tid_ = _TID;
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
    // TODO: this will probably mess up with nested transactions
    threadinfo_t& thr = tinfo[TThread::id()];
    if (thr.trans_end_callback)
        thr.trans_end_callback();
    // XXX should reset trans_end_callback after calling it...
    state_ = s_aborted + committed;
}

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

void Transaction::shard_serialize_util(std::vector<uint32_t> vectorT) {
#if defined(PAXOS_LIB_ENABLED)
    #if defined(SIMPLE_WORKLOAD)
        int small_batch_num=2;
    #else
        int small_batch_num=100;
    #endif
    serialize_util(1 /* anything > 0 */, true, MAX_ARRAY_SIZE_IN_BYTES_SMALL, small_batch_num, vectorT);
#endif
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

void Transaction::shard_install(std::vector<uint32_t> vectorT) {
    assert(TThread::id() == threadid_);

    for (int i=0; i<SHARDS; i++) {
        TThread::txn->timestampsReadSet[i] = vectorT[i];
    }
    tid_unique_ = vectorT[TThread::get_shard_index()];

    if (MERGE_KEYS_GROUPS != SHARDS) {
        int delta = tid_unique_ - sync_util::sync_logger::local_replica_id;
        if (delta > 0) {
            __sync_fetch_and_add(&sync_util::sync_logger::local_replica_id, delta);
        }
    }


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
    std::vector<uint32_t> watermarkTimestamp(TThread::get_nshards(),0);
    writeset[0] = tset_size_;

    //phase1
    TransItem* it = nullptr;

    std::vector<int> dummy_table_id_batch;
    std::vector<std::string> key_batch;
    std::vector<std::string> value_batch;

    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_dummy();
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
            dummy_table_id_batch.push_back(it->owner()->get_table_id());
            key_batch.push_back(key);
            value_batch.push_back(val);
        }
    }

    int ret = TThread::sclient->remoteBatchLock(dummy_table_id_batch, key_batch, value_batch);
    if (ret > 0) {
        goto abort;
    }

    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_dummy();
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
#if MERGE_KEYS_GROUPS > SHARDS
        //if (!TThread::isRemoteShard && !TThread::isHomeWarehouse) usleep(15);
#endif
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

    if (!no_paxos){
        updateVectorizedTimestamp(vectorTimestamp); // actual timestamp, not *10
        // merge the vs from the read set;
        for (int i=0;i<TThread::get_nshards();i++) {
            vectorTimestamp[i] = vectorTimestamp[i]>timestampsReadSet[i]?vectorTimestamp[i]:timestampsReadSet[i];
        }

        int cnt_per_group = max(1,SHARDS / MERGE_KEYS_GROUPS);
        if (MERGE_KEYS_GROUPS != SHARDS) {
            for (int i=0; i<SHARDS; i++) {
                if (i % cnt_per_group != 0)
                    vectorTimestamp[i] = max(vectorTimestamp[i],vectorTimestamp[i-1]);
            }

            for (int i=0; i<SHARDS; i++) {
                vectorTimestamp[i] = vectorTimestamp[cnt_per_group * ((i / cnt_per_group) + 1) - 1];
            }
            tid_unique_ = vectorTimestamp[TThread::get_shard_index()];
        }

#if defined(TRACKING_ROLLBACK)
        if (get_current_term()==0) {
            rollbacks_tracker[srolis::getCurrentTimeMillis()].push_back(vectorTimestamp[0]);
        }
#endif
    }

    //phase2
    for (unsigned tidx = 0; tidx != tset_size_; ++tidx) {
        it = (tidx % tset_chunk ? it + 1 : tset_[tidx / tset_chunk]);
        bool isRemote = it->owner()->get_is_dummy();
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
        int ret=TThread::sclient->remoteValidate(watermarkTimestamp);
        for (int i=0;i<TThread::get_nshards();i++) {
            if(watermarkTimestamp[i]>0) {
                sync_util::sync_logger::vectorized_w_[i]=watermarkTimestamp[i];
            }
        }
        if (ret > 0) {
            goto abort;
        }
    }

#if MERGE_KEYS_GROUPS > SHARDS
        //if (!TThread::isRemoteShard && !TThread::isHomeWarehouse) usleep(15);
#endif

    //phase3
#if STO_SORT_WRITESET
    for (unsigned tidx = first_write_; tidx != tset_size_; ++tidx) {
        it = &tset_[tidx / tset_chunk][tidx % tset_chunk];
        if (it->has_write()) {
            TXP_INCREMENT(txp_total_w);
            it->owner()->install(*it, *this);
        }
    }
#else
    if (nwriteset) {
        auto writeset_end = writeset + nwriteset;

        if (MERGE_KEYS_GROUPS != SHARDS) {
            int delta = tid_unique_ - sync_util::sync_logger::local_replica_id;
            if (delta > 0) {
                __sync_fetch_and_add(&sync_util::sync_logger::local_replica_id, delta);
            }
        }

        for (auto idxit = writeset; idxit != writeset_end; ++idxit) {
            if (likely(*idxit < tset_initial_capacity))
                it = &tset0_[*idxit];
            else
                it = &tset_[*idxit / tset_chunk][*idxit % tset_chunk];
            TXP_INCREMENT(txp_total_w);
            // to ensure invalid-bit to be reset in transPut for dummy tables on the coordinator shard
            it->owner()->install(*it, *this);
        }
        if (TThread::writeset_shard_bits > 0||TThread::readset_shard_bits>0) {
#if defined(FAIL_NEW_VERSION)
            int retry_c = 0;
            while (1) {
                try {
                    retry_c += 1;
                    TThread::sclient->remoteInstall(vectorTimestamp);
                    break;
                } catch (int n) {
			break;
                    if (n==1002) { 
                        // There is a timeout on partial INSTALL, we retry instead of abort for correctness.
                        // Warbler can't solve "blocking" issue in 2PC.
                        //std::cout<<"timeout in remoteInstall; retry attempts: " << retry_c <<std::endl;
                        if (!TThread::sclient->isBlocking) {
                            break;
                        }
                    }
                }
            }
#else
            TThread::sclient->remoteInstall(vectorTimestamp);
#endif
        }
    }
#endif

#if MERGE_KEYS_GROUPS > SHARDS
        //if (!TThread::isRemoteShard && !TThread::isHomeWarehouse) usleep(15);
#endif

#if defined(PAXOS_LIB_ENABLED)
    if (!no_paxos) {
        #if defined(SIMPLE_WORKLOAD)
            int large_batch_num=5;
        #elif defined(MICRO_BENCHMARK)
            int large_batch_num=3000;
        #else
            int large_batch_num=400;
        #endif
        serialize_util(nwriteset, false, MAX_ARRAY_SIZE_IN_BYTES, large_batch_num, vectorTimestamp);
    }
#endif

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
inline void Transaction::serialize_util(unsigned nwriteset, bool on_remote, int max_bytes_size, int batch_size, std::vector<uint32_t> vectorT) const {
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
    if (vectorT[TThread::get_shard_index()]%1000==0&&TThread::getPartitionID()==4){
        uint32_t cur_time = srolis::getCurrentTimeMillis();
        if (cur_time - start_time>= 5*1000 && cur_time - start_time <= 15*1000){ // time duration: [5,15]
            sample_transaction_tracker[vectorT[TThread::get_shard_index()]] = srolis::getCurrentTimeMillis() ;
        }
    }
#endif

    int epoch = get_current_term();
    for (int i=0; i<TThread::get_nshards(); ++i) {
        uint32_t tmp=epoch+vectorT[i]*10;
        instance->update_commit_id(i, tmp);
    }
    // 1. copy current Commit ID
    memcpy(array + w, &instance->latest_commit_id_v[TThread::get_shard_index()], sizeof(uint32_t));
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
        bool isRemote = it->owner()->get_is_dummy();
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
            assert(vvx->length() > srolis::EXTRA_BITS_FOR_VALUE);
            len_of_V = vvx->length() - srolis::EXTRA_BITS_FOR_VALUE;
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
                assert(vvx.length() > srolis::EXTRA_BITS_FOR_VALUE);
                len_of_V = vvx.length() - srolis::EXTRA_BITS_FOR_VALUE;
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
            Warning("SHOULD NOT BE a zero here");
        }
        memcpy(array + w, (char *) &table_id, sizeof(unsigned short));
        w += sizeof(unsigned short);
    }
    memcpy(array + w_tmp_c - sizeof(unsigned short int), (char *) &_count, sizeof(unsigned short int));
    unsigned int len_of_KV = w - w_tmp;
    memcpy(array + w_tmp - sizeof(unsigned int), (char *) &len_of_KV, sizeof(unsigned int));

    std::string prefix_str = std::to_string(TThread::id());
    instance->update_ptr(w);
    size_t pos = 0;
    unsigned char *queueLog = instance->getLogOnly (pos);
    if(instance->checkPushRequired()) {
      assert(pos <= MAX_ARRAY_SIZE_IN_BYTES) ;
      if(pos!=0) {
          // 7. latest_commit_id: maximum for each shard
          for (int i=0;i<TThread::get_nshards();i++) {
            memcpy (queueLog + pos, &instance->latest_commit_id_v[i], sizeof(uint32_t));
            pos += sizeof(uint32_t);
          }
          
          // 8. tracking purpose, the latency to commit a huge log
          uint32_t st_time = srolis::getCurrentTimeMillis();
          memcpy (queueLog + pos, &st_time, sizeof(uint32_t));
          pos += sizeof(uint32_t);

          instance->update_ptr(pos);
          //int outstanding = get_outstanding_logs(TThread::getPartitionID ()) ;
          //if (outstanding>20){
          //  usleep(10*1000); // wait 1 Paxos log time 
          //}
           
        //   while ((TThread::sclient == NULL) || !TThread::sclient->stopped) {
        //     if (outstanding>20) {
        //         usleep(50);
        //     } else {
        //         break;
        //     }
        //     outstanding = get_outstanding_logs(TThread::getPartitionID ()) ;
        //   }
        //   Warning("outstanding request: %d, par_id: %d", outstanding, TThread::getPartitionID ());
          
          // deal with logs from its corresponding threads
        //   if (TThread::in_loading_phase){
        //     Warning("add a log to nc, par_id:%d,", TThread::getPartitionID());
        //     usleep(10*1000);
        //   }
        if (!on_remote)  // FIX me: merge the logs from helper threads instead of a separate log
          add_log_to_nc((char *)queueLog, pos, TThread::getPartitionID (), batch_size); // the partitionID for the helper thread
      }
      instance->resetMemory();
    }
}

void Transaction::print_stats() {
    if (tset_size_ == 0) return;
    TransItem* it = nullptr;
    if (tset_size_ == 0) return;
    for (unsigned tidx = tset_size_-1; tidx >= 0; --tidx) {
        auto base = tset_[tidx / tset_chunk];
        it = base + tidx % tset_chunk;
        versioned_str_struct *value = (*it).key<versioned_str_struct *>();
        std::string val = std::string(value->data(), value->length());
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

const char* Transaction::state_name(int state) {
    static const char* names[] = {"in-progress", "opacity-check", "committing", "committing-locked", "aborted", "committed"};
    if (unsigned(state) < arraysize(names))
        return names[state];
    else
        return "unknown-state";
}

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

void Transaction::print() const {
    print(std::cerr);
}

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

bool TObject::get_is_dummy() const {
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
