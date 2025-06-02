#pragma once

#include "../abstract_db.h"
#include "function_pool.h"

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
            TThread::set_id(this->thread_id);
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
            this->_mapping[i] = new ThreadDBWrapperMbta(i);
        }
    }

    ThreadDBWrapperMbta* getDBWrapper(int par_id) {
        return this->_mapping[par_id];
    }

    std::unordered_map<int,ThreadDBWrapperMbta*> _mapping;
};