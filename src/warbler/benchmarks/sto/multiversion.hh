#pragma once
#include <map>
#include "lib/common.h"
#include <vector>
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/sto/common.hh"
#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

// value field composition: data + srolis::BITS_OF_TT (timestamp + term) + srolis::BITS_OF_NODE
class MultiVersionValue {
public:
    static bool isDeleted(std::string& v) {
        // for non-deleted value, the length of value at least 2+srolis::EXTRA_BITS_FOR_VALUE
        return v.length() == 1+srolis::EXTRA_BITS_FOR_VALUE && v[0] == 'B';
    }

    template <typename ValueType>
    static std::vector<string> getAllVersion(string val) {
        std::vector<string> ret;
        int vt = 1;
        uint32_t *time_term = 0;
        time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-srolis::EXTRA_BITS_FOR_VALUE));
        
        std::string tmp;
        tmp.assign(val.data(),val.length());
        ret.push_back(isDeleted(val)? "DEL": (tmp));
        
        // fast peek
        srolis::Node *header = reinterpret_cast<srolis::Node *>((char*)(val.data()+val.length()-srolis::BITS_OF_NODE));
        while (header->data_size > 0) {
            vt ++;
            time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-srolis::EXTRA_BITS_FOR_VALUE));

            val.assign(header->data, (int)header->data_size); // rewrite with next block value
            std::string tmp;
            tmp.assign(val.data(),val.length());
            ret.push_back(isDeleted(val)? "DEL": (tmp));
            header = reinterpret_cast<srolis::Node *>((char*)(val.data()+val.length()-srolis::BITS_OF_NODE));
        }
        return ret;
    }

    // lazyReclaim goes after write not read, because two read threads might contend on the same key
    // 1) if a node is deleted, then free all nodes following it; 
    // 2) find the first safe version (<=watermark) and reclaim all versions after it (always stay at the same term)
    // facts: even free it frequently, the jemalloc would release it back to OS if memory is sufficient;
    static void lazyReclaim(uint32_t time_term, uint32_t current_term, srolis::Node *root) {
        return ;
        TThread::incr_counter();
        if (TThread::counter()%10!=0) return;

        srolis::Node *next;
        char * data;
        int16_t data_size;

        uint32_t watermark = sync_util::sync_logger::retrieveShardW()/10;
        uint32_t *tt;
        while (time_term/10>=watermark && root->data_size>0) {
            next = reinterpret_cast<srolis::Node *>(root->data+root->data_size-srolis::BITS_OF_NODE);
            tt = reinterpret_cast<uint32_t *>(root->data+root->data_size-srolis::EXTRA_BITS_FOR_VALUE);
            time_term = *tt;
            root = next;
        }

        while (root->data_size>0) {
           next = reinterpret_cast<srolis::Node *>(root->data+root->data_size-srolis::BITS_OF_NODE);
           data = next->data;
           data_size = next->data_size;
           if (next->data_size>0) {  // to avoid partial ::free
            ::free((char*)root->data);
           }
           root->data = data;
           root->data_size = data_size;
        }
    }

    static bool mvGET(string& val,
                      char *oldval_str, // oldval_str == val, but it's the reference to the actual value
                      uint8_t current_term,
                      std::unordered_map<int, uint32_t> hist_timestamp,
                      std::unordered_map<int, vector<uint32_t>> hist_timestamp_vec) {
        uint32_t *time_term = 0;
        time_term = reinterpret_cast<uint32_t*>((char*)(val.data()+val.length()-srolis::EXTRA_BITS_FOR_VALUE));

        if (likely(*time_term % 10 == current_term)) { // current term: get the latest value but reclaim the all version below the watermark within the current term
            return !isDeleted(val);
        } else { // past term e
            srolis::Node *header = reinterpret_cast<srolis::Node *>((char*)(val.data()+val.length()-srolis::BITS_OF_NODE));
            
#if defined(FAIL_NEW_VERSION)
            // It's possible that hist_timestamp_vec is not updated yet, and return it directly; and the remote server would do a check
            if  (hist_timestamp_vec.find(*time_term % 10)==hist_timestamp_vec.end()) {
                return !isDeleted(val);
            }
            // check if the stored value is below the cached watermark
            if (sync_util::sync_logger::safety_check(header->timestamps, hist_timestamp_vec[*time_term % 10])) { // We always kill the failed shard 0.
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getPartitionID(),*time_term%10,current_term, hist_timestamp[*time_term % 10],val.length());
                    //srolis::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (header->data_size > 0) {
                time_term = reinterpret_cast<uint32_t*>((char*)(header->data+header->data_size-srolis::EXTRA_BITS_FOR_VALUE));
                if (sync_util::sync_logger::safety_check(header->timestamps, hist_timestamp_vec[*time_term % 10])) { // We always kill the failed shard 0.
                    val.assign(header->data, (int)header->data_size); // rewrite val with next block value
                    header = reinterpret_cast<srolis::Node *>((char*)(val.data()+val.length()-srolis::BITS_OF_NODE));
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = reinterpret_cast<srolis::Node *>((char*)(header->data+header->data_size-srolis::BITS_OF_NODE));
            }
        }
#else
            if (header->timestamps[0] / 10 <= hist_timestamp[*time_term % 10]) { // We always kill the failed shard 0.
                bool ret = !isDeleted(val);
                if (!ret) {
                    //Warning("XXXX par_id:%d,time_term:%d,cur_term:%d, watermark:%lld,len of v:%d",TThread::getPartitionID(),*time_term%10,current_term, hist_timestamp[*time_term % 10],val.length());
                    //srolis::printStringAsBit(val);
                }
                return ret;
            }
            // find the latest stable timestamp below the watermark within the past term e
            while (header->data_size > 0) {
                time_term = reinterpret_cast<uint32_t*>((char*)(header->data+header->data_size-srolis::EXTRA_BITS_FOR_VALUE));
                if (header->timestamps[0] / 10 <= hist_timestamp[*time_term % 10]) { // We always kill the failed shard 0.
                    val.assign(header->data, (int)header->data_size); // rewrite val with next block value
                    header = reinterpret_cast<srolis::Node *>((char*)(val.data()+val.length()-srolis::BITS_OF_NODE));
                    if (isDeleted(val)) {
                        return false;
                    }
                    break;
                }
                header = reinterpret_cast<srolis::Node *>((char*)(header->data+header->data_size-srolis::BITS_OF_NODE));
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
        char *oldval_str=(char*)e->data();
        int oldval_len=e->length();
        uint32_t time_term = TThread::txn->tid_unique_ * 10 + TThread::txn->current_term_;
        if (isInsert) { // insert
            srolis::Node* header = reinterpret_cast<srolis::Node*>(oldval_str+oldval_len-srolis::BITS_OF_NODE);
#if MERGE_KEYS_GROUPS < SHARDS
            int cnt_per_group = SHARDS / MERGE_KEYS_GROUPS;
            for (int i=0;i<MERGE_KEYS_GROUPS;i++) {
                header->timestamps[i] = TThread::txn->vectorTimestamp[i * (cnt_per_group + 1) - 1];
            }
#elif MERGE_KEYS_GROUPS > SHARDS
            for (int i=0;i<MERGE_KEYS_GROUPS;i++){
                header->timestamps[i]= i<SHARDS ? TThread::txn->vectorTimestamp[i] : 0;
            }
#else
            for (int i=0;i<SHARDS;i++){
                header->timestamps[i]=TThread::txn->vectorTimestamp[i];
            }
#endif
            header->data_size = 0;  // indicate no next block
            memcpy(oldval_str+oldval_len-srolis::EXTRA_BITS_FOR_VALUE, &time_term, srolis::BITS_OF_TT);
            lazyReclaim(time_term, current_term, header);
        } else {  // update or delete
            // alg1: copy old value and then update
            // if (false){
            //     char *data = (char*) malloc(oldval_len);
            //     memcpy(data, oldval_str, oldval_len);

            //     // update the old position: node, value, time_term
            //     memcpy(oldval_str, newval.data(), newval.length()-srolis::EXTRA_BITS_FOR_VALUE);
            //     memcpy(oldval_str+newval.length()-srolis::EXTRA_BITS_FOR_VALUE, &time_term, srolis::BITS_OF_TT);
            //     srolis::Node* header = reinterpret_cast<srolis::Node*>(oldval_str+newval.length()-srolis::BITS_OF_NODE);
            //     header->data_size = oldval_len;
            //     header->data = data;
            //     lazyReclaim(time_term, current_term, header);
            // }

            // alg2: modify the pointer, avoid memcpy for oldval
            //if (true){
                char* new_vv = (char*)malloc(newval.length());
                memcpy(new_vv, newval.data(), newval.length()-srolis::EXTRA_BITS_FOR_VALUE);
                memcpy(new_vv+newval.length()-srolis::EXTRA_BITS_FOR_VALUE, 
                                    &time_term, srolis::BITS_OF_TT);
                srolis::Node* header = reinterpret_cast<srolis::Node*>(new_vv+newval.length()-srolis::BITS_OF_NODE);
#if MERGE_KEYS_GROUPS < SHARDS
                int cnt_per_group = SHARDS / MERGE_KEYS_GROUPS;
                for (int i=0;i<MERGE_KEYS_GROUPS;i++) {
                    header->timestamps[i] = TThread::txn->vectorTimestamp[i * (cnt_per_group + 1) - 1];
                }
#elif MERGE_KEYS_GROUPS > SHARDS
                for (int i=0;i<MERGE_KEYS_GROUPS;i++){
                    header->timestamps[i]=i<SHARDS ? TThread::txn->vectorTimestamp[i] : 0;
                }
#else
                for (int i=0;i<SHARDS;i++){
                    header->timestamps[i]=TThread::txn->vectorTimestamp[i];
                }
#endif
                header->data_size = oldval_len;
                header->data = e->data();
                e->modifyData(new_vv);
                lazyReclaim(time_term, current_term, header);
            //}
        }
        return ;
    }
} ;