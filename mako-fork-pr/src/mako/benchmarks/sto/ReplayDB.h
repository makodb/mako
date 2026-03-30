
#ifndef SILO_ONS_BENCHMARKS_STO_REPLAYDB_H_
#define SILO_ONS_BENCHMARKS_STO_REPLAYDB_H_
#include <iostream>
#include <vector>
#include "../abstract_db.h"

// Single timestamp system commit info
struct CommitInfo {
    uint32_t timestamp; // timestamp*10+term
    uint32_t latency_tracker;
};

/**
 * @brief: decode buffer and then replay records
 *
 */
size_t treplay_in_same_thread_opt_mbta_v2(size_t par_id, char *buffer, size_t len, abstract_db* db, int nshards);

/**
 * @brief Get the latest commit info from buffer (single timestamp system)
 *
 */
CommitInfo get_latest_commit_info(char *buffer, size_t len);

#endif