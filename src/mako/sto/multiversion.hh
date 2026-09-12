#pragma once
#include <map>
#include "lib/common.h"
#include "sto/version_chain.h"
#include <vector>
#include "sto/sync_util.hh"
#include "sto/common.hh"
#include <cstdint>
#include <limits>
#include <new>
#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

class retired_value_chain_rcu_callback final : public threadinfo::mrcu_callback {
public:
    static retired_value_chain_rcu_callback* make(
            char* head, size_t head_size, const char* embedded_data,
            threadinfo& ti) {
        if (head == nullptr || head == embedded_data) {
            return nullptr;
        }
        void* const storage = ti.allocate(
            sizeof(retired_value_chain_rcu_callback), memtag_masstree_gc);
        if (storage == nullptr) {
            Panic("failed to allocate a retired value-chain callback");
        }
        return new (storage) retired_value_chain_rcu_callback(
            head, head_size, reinterpret_cast<uintptr_t>(embedded_data));
    }

    void retire(threadinfo& ti) {
        ti.rcu_register(this);
    }

    void operator()(threadinfo& ti) noexcept override {
        mako::free_retired_value_chain(
            head_, head_size_, embedded_address_);
        this->~retired_value_chain_rcu_callback();
        ti.deallocate(this, sizeof(*this), memtag_masstree_gc);
    }

private:
    retired_value_chain_rcu_callback(
            char* head, size_t head_size, uintptr_t embedded_address)
        : head_(head), head_size_(head_size),
          embedded_address_(embedded_address) {
    }

    char* head_;
    size_t head_size_;
    uintptr_t embedded_address_;
};

// value field composition: data + mako::BITS_OF_TT (timestamp + term) + mako::BITS_OF_NODE
class MultiVersionValue {
public:
    static bool validPackedSize(size_t size, bool multiversion) {
        if (size < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE)) {
            return false;
        }
        if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        if (!versioned_str::valid_initial_value_size(size)) {
            return false;
        }
        return !multiversion ||
            size <= static_cast<size_t>(std::numeric_limits<int16_t>::max());
    }

    static bool isDeleted(std::string& v) {
        // for non-deleted value, the length of value at least 2+mako::EXTRA_BITS_FOR_VALUE
        return v.length() == 1+mako::EXTRA_BITS_FOR_VALUE && v[0] == 'B';
    }

    template <typename ValueType>
    static std::vector<string> getAllVersion(string val) {
        std::vector<string> ret;

        std::string tmp;
        tmp.assign(val.data(),val.length());
        ret.push_back(isDeleted(val)? "DEL": (tmp));

        // fast peek
        const char *header = mako::value_node_address(
            val.data(), val.length());
        int16_t data_size = mako::load_node_data_size(header);
        while (data_size > 0) {
            char *data = mako::load_node_data(header);
            val.assign(data, static_cast<int>(data_size)); // rewrite with next block value
            std::string tmp;
            tmp.assign(val.data(),val.length());
            ret.push_back(isDeleted(val)? "DEL": (tmp));
            header = mako::value_node_address(val.data(), val.length());
            data_size = mako::load_node_data_size(header);
        }
        return ret;
    }

    // Copy the retained prefix before publishing a new head. The old chain is
    // left byte-for-byte unchanged, so readers that sampled it can finish
    // under Masstree RCU while the replacement becomes visible.
    static mako::value_chain_prune_result pruneBeforePublication(
            char* head, size_t head_size) {
        if (!TThread::should_reclaim()) return {false, 0};
        
        const uint32_t watermark =
            sync_util::sync_logger::retrieveShardW() / 10;
        if (watermark == 0) return {false, 0};

        return mako::cow_prune_value_chain(head, head_size, watermark);
    }

    static void retirePublishedValue(versioned_str_struct* owner,
                                     threadinfo& ti) {
        const auto old_value = owner->snapshot();
        retired_value_chain_rcu_callback* retired =
            retired_value_chain_rcu_callback::make(
                old_value.data, old_value.size, owner->embedded_data(), ti);
        if (retired != nullptr) {
            retired->retire(ti);
        }
    }

    // Replace a value without mutating bytes visible through the previously
    // published pointer. This is used for single-version commits and for
    // repeated writes to a newly inserted row. Both cases still have
    // concurrent Masstree readers, even though OCC will reject their attempt.
    static void publishImmutableValue(const string& newval,
                                      versioned_str_struct* owner,
                                      threadinfo& ti,
                                      bool reset_single_version_metadata) {
        if (!validPackedSize(newval.size(), false)) {
            Panic("invalid packed value size");
        }

        char* replacement = static_cast<char*>(std::malloc(newval.size()));
        if (replacement == nullptr) {
            Panic("failed to allocate an immutable value");
        }
        std::memcpy(replacement, newval.data(), newval.size());
        if (reset_single_version_metadata) {
            mako::store_value_node_timestamp(
                replacement, newval.size(), 0);
            mako::store_value_node_data_size(
                replacement, newval.size(), 0);
            mako::store_value_node_data(
                replacement, newval.size(), nullptr);
        }

        const auto old_value = owner->snapshot();
        retired_value_chain_rcu_callback* retired =
            retired_value_chain_rcu_callback::make(
                old_value.data, old_value.size, owner->embedded_data(), ti);
        owner->publish_value(replacement, static_cast<int>(newval.size()));
        if (retired != nullptr) {
            retired->retire(ti);
        }
    }

    static bool mvGET(string& val,
                      uint8_t current_term,
                      std::unordered_map<int, uint32_t> hist_timestamp) {
        uint32_t time_term = mako::load_value_time_term(
            val.data(), val.length());

        if (likely(time_term % 10 == current_term)) { // current term: get the latest value but reclaim the all version below the watermark within the current term
            return !isDeleted(val);
        } else { // past term e
            char *header = mako::value_node_address(val.data(), val.length());
            
#if defined(FAIL_NEW_VERSION)
            // It's possible that hist_timestamp is not updated yet, and return it directly; and the remote server would do a check
            if  (hist_timestamp.find(time_term % 10)==hist_timestamp.end()) {
                return !isDeleted(val);
            }
            // check if the stored value is below the cached watermark
            if (sync_util::sync_logger::safety_check(
                    mako::load_node_timestamp(header),
                    hist_timestamp[time_term % 10])) { // Single timestamp check
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getGlobalPartitionID(),time_term%10,current_term, hist_timestamp[time_term % 10],val.length());
                    //mako::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (mako::load_node_data_size(header) > 0) {
                const int16_t data_size = mako::load_node_data_size(header);
                char *data = mako::load_node_data(header);
                time_term = mako::load_value_time_term(data, data_size);
                if (sync_util::sync_logger::safety_check(
                        mako::load_node_timestamp(header),
                        hist_timestamp[time_term % 10])) { // Single timestamp check
                    val.assign(data, static_cast<int>(data_size)); // rewrite val with next block value
                    header = mako::value_node_address(val.data(), val.length());
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = mako::value_node_address(data, data_size);
            }
        }
#else
            if (mako::load_node_timestamp(header) / 10 <=
                hist_timestamp[time_term % 10]) { // Single timestamp check
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getGlobalPartitionID(),time_term%10,current_term, hist_timestamp[time_term % 10],val.length());
                    //mako::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (mako::load_node_data_size(header) > 0) {
                const int16_t data_size = mako::load_node_data_size(header);
                char *data = mako::load_node_data(header);
                time_term = mako::load_value_time_term(data, data_size);
                if (mako::load_node_timestamp(header) / 10 <=
                    hist_timestamp[time_term % 10]) { // Single timestamp check
                    val.assign(data, static_cast<int>(data_size)); // rewrite val with next block value
                    header = mako::value_node_address(val.data(), val.length());
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = mako::value_node_address(data, data_size);
            }
        }
#endif
        return true;
    }

    // kvthread.hh -> it's same as malloc vs free
    // one way to solve it: include "rcu.h"
    static void mvInstall(bool isInsert,
                          bool isDelete,
                          const string newval,  // the new value to be updated
                          versioned_str_struct* e, /* versioned_value */
                          uint8_t current_term,
                          threadinfo& ti) {
        (void)isDelete;
        (void)current_term;
        // Single timestamp system
        const auto old_value = e->snapshot();
        char* const oldval_str = old_value.data;
        if (!validPackedSize(old_value.size, true)) {
            Panic("multi-version value exceeds the packed chain limit");
        }
        const int oldval_len = static_cast<int>(old_value.size);
        uint32_t time_term = TThread::txn->tid_unique_ * 10 + TThread::txn->current_term_;
        if (isInsert) { // insert
            // Set single timestamp
            mako::store_value_node_timestamp(
                oldval_str, oldval_len, TThread::txn->tid_unique_);
            mako::store_value_node_data_size(
                oldval_str, oldval_len, 0);  // indicate no next block
            mako::store_value_time_term(oldval_str, oldval_len, time_term);
        } else {  // update or delete
            if (!validPackedSize(newval.length(), true)) {
                Panic("multi-version value exceeds the packed chain limit");
            }
            char* new_vv = (char*)malloc(newval.length());
            if (new_vv == nullptr) {
                Panic("failed to allocate a multi-version value");
            }
            memcpy(new_vv, newval.data(), newval.length()-mako::EXTRA_BITS_FOR_VALUE);
            mako::initialize_value_metadata(new_vv, newval.length());
            mako::store_value_time_term(new_vv, newval.length(), time_term);
            char *header = mako::value_node_address(new_vv, newval.length());
            // Set single timestamp
            mako::store_node_timestamp(header, TThread::txn->tid_unique_);
            mako::store_node_data_size(
                header, static_cast<int16_t>(oldval_len));
            mako::store_node_data(header, oldval_str);
            const auto prune =
                pruneBeforePublication(new_vv, newval.length());
            retired_value_chain_rcu_callback* retired = nullptr;
            if (prune.pruned) {
                retired = retired_value_chain_rcu_callback::make(
                    oldval_str, old_value.size, e->embedded_data(), ti);
            }
            e->publish_value(new_vv, static_cast<int>(newval.length()));
            if (retired != nullptr) {
                retired->retire(ti);
            }
        }
        return ;
    }
} ;
