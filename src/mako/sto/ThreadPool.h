#pragma once

#include <stdexcept>

#include "silo_runtime.h"
#include "storage/abstract_db.h"
#include "function_pool.h"

// @unsafe
size_t getFileContentNew_OneLogOptimized_mbta_v2(const char *buffer, /* K-V pairs */
                                                 uint32_t cid,  /* timestamp on current shard */
                                                 unsigned short int count,
                                                 unsigned int len,
                                                 abstract_db* db);

class ThreadDBWrapperMbta {
protected:
    int thread_id;
    inline static thread_local bool replay_thread_initialized_ = false;

public:
    static abstract_db* replay_thread_wrapper_db;
    ThreadDBWrapperMbta() = delete;
    ThreadDBWrapperMbta(int thread_id){
        this->thread_id = thread_id;
    }
    abstract_db * getDB(){ // have to be initialized inside each replay thread
        // Replay tables are created by DB::Open under the process-global Silo
        // runtime. A replay participant must join that same Masstree epoch
        // domain before threadinfo::make() publishes it to a registry.
        SiloRuntime* const runtime = SiloRuntime::GlobalDefault();
        if (SiloRuntime::Current() != runtime) {
            throw std::logic_error(
                "Masstree replay thread is bound to a non-default Silo runtime");
        }
        if (actual_directs::mythreadinfo.ti != nullptr &&
            actual_directs::mythreadinfo.ti->context() !=
                runtime->masstree_context()) {
            throw std::logic_error(
                "Masstree replay thread already owns a foreign epoch participant");
        }
        runtime->BindToCurrentThread();

        // The replay partition is not a transaction-thread identity. Restore
        // this OS thread's stable STO/RCU slot and the selected replay
        // partition on every call: single-Raft can alternate partitions on
        // one OS thread, and callbacks may be dispatched by a fixed pool.
        TThread::assign_stable_id();
        TThread::set_pid(this->thread_id);
        TThread::disable_multiversion(); // on the follower, disable multi-version
        Sto::update_threadid();
        if (!replay_thread_initialized_) {
            actual_directs::thread_init();
            replay_thread_initialized_ = true;
        }
        if (actual_directs::mythreadinfo.ti == nullptr ||
            actual_directs::mythreadinfo.ti->context() !=
                runtime->masstree_context()) {
            throw std::logic_error(
                "Masstree replay thread failed to join the default epoch domain");
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
