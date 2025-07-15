#include "ThreadPool.h"
#include <stdexcept>
#include <algorithm>
#include<string_view>
#include <unordered_set>
#include <vector>
#include "lib/common.h"

thread_local str_arena arena;
thread_local void *buf = NULL;
thread_local string obj_k;
thread_local string obj_v;

void keystore_encode3_v2(std::string& s, uint32_t x) {
    assert(s.length()>srolis::EXTRA_BITS_FOR_VALUE);
    memcpy ((void *)(s.data() + s.length() - srolis::EXTRA_BITS_FOR_VALUE), &x, srolis::BITS_OF_TT);
    // for the later on takeovering from the old leader
    srolis::Node *header = reinterpret_cast<srolis::Node *>((char*)(s.data() + s.length() - srolis::BITS_OF_NODE));
    header->data_size = 0;
}

inline uint32_t keystore_decode3_v2(const std::string& s){
    uint32_t cid=0;
    memcpy (&cid, (void *) (s.data () + s.length() - srolis::EXTRA_BITS_FOR_VALUE), srolis::BITS_OF_TT);
    return cid;
}

bool cmpFunc2_v2(const std::string& newValue,const std::string& oldValue)
{
    uint32_t commit_id_new = keystore_decode3_v2(newValue);
    uint32_t commit_id_old = keystore_decode3_v2(oldValue);

    return (commit_id_new%10 > commit_id_old%10) || (commit_id_new/10 > commit_id_old/10);
}

size_t getFileContentNew_OneLogOptimized_mbta_v2(char *buffer, /* K-V pairs */
                                                 uint32_t cid,  /* timestamp on current shard */
                                                 unsigned short int count,
                                                 unsigned int len,
                                                 abstract_db* db) {
    size_t put_ops = 0;

    unsigned short int *len_of_K=0, *len_of_V=0, *table_id=0;
    size_t offset=0;
    bool delete_true = false;
    for(int i=0;i<count;i++) {
        delete_true = false ;
        // 1. len of K
        len_of_K = reinterpret_cast<unsigned short int*>(buffer + offset);
        offset += sizeof(unsigned short int) ;

        // 2. content of K
        obj_k.assign(buffer + offset, *len_of_K);
        offset += *len_of_K;

        // 3. len of V
        len_of_V = reinterpret_cast<unsigned short int*>(buffer + offset);
        offset += sizeof(unsigned short int) ;

        // 4. content of V, add an extra sizeof(uint64) bytes
        //obj_v.assign(buffer + offset, *len_of_V + srolis::EXTRA_BITS_FOR_VALUE); // might cause coredump due to freed memory
        obj_v.resize(*len_of_V + srolis::EXTRA_BITS_FOR_VALUE);
        memcpy((char*)obj_v.c_str(),buffer+offset, *len_of_V);
        offset += *len_of_V;

        // 5. table id
        table_id = reinterpret_cast<unsigned short*>(buffer + offset);
        offset += sizeof(unsigned short int) ;

        if((*table_id) & (1 << 15)) {
            delete_true = true;
            *table_id = (*table_id) ^ (1 << 15);
        }

        if (*table_id == 0 || *table_id > 10000) {
            Warning("the table_id: %d", *table_id);
            exit(1);
        }

        if (delete_true) {
            obj_v.assign(1+srolis::EXTRA_BITS_FOR_VALUE, 'B'); // special flags for DELETE
            //value = string(1+srolis::EXTRA_BITS_FOR_VALUE, 'B');  // special flags for DELETE
        }

        // 6. encode value + cid_v
        keystore_encode3_v2 (obj_v, cid);
        //keystore_encode3_v2 (value, cid);

        // 7. put it into actual tables
        int try_cnt = 1 ;
        while (1) {
            try {
                //Warning("Info of KV: # of K: %d, # of V: %d, table_id: %d, is_deleted: %d,key:%s", *len_of_K, obj_v.length(), *table_id, delete_true,srolis::getStringAsBit(obj_k).c_str());
                void *txn = db->new_txn(0, arena, buf, abstract_db::HINT_DEFAULT);
                abstract_ordered_index *table_index = db->open_index(*table_id) ;
                table_index->put_mbta(txn, obj_k, cmpFunc2_v2, obj_v);
                auto ret = db->commit_txn_no_paxos(txn);
                if (try_cnt > 1) {
                    std::cout << "succeed at retry#:" << try_cnt << std::endl;
                }
                break ;
            } catch (...) {   // if abort happens, replay it until it succeeds
                std::cout << "exception, retry#:" << try_cnt << std::endl;
                try_cnt += 1 ;
            }
        }
        // if (*table_id>=25&&*table_id<=27)
            put_ops ++;
    }

    return put_ops;
}