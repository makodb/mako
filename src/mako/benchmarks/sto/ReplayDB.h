
#ifndef SILO_ONS_BENCHMARKS_STO_REPLAYDB_H_
#define SILO_ONS_BENCHMARKS_STO_REPLAYDB_H_
#include <iostream>
#include <vector>
#include "../abstract_db.h"

/**
 * @brief: decode buffer and then replay records
 *
 */
size_t treplay_in_same_thread_opt_mbta_v2(size_t par_id, char *buffer, size_t len, abstract_db* db, int nshards);

/**
 * @brief Get the latest vectorized commit from buffer
 *
 */
std::vector<uint32_t> get_latest_commit_id(char *buffer, size_t len, int nshards);

#endif