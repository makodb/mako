// RPC setup helpers extracted from tpcc.cc
#ifndef MAKO_BENCHMARKS_RPC_SETUP_H
#define MAKO_BENCHMARKS_RPC_SETUP_H

#include <thread>
#include <vector>
#include <unordered_map>
#include <map>
#include <atomic>
#include <string>

#include "bench.h"
#include <unordered_map>

// Decoupled setup helpers for RPC benchmark: eRPC server and helper threads.
// These functions mirror the original logic in tpcc.cc but are now reusable.

namespace mako {

// Launch helper threads for all remote warehouses across shards.
void setup_helper(
  abstract_db *db,
  const std::map<int, abstract_ordered_index *> &open_tables);

// Add or update a table mapping for already running helper threads.
void setup_update_table(int table_id, abstract_ordered_index *table);

// Signal helper threads to stop processing requests.
void stop_helper();

// Launch eRPC server threads and wire up per-warehouse queues.
void setup_erpc_server();

// Stop all eRPC servers previously started by setup_erpc_server().
void stop_erpc_server();

// Initialize per thread
void initialize_per_thread(abstract_db *db) ;

} // namespace mako

#endif // MAKO_BENCHMARKS_RPC_SETUP_H
