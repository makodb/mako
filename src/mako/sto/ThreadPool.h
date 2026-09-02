#pragma once

#include "storage/abstract_db.h"
#include "function_pool.h"
#include "thread_registration.hh"

// @unsafe
size_t getFileContentNew_OneLogOptimized_mbta_v2(char *buffer, /* K-V pairs */
                                                 uint32_t cid,  /* timestamp on current shard */
                                                 unsigned short int count,
                                                 unsigned int len,
                                                 abstract_db* db);

class ThreadDBWrapperMbta {
protected:
    int thread_id;
    bool is_init = false;

public:
    static abstract_db* replay_thread_wrapper_db;
    ThreadDBWrapperMbta() = delete;
    ThreadDBWrapperMbta(int thread_id){
        this->thread_id = thread_id;
    }
    abstract_db * getDB(){ // have to be initialized inside each replay thread
        if(!is_init){
            ALWAYS_ASSERT(mako::silo::claim_thread_runtime(
                mako::silo::thread_runtime::native_mako));
            const int registered_id = mako::silo::try_allocate_thread_id();
            ALWAYS_ASSERT(registered_id >= 0);
            this->thread_id = registered_id;
            TThread::set_id(registered_id);
            TThread::disable_multiversion(); // one the follower, disable the multi-version
            ALWAYS_ASSERT(mako::silo::ensure_epoch_runtime());
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
            this->_mapping[i] = new ThreadDBWrapperMbta(i);
        }
    }

    ThreadDBWrapperMbta* getDBWrapper(int par_id) {
        return this->_mapping[par_id];
    }

    std::unordered_map<int,ThreadDBWrapperMbta*> _mapping;
};
