#pragma once

#include "storage/abstract_db.h"
#include "function_pool.h"

// @unsafe
size_t getFileContentNew_OneLogOptimized_mbta_v2(char *buffer, /* K-V pairs */
                                                 uint32_t cid,  /* timestamp on current shard */
                                                 unsigned short int count,
                                                 unsigned int len,
                                                 abstract_db* db);

class ThreadDBWrapperMbta {
public:
    static abstract_db* replay_thread_wrapper_db;
    // @unsafe - initialize the physical replay worker, not a partition alias.
    abstract_db * getDB() {
        static thread_local bool is_init = false;
        if (!is_init) {
            Transaction::register_thread();
            TThread::disable_multiversion(); // one the follower, disable the multi-version
            actual_directs::thread_init();
            is_init = true;
        }
        return replay_thread_wrapper_db;
    }
};

class TSharedThreadPoolMbta
{
  public:
    TSharedThreadPoolMbta (int threads)
    {
        // Create the specified number of threads
        for (int i = 0; i < threads; ++i) {
            this->_mapping[i] = new ThreadDBWrapperMbta();
        }
    }

    ThreadDBWrapperMbta* getDBWrapper(int par_id) {
        return this->_mapping[par_id];
    }

    std::unordered_map<int,ThreadDBWrapperMbta*> _mapping;
};