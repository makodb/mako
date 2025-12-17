#pragma once

#include "../abstract_db.h"
#include "function_pool.h"
#include <rusty/hashmap.hpp>

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
    // @safe - simple value assignment, no pointer operations
    ThreadDBWrapperMbta(int thread_id){
        this->thread_id = thread_id;
    }
    // @unsafe - calls undeclared functions (TThread::set_id, TThread::disable_multiversion, actual_directs::thread_init)
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

// @safe - uses rusty::HashMap which is safe
class TSharedThreadPoolMbta
{
  public:
    // @safe - uses new operator and rusty::HashMap::insert
    TSharedThreadPoolMbta (int threads)
    {
        // Create the specified number of threads
        for (int i = 0; i < threads; ++i) {
            _mapping.insert(i, new ThreadDBWrapperMbta(i));
        }
    }

    // @safe - uses rusty::HashMap::get which returns Option
    ThreadDBWrapperMbta* getDBWrapper(int par_id) {
        auto result = _mapping.get(par_id);
        if (result.is_some()) {
            return *result.unwrap();
        }
        return nullptr;
    }

    rusty::HashMap<int, ThreadDBWrapperMbta*> _mapping;
};