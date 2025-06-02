#include "ReplayDB.h"
#include <random>
#include <algorithm>
#include <unordered_set>
#include "ThreadPool.h"

std::vector<uint32_t> get_latest_commit_id(char *buffer, size_t len, int nshards) {
    std::vector<uint32_t> ret;
    ret.reserve(nshards+1);
    int pos=len-sizeof(uint32_t)*(nshards+1);
    // [0 - nshards-1] [latency_tracker]
    //     VT            tracking latency to commit a log
    for (int i=0;i<nshards+1;i++){
        uint32_t tmp=0;
        memcpy(&tmp, buffer+pos, sizeof(uint32_t));
        ret.push_back(tmp);
        pos+=sizeof(uint32_t);
    }
    return ret;
}


size_t treplay_in_same_thread_opt_mbta_v2(size_t par_id, char *buffer, size_t len, abstract_db* db, int nshards) {
    //printf("replay a log, par_id:%d, len:%d\n", par_id, len);
    len -= sizeof(uint32_t)*(nshards+1); // eliminate last max vectorized timestamp
    size_t ULL_LEN = sizeof (uint32_t);

    std::vector<WrappedLogV2*> _intermediateLogs;

    size_t idx=0;
    int c_txn=0, c_kv=0;
    while (idx < len) {
        // 1. get current shard cid
        c_txn+=1;
        uint32_t cid=0;
        memcpy ((char *) &cid, buffer + idx, ULL_LEN);
        idx += sizeof(uint32_t);

        // 2. get count of K-V
        unsigned short int count=0;
        memcpy ((char *) &count, buffer + idx, sizeof(unsigned short int));
        idx += sizeof(unsigned short int) ;
        c_kv+=count;

        // 3. get len of K-V
        unsigned int _len=0;
        memcpy ((char *) &_len, buffer + idx, sizeof(unsigned int));
        idx += sizeof(unsigned int) ;

        // 4. wrap K-V
        auto on_log = new WrappedLogV2() ;
        on_log->cid= cid;
        on_log->count = count;
        on_log->len = _len;
        on_log->pairs = (char *)buffer + idx;

        // 5. skip K-V pairs
        idx += _len ;

        _intermediateLogs.emplace_back (on_log);
    }

    size_t thread_put = 0;
    auto iter = _intermediateLogs.begin();
    for (;iter!=_intermediateLogs.end();iter++) {
        thread_put += getFileContentNew_OneLogOptimized_mbta_v2((*iter)->pairs,
                                                                (*iter)->cid,
                                                                (*iter)->count,
                                                                (*iter)->len,
                                                                db) ;
    }
    //Warning("%d K-V pairs replayed, par_id:%d, total-len: %d, c_txn:%d, c_kv:%d", thread_put, par_id, len, c_txn, c_kv);
    return thread_put ;
}