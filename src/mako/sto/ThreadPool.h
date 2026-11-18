#pragma once

#include <stdexcept>
#include <exception>

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
    struct replay_attachment {
        abstract_db* db = nullptr;

        // @unsafe - The process-lifetime replay database outlives its workers.
        ~replay_attachment() {
            if (db != nullptr) {
                try {
                    db->thread_end();
                } catch (...) {
                    // Cleanup failure is terminal, not an invitation to replay.
                    std::terminate();
                }
            }
        }
    };
    static thread_local replay_attachment replay_thread_attachment_;

public:
    static abstract_db* replay_thread_wrapper_db;
    ThreadDBWrapperMbta() = delete;
    // @safe - The partition is independent of the OS-thread worker identity.
    explicit ThreadDBWrapperMbta(int thread_id){
        this->thread_id = thread_id;
    }
    // @unsafe - Calls the selected backend's worker-affine attachment bridge.
    abstract_db* getDB() {
        abstract_db* const db = replay_thread_wrapper_db;
        if (db == nullptr) {
            throw std::logic_error("replay database is not initialized");
        }
        auto& attachment = replay_thread_attachment_;
        if (attachment.db != nullptr && attachment.db != db) {
            throw std::logic_error(
                "replay OS thread cannot switch database runtimes");
        }
        if (attachment.db == nullptr) {
            db->replay_thread_init();
            attachment.db = db;
        }
        // Single-Raft may alternate partitions on this same attached worker.
        db->replay_thread_select_partition(thread_id);
        return db;
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
