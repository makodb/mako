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

// Decoupled setup helpers for RPC benchmark: RPC server and helper threads.
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

// Launch RPC server threads and wire up per-warehouse queues.
void setup_rpc_server();

// Stop all RPC servers previously started by setup_rpc_server().
void stop_rpc_server();

// Initialize per thread
void initialize_per_thread(abstract_db *db) ;

// Start TCP server for remote client connections.
// The TCP server listens on the specified port and routes client API
// requests (BeginTxn, Commit, Put, Get, etc.) to the ShardReceiver handlers.
// @param port - TCP port to listen on (default 31000)
// @return true if started successfully
bool setup_client_tcp_server(int port = 31000);

// Start TCP server with explicit database and table mappings.
// Use this overload in single-shard mode where no helper servers exist.
// @param db - Database handle for transaction processing
// @param open_tables - Table ID to index mappings
// @param port - TCP port to listen on (default 31000)
// @return true if started successfully
// @unsafe - Creates ShardReceiver that requires external lifetime management
bool setup_client_tcp_server(
  abstract_db *db,
  const std::map<int, abstract_ordered_index *> &open_tables,
  int port = 31000);

// Stop the client TCP server.
void stop_client_tcp_server();

// Get the ShardReceiver instance for the current shard.
// Returns nullptr if not initialized.
// Used by MakoClientService for RPC-based client connections.
class ShardReceiver;  // Forward declaration
ShardReceiver* get_shard_receiver();

} // namespace mako

#endif // MAKO_BENCHMARKS_RPC_SETUP_H
