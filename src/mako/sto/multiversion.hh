#pragma once
#include <map>
#include "lib/common.h"
#include "sto/version_chain.h"
#include <vector>
#include "sto/sync_util.hh"
#include "sto/common.hh"
#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

// value field composition: data + mako::BITS_OF_TT (timestamp + term) + mako::BITS_OF_NODE
class MultiVersionValue {
public:
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

    // Lazy reclamation with optimized watermark checking
    // Reclaims old versions that are safe to delete (below watermark)
    static void lazyReclaim(uint32_t time_term, uint32_t current_term,
                            char *root,
                            versioned_str_struct *root_owner) {
        // Use TThread counter for thread-local reclamation frequency
        TThread::incr_counter();
        if (TThread::counter() % 50 != 0) return;
        
        // Cache watermark with proper memory ordering
        uint32_t watermark = sync_util::sync_logger::retrieveShardW_relaxed() / 10;
        if (watermark == 0) return;  // Skip if watermark not initialized
        
        // Phase 1: Find the safe reclamation point
        char *safe_point = nullptr;
        char *current = root;
        
        // Navigate to first version below watermark
        while (current && mako::load_node_data_size(current) > 0) {
            const int16_t data_size = mako::load_node_data_size(current);
            char *data = mako::load_node_data(current);
            const uint32_t tt = mako::load_value_time_term(data, data_size);
            
            if (tt / 10 < watermark) {
                safe_point = current;
                break;
            }
            
            current = mako::value_node_address(data, data_size);
        }
        
        if (!safe_point) return;  // No safe versions to reclaim
        
        // No other thread accesses this chain while it is reclaimed.
        mako::reclaim_value_chain_after(safe_point, root_owner->embedded_data());
    }

    static bool mvGET(string& val,
                      char *oldval_str, // oldval_str == val, but it's the reference to the actual value
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
                          uint8_t current_term) {
        // Single timestamp system
        char *oldval_str=(char*)e->data();
        int oldval_len=e->length();
        uint32_t time_term = TThread::txn->tid_unique_ * 10 + TThread::txn->current_term_;
        if (isInsert) { // insert
            // Set single timestamp
            mako::store_value_node_timestamp(
                oldval_str, oldval_len, TThread::txn->tid_unique_);
            mako::store_value_node_data_size(
                oldval_str, oldval_len, 0);  // indicate no next block
            mako::store_value_time_term(oldval_str, oldval_len, time_term);
        } else {  // update or delete
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
            e->modifyData(new_vv);
            lazyReclaim(time_term, current_term, header, e);
        }
        return ;
    }
} ;
