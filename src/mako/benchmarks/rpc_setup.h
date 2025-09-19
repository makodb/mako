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
  const std::map<std::string, abstract_ordered_index *> &open_tables/*,
  const std::map<std::string, std::vector<abstract_ordered_index *>> &partitions,
  const std::map<std::string, std::vector<abstract_ordered_index *>> &remote_partitions*/);

// Signal helper threads to stop processing requests.
void stop_helper();

// Launch eRPC server threads and wire up per-warehouse queues.
void setup_erpc_server();

// Stop all eRPC servers previously started by setup_erpc_server().
void stop_erpc_server();

} // namespace mako

#endif // MAKO_BENCHMARKS_RPC_SETUP_H
