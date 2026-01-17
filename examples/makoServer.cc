//
// Mako Standalone Server
//
// This server binary hosts the Mako database and accepts RPC requests
// from remote clients. It decouples the server from client logic.
//
// Usage:
//   ./makoServer <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]
//
// Example:
//   ./makoServer 2 0 6 localhost 1       # Paxos replication
//   ./makoServer 2 0 6 localhost 1 raft  # Raft replication
//

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <map>
#include <signal.h>
#include <atomic>
#include <mako.hh>
#include "db.hh"
#include "examples/common.h"
#include "benchmarks/rpc_setup.h"
#include "../src/mako/benchmarks/mbta_sharded_ordered_index.hh"
#include "deptran/replication_helper.h"
#include "rrr/rpc/server.hpp"
#include "client_service.h"

using namespace std;
using namespace mako;

// @safe - Global shutdown flag with atomic access
static std::atomic<bool> g_shutdown_requested{false};

// @safe - Signal handler for graceful shutdown
void signal_handler(int signum) {
    printf("\nReceived signal %d, initiating shutdown...\n", signum);
    g_shutdown_requested.store(true);
}

// @safe - Print usage information
void print_usage(const char* program_name) {
    printf("Usage: %s <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]\n", program_name);
    printf("\nArguments:\n");
    printf("  nshards          - Number of shards in the cluster (1-16)\n");
    printf("  shardIdx         - Index of this shard (0 to nshards-1)\n");
    printf("  nthreads         - Number of worker threads (1-64)\n");
    printf("  paxos_proc_name  - Process role: 'localhost' (leader), 'p1', 'p2' (followers), 'learner'\n");
    printf("  is_replicated    - Enable replication: 0 (disabled), 1 (enabled)\n");
    printf("  replication_type - (Optional) 'paxos' (default) or 'raft'\n");
    printf("\nExamples:\n");
    printf("  %s 2 0 6 localhost 1       # 2-shard leader with Paxos replication\n", program_name);
    printf("  %s 2 0 6 localhost 1 raft  # 2-shard leader with Raft replication\n", program_name);
    printf("  %s 1 0 4 localhost 0       # Single shard, no replication\n", program_name);
}

int main(int argc, char **argv) {
    // Parse command-line arguments
    if (argc < 6 || argc > 7) {
        print_usage(argv[0]);
        return 1;
    }

    int nshards = std::stoi(argv[1]);
    int shardIdx = std::stoi(argv[2]);
    int nthreads = std::stoi(argv[3]);
    std::string paxos_proc_name = std::string(argv[4]);
    int is_replicated = std::stoi(argv[5]);

    // Validate arguments
    if (nshards < 1 || nshards > 16) {
        fprintf(stderr, "Error: nshards must be between 1 and 16\n");
        return 1;
    }
    if (shardIdx < 0 || shardIdx >= nshards) {
        fprintf(stderr, "Error: shardIdx must be between 0 and %d\n", nshards - 1);
        return 1;
    }
    if (nthreads < 1 || nthreads > 64) {
        fprintf(stderr, "Error: nthreads must be between 1 and 64\n");
        return 1;
    }

    // Set replication type if provided (default is paxos)
    std::string replication_type = "paxos";
    if (argc == 7) {
        replication_type = argv[6];
        janus::set_replication_type_from_string(replication_type);
        printf("Using replication type: %s\n", replication_type.c_str());
    }

    // Install signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== Mako Server Starting ===\n");
    printf("Configuration:\n");
    printf("  Shards: %d (this shard: %d)\n", nshards, shardIdx);
    printf("  Threads: %d\n", nthreads);
    printf("  Process role: %s\n", paxos_proc_name.c_str());
    printf("  Replication: %s\n", is_replicated ? (replication_type.c_str()) : "disabled");

    // Build config path
    std::string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards" + std::to_string(nshards)
            + "-warehouses" + std::to_string(nthreads) + ".yml";

    // Use occ_raft.yml for Raft replication, occ_paxos.yml for Paxos
    std::string occ_config = (replication_type == "raft" || replication_type == "RAFT")
        ? "../config/occ_raft.yml"
        : "../config/occ_paxos.yml";

    std::vector<std::string> paxos_config_files{
        get_current_absolute_path() + "../config/1leader_2followers/paxos" + std::to_string(nthreads) + "_shardidx" + std::to_string(shardIdx) + ".yml",
        get_current_absolute_path() + occ_config
    };

    // Configure database options
    mako::Options options;
    options.num_shards = nshards;
    options.shard_index = shardIdx;
    options.num_threads = nthreads;
    options.paxos_proc_name = paxos_proc_name;
    options.paxos_config_files = paxos_config_files;
    options.replication.enabled = (is_replicated != 0);
    options.replication.is_leader = (paxos_proc_name == "localhost");

    // Create transport configuration
    auto transport_config = new transport::Configuration(config_path);
    options.transport_config = transport_config;

    // Open the database
    mako::DB* mako_db = nullptr;
    mako::Status status = mako::DB::Open(options, "/tmp/mako_server", &mako_db);
    if (!status.ok()) {
        std::cerr << "Failed to open database: " << status.ToString() << std::endl;
        return 1;
    }

    printf("Database opened successfully\n");

    // Get the underlying abstract_db for operations
    abstract_db* db = mako_db->GetDB();
    auto& benchConfig = BenchmarkConfig::getInstance();

    // RPC server for client API (using RRR RPC framework)
    rusty::Option<rusty::Arc<rrr::Server>> client_rpc_server = rusty::None;
    rusty::Option<rusty::Arc<rrr::PollThread>> client_poll_thread = rusty::None;

    // Setup RPC server and helper threads (leader nodes only)
    if (benchConfig.getLeaderConfig()) {
        // Start eRPC server for handling client requests
        mako::setup_erpc_server();

        // Pre-declare sharded tables
        mbta_sharded_ordered_index *table = db->open_sharded_index("customer_0");

        map<int, abstract_ordered_index*> open_tables;
        auto *local_table = table->shard_for_index(benchConfig.getShardIndex());
        if (local_table) {
            open_tables[local_table->get_table_id()] = local_table;
        }
        mako::setup_helper(db, std::ref(open_tables));

        // Start RRR RPC server for RemoteDB client connections
        int client_port = 31000 + shardIdx;  // Different port per shard
        std::string client_addr = "0.0.0.0:" + std::to_string(client_port);

        // Create poll thread for RPC server
        client_poll_thread = rusty::Some(rrr::PollThread::create());

        // Create RPC server with poll thread
        auto server = rusty::Arc<rrr::Server>::make(client_poll_thread);

        // Get the ShardReceiver to pass to the service
        ShardReceiver* receiver = mako::get_shard_receiver();
        if (receiver) {
            // Create and register MakoClientService
            auto client_service = rusty::Box<mako::MakoClientService>::make(receiver);
            server->reg_service(std::move(client_service));

            // Start the RPC server
            int ret = server->start(client_addr.c_str());
            if (ret == 0) {
                printf("Client RPC server (RRR) started on %s\n", client_addr.c_str());
                client_rpc_server = rusty::Some(server);
            } else {
                printf("Warning: Failed to start client RPC server (error: %d)\n", ret);
            }
        } else {
            printf("Warning: No ShardReceiver available for client RPC service\n");
        }

        printf("RPC server started, waiting for client connections...\n");
    } else {
        printf("Running as %s, waiting for replication data...\n", paxos_proc_name.c_str());
    }

    // Server main loop - wait for shutdown signal
    printf("\nServer running. Press Ctrl+C to shutdown.\n");
    fflush(stdout);

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    printf("\nShutting down server...\n");

    // Cleanup: stop client RPC server, helper and eRPC server threads
    if (benchConfig.getLeaderConfig()) {
        // Shutdown RRR RPC server for client connections
        if (client_rpc_server.is_some()) {
            client_rpc_server.as_ref().unwrap()->do_shutdown();
        }
        mako::stop_erpc_server();
    }

    db_close();
    delete mako_db;

    printf("Server shutdown complete.\n");
    return 0;
}
