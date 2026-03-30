#pragma once
#include <map>
#include "lib/common.h"
#include <vector>
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/sto/common.hh"
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
        int vt = 1;
        uint32_t *time_term = 0;
        time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-mako::EXTRA_BITS_FOR_VALUE));
        
        std::string tmp;
        tmp.assign(val.data(),val.length());
        ret.push_back(isDeleted(val)? "DEL": (tmp));
        
        // fast peek
        mako::Node *header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
        while (header->data_size > 0) {
            vt ++;
            time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-mako::EXTRA_BITS_FOR_VALUE));

            val.assign(header->data, (int)header->data_size); // rewrite with next block value
            std::string tmp;
            tmp.assign(val.data(),val.length());
            ret.push_back(isDeleted(val)? "DEL": (tmp));
            header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
        }
        return ret;
    }

    // Lazy reclamation with optimized watermark checking
    // Reclaims old versions that are safe to delete (below watermark)
    static void lazyReclaim(uint32_t time_term, uint32_t current_term, mako::Node *root) {
        // Use TThread counter for thread-local reclamation frequency
        TThread::incr_counter();
        if (TThread::counter() % 50 != 0) return;
        
        // Cache watermark with proper memory ordering
        uint32_t watermark = sync_util::sync_logger::retrieveShardW_relaxed() / 10;
        if (watermark == 0) return;  // Skip if watermark not initialized
        
        // Phase 1: Find the safe reclamation point
        mako::Node *safe_point = nullptr;
        mako::Node *current = root;
        std::vector<mako::Node*> to_free;  // Batch freeing for efficiency
        
        // Navigate to first version below watermark
        while (current && current->data_size > 0) {
            uint32_t *tt = reinterpret_cast<uint32_t*>(
                current->data + current->data_size - mako::EXTRA_BITS_FOR_VALUE);
            
            if ((*tt) / 10 < watermark) {
                safe_point = current;
                break;
            }
            
            current = reinterpret_cast<mako::Node*>(
                current->data + current->data_size - mako::BITS_OF_NODE);
        }
        
        if (!safe_point) return;  // No safe versions to reclaim
        
        // Phase 2: Collect nodes to free (after safe point)
        current = safe_point;
        while (current && current->data_size > 0) {
            mako::Node *next = reinterpret_cast<mako::Node*>(
                current->data + current->data_size - mako::BITS_OF_NODE);
            
            if (next->data_size > 0) {
                to_free.push_back(current);
            }
            current = next;
        }
        
        // Phase 3: Update chain and batch free
        if (!to_free.empty()) {
            // Update the chain - no other thread accesses this
            safe_point->data_size = 0;  // Mark end of chain
            
            // Batch free old nodes
            for (auto* node : to_free) {
                ::free(node->data);
            }
        }
    }

    static bool mvGET(string& val,
                      char *oldval_str, // oldval_str == val, but it's the reference to the actual value
                      uint8_t current_term,
                      std::unordered_map<int, uint32_t> hist_timestamp) {
        uint32_t *time_term = 0;
        time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-mako::EXTRA_BITS_FOR_VALUE));

        if (likely(*time_term % 10 == current_term)) { // current term: get the latest value but reclaim the all version below the watermark within the current term
            return !isDeleted(val);
        } else { // past term e
            mako::Node *header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
            
#if defined(FAIL_NEW_VERSION)
            // It's possible that hist_timestamp is not updated yet, and return it directly; and the remote server would do a check
            if  (hist_timestamp.find(*time_term % 10)==hist_timestamp.end()) {
                return !isDeleted(val);
            }
            // check if the stored value is below the cached watermark
            if (sync_util::sync_logger::safety_check(header->timestamp, hist_timestamp[*time_term % 10])) { // Single timestamp check
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getGlobalPartitionID(),*time_term%10,current_term, hist_timestamp[*time_term % 10],val.length());
                    //mako::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (header->data_size > 0) {
                time_term = reinterpret_cast<uint32_t*>((char*)(header->data+header->data_size-mako::EXTRA_BITS_FOR_VALUE));
                if (sync_util::sync_logger::safety_check(header->timestamp, hist_timestamp[*time_term % 10])) { // Single timestamp check
                    val.assign(header->data, (int)header->data_size); // rewrite val with next block value
                    header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = reinterpret_cast<mako::Node *>((char*)(header->data+header->data_size-mako::BITS_OF_NODE));
            }
        }
#else
            if (header->timestamp / 10 <= hist_timestamp[*time_term % 10]) { // Single timestamp check
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getGlobalPartitionID(),*time_term%10,current_term, hist_timestamp[*time_term % 10],val.length());
                    //mako::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (header->data_size > 0) {
                time_term = reinterpret_cast<uint32_t*>((char*)(header->data+header->data_size-mako::EXTRA_BITS_FOR_VALUE));
                if (header->timestamp / 10 <= hist_timestamp[*time_term % 10]) { // Single timestamp check
                    val.assign(header->data, (int)header->data_size); // rewrite val with next block value
                    header = reinterpret_cast<mako::Node *>((char*)(val.data()+val.length()-mako::BITS_OF_NODE));
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = reinterpret_cast<mako::Node *>((char*)(header->data+header->data_size-mako::BITS_OF_NODE));
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
            mako::Node* header = reinterpret_cast<mako::Node*>(oldval_str+oldval_len-mako::BITS_OF_NODE);
            // Set single timestamp
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = 0;  // indicate no next block
            memcpy(oldval_str+oldval_len-mako::EXTRA_BITS_FOR_VALUE, &time_term, mako::BITS_OF_TT);
        } else {  // update or delete
            char* new_vv = (char*)malloc(newval.length());
            memcpy(new_vv, newval.data(), newval.length()-mako::EXTRA_BITS_FOR_VALUE);
            memcpy(new_vv+newval.length()-mako::EXTRA_BITS_FOR_VALUE, 
                                &time_term, mako::BITS_OF_TT);
            mako::Node* header = reinterpret_cast<mako::Node*>(new_vv+newval.length()-mako::BITS_OF_NODE);
            // Set single timestamp
            header->timestamp = TThread::txn->tid_unique_;
            header->data_size = oldval_len;
            header->data = e->data();
            e->modifyData(new_vv);
            lazyReclaim(time_term, current_term, header);
        }
        return ;
    }
} ;