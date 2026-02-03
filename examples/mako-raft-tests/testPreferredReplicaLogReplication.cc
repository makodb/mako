/**
 * testPreferredReplicaLogReplication.cc
 *
 * TEST: Preferred Replica Log Replication Test
 *
 * Purpose:
 * - Verify that logs submitted to Raft are replicated to all replicas
 * - Test batching behavior with TpcCommitCommand wrapping
 * - Verify preferred leader accepts and replicates 25 logs
 * - Ensure all 5 replicas apply all 25 logs via callbacks
 *
 * Test Setup:
 * - 5-node Raft cluster: localhost (preferred), p1, p2, p3, p4
 * - Phase 1: Startup (2s) - cluster stabilization
 * - Phase 2: Wait for leader election
 * - Phase 3: Leader submits 25 logs wrapped in TpcCommitCommand
 * - Phase 4: Wait for replication (5s)
 * - Phase 5: Verify all replicas applied all logs
 * - Phase 6: Shutdown
 */

#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <mako.hh>
#include <examples/common.h>
#include "../src/deptran/classic/tpc_command.h"  // TpcCommitCommand
#include "../src/deptran/procedure.h"            // VecPieceData, SimpleCommand

using namespace std;
using namespace mako;
using namespace chrono;
using namespace janus;

// =============================================================================
// Test Configuration
// =============================================================================
const int STARTUP_TIME_SEC = 2;       // Wait for cluster stabilization
const int LEADER_WAIT_SEC = 3;        // Wait for leader election
const int REPLICATION_WAIT_SEC = 5;   // Wait for logs to replicate
const int NUM_LOGS = 25;              // Number of logs to submit
const int BATCH_SIZE = 5;             // Batching parameter

// =============================================================================
// Test State Tracking
// =============================================================================
atomic<bool> i_am_leader{false};
atomic<int> logs_applied_count{0};
atomic<int> logs_submitted_count{0};
atomic<uint64_t> first_log_applied_time{0};
atomic<uint64_t> last_log_applied_time{0};
atomic<uint64_t> startup_time_ms{0};

mutex cout_mutex;

void safe_print(const string& msg) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    cout << msg << endl;
}

// =============================================================================
// Helper: Create TpcCommitCommand wrapping raw log bytes
// =============================================================================
shared_ptr<TpcCommitCommand> create_log_command(const char* log_data, int length, txnid_t tx_id) {
    // Create TpcCommitCommand (outer wrapper for batch optimization)
    auto tpc_cmd = make_shared<TpcCommitCommand>();
    tpc_cmd->tx_id_ = tx_id;

    // Create VecPieceData (inner container)
    auto vpd = make_shared<VecPieceData>();
    vpd->sp_vec_piece_data_ = make_shared<vector<shared_ptr<SimpleCommand>>>();

    // Create SimpleCommand to hold the raw payload
    auto simple_cmd = make_shared<SimpleCommand>();

    // Store raw bytes as STRING value at key=0
    simple_cmd->input.values_ = make_shared<map<int32_t, Value>>();
    (*simple_cmd->input.values_)[0] = Value(string(log_data, length));
    simple_cmd->input.keys_.insert(0);
    simple_cmd->partition_id_ = 0;

    // Assemble: TpcCommitCommand → VecPieceData → SimpleCommand
    vpd->sp_vec_piece_data_->push_back(simple_cmd);
    tpc_cmd->cmd_ = vpd;

    return tpc_cmd;
}

// =============================================================================
// Helper: Serialize TpcCommitCommand to bytes
// =============================================================================
string serialize_tpc_command(shared_ptr<TpcCommitCommand> cmd) {
    // For this test, we'll use a simple format:
    // [tx_id(8 bytes)][log_data_length(4 bytes)][log_data]

    ostringstream oss;

    // Write tx_id
    uint64_t tx_id = cmd->tx_id_;
    oss.write(reinterpret_cast<const char*>(&tx_id), sizeof(tx_id));

    // Extract log data from SimpleCommand
    auto vpd = dynamic_pointer_cast<VecPieceData>(cmd->cmd_);
    if (vpd && vpd->sp_vec_piece_data_ && !vpd->sp_vec_piece_data_->empty()) {
        auto simple_cmd = (*vpd->sp_vec_piece_data_)[0];
        if (simple_cmd->input.values_) {
            auto it = simple_cmd->input.values_->find(0);
            if (it != simple_cmd->input.values_->end()) {
                string log_data = it->second.get_str();
                uint32_t len = log_data.length();

                // Write length
                oss.write(reinterpret_cast<const char*>(&len), sizeof(len));

                // Write data
                oss.write(log_data.c_str(), len);
            }
        }
    }

    return oss.str();
}

// =============================================================================
// Main Test
// =============================================================================
int main(int argc, char **argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <process_name>" << endl;
        cerr << "  process_name: localhost, p1, p2, p3, or p4" << endl;
        return 1;
    }

    string proc_name = string(argv[1]);
    startup_time_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    safe_print("=================================================================");
    safe_print("TEST: Preferred Replica Log Replication Test");
    safe_print("=================================================================");
    safe_print("Process: " + proc_name);
    safe_print("Logs to submit: " + to_string(NUM_LOGS));
    safe_print("Batch size: " + to_string(BATCH_SIZE));
    safe_print("=================================================================");
    safe_print("");

    // =========================================================================
    // Step 1: Setup Configuration
    // =========================================================================
    safe_print("[" + proc_name + "] Step 1: Configuring Raft cluster...");

    vector<string> config{
        get_current_absolute_path() + "../config/none_raft.yml",
        get_current_absolute_path() + "../config/1c1s5r1p_cluster_test.yml"
    };

    char *argv_raft[18];
    argv_raft[0] = (char *)"";
    argv_raft[1] = (char *)"-b";
    argv_raft[2] = (char *)"-d";
    argv_raft[3] = (char *)"60";
    argv_raft[4] = (char *)"-f";
    argv_raft[5] = (char *) config[0].c_str();
    argv_raft[6] = (char *)"-f";
    argv_raft[7] = (char *) config[1].c_str();
    argv_raft[8] = (char *)"-t";
    argv_raft[9] = (char *)"30";
    argv_raft[10] = (char *)"-T";
    argv_raft[11] = (char *)"100000";
    argv_raft[12] = (char *)"-n";
    argv_raft[13] = (char *)"32";
    argv_raft[14] = (char *)"-P";
    argv_raft[15] = (char *) proc_name.c_str();
    argv_raft[16] = (char *)"-A";
    argv_raft[17] = (char *)"10000";

    safe_print("[" + proc_name + "] ✓ Configuration prepared");

    // =========================================================================
    // Step 2: Initialize Raft
    // =========================================================================
    safe_print("[" + proc_name + "] Step 2: Initializing Raft...");

    vector<string> ret = setup(18, argv_raft);
    if (ret.empty()) {
        cerr << "[" << proc_name << "] ERROR: setup() failed!" << endl;
        return 1;
    }

    safe_print("[" + proc_name + "] ✓ Raft initialized");

    // =========================================================================
    // Step 3: Register Callbacks
    // =========================================================================
    safe_print("[" + proc_name + "] Step 3: Registering callbacks...");

    // Leadership callback (called for both gaining AND losing leadership)
    register_leader_election_callback([&](int control) {
        uint64_t now = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        uint64_t elapsed = now - startup_time_ms;

        if (control == 1) {
            // Became leader
            i_am_leader.store(true);
            safe_print("[" + proc_name + "] 👑 BECAME LEADER at +" + to_string(elapsed) + "ms");
        } else {
            // Lost leadership
            i_am_leader.store(false);
            safe_print("[" + proc_name + "] 👥 LOST LEADERSHIP at +" + to_string(elapsed) + "ms");
        }
    });

    // Log application callback (unified for leader and follower)
    auto log_callback = [&](const char*& log, int len, int par_id, int slot_id,
                            queue<tuple<int, int, int, int, const char*>>& un_replay_logs_) {
        // slot_id starts from 1 (slot 0 is reserved)
        if (slot_id >= 1) {
            int current_count = ++logs_applied_count;

            uint64_t now = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();

            if (current_count == 1) {
                first_log_applied_time = now;
            }
            last_log_applied_time = now;

            uint64_t elapsed = now - startup_time_ms;

            safe_print("[" + proc_name + "] 📝 Applied log " + to_string(current_count) +
                      " (slot=" + to_string(slot_id) + ", len=" + to_string(len) +
                      ") at +" + to_string(elapsed) + "ms");
        }

        uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());
        return static_cast<int>(timestamp * 10 + 1);
    };

    register_for_leader_par_id_return(log_callback, 0);
    register_for_follower_par_id_return(log_callback, 0);

    safe_print("[" + proc_name + "] ✓ Callbacks registered");

    // =========================================================================
    // Step 4: Launch Raft Services
    // =========================================================================
    safe_print("[" + proc_name + "] Step 4: Launching Raft services...");
    safe_print("");

    setup2(0, 0);

    safe_print("[" + proc_name + "] ✓ Raft services launched");
    safe_print("");

    // =========================================================================
    // Step 5: Startup Phase
    // =========================================================================
    safe_print("[" + proc_name + "] Step 5: Startup phase (" + to_string(STARTUP_TIME_SEC) + "s)...");
    this_thread::sleep_for(chrono::seconds(STARTUP_TIME_SEC));
    safe_print("[" + proc_name + "] ✓ Startup complete");
    safe_print("");

    // =========================================================================
    // Step 6: Wait for Leader Election
    // =========================================================================
    safe_print("[" + proc_name + "] Step 6: Waiting for leader election (" +
               to_string(LEADER_WAIT_SEC) + "s)...");
    this_thread::sleep_for(chrono::seconds(LEADER_WAIT_SEC));

    bool is_leader = i_am_leader.load();
    if (is_leader) {
        safe_print("[" + proc_name + "] ✓ I am the LEADER");
    } else {
        safe_print("[" + proc_name + "] ✓ I am a FOLLOWER");
    }
    safe_print("");

    // =========================================================================
    // Step 7: Submit Logs (Leader Only)
    // =========================================================================
    if (is_leader) {
        safe_print("[" + proc_name + "] Step 7: Submitting " + to_string(NUM_LOGS) +
                   " logs with TpcCommitCommand wrapping...");
        safe_print("");

        for (int i = 1; i <= NUM_LOGS; i++) {
            // Create log content
            ostringstream log_stream;
            log_stream << "LOG_ENTRY_" << setfill('0') << setw(3) << i;
            string log_content = log_stream.str();

            // Wrap in TpcCommitCommand
            txnid_t tx_id = 1000 + i;
            auto tpc_cmd = create_log_command(log_content.c_str(), log_content.length(), tx_id);

            // Serialize to bytes
            string serialized = serialize_tpc_command(tpc_cmd);

            // Submit to Raft with batching
            add_log_to_nc(serialized.c_str(), serialized.length(), 0, BATCH_SIZE);
            logs_submitted_count++;

            uint64_t now = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
            uint64_t elapsed = now - startup_time_ms;

            safe_print("[" + proc_name + "] 📤 Submitted log " + to_string(i) +
                      " (tx_id=" + to_string(tx_id) + ", len=" + to_string(serialized.length()) +
                      ") at +" + to_string(elapsed) + "ms");

            // Small delay between submissions to observe batching
            this_thread::sleep_for(chrono::milliseconds(10));
        }

        safe_print("");
        safe_print("[" + proc_name + "] ✓ Submitted " + to_string(logs_submitted_count.load()) + " logs");
    } else {
        safe_print("[" + proc_name + "] Step 7: Not leader - skipping log submission");
    }
    safe_print("");

    // =========================================================================
    // Step 8: Wait for Replication (Exit immediately when complete)
    // =========================================================================
    safe_print("[" + proc_name + "] Step 8: Waiting for replication (max " +
               to_string(REPLICATION_WAIT_SEC) + "s)...");

    // Poll every 100ms until all logs applied or timeout
    int checks = 0;
    int max_checks = REPLICATION_WAIT_SEC * 10;  // 5s * 10 = 50 checks

    while (checks < max_checks) {
        int current_applied = logs_applied_count.load();

        // Exit immediately once all logs are applied
        if (current_applied >= NUM_LOGS) {
            safe_print("[" + proc_name + "] ✓ All " + to_string(NUM_LOGS) +
                      " logs replicated! (took " + to_string(checks * 100) + "ms)");
            break;
        }

        this_thread::sleep_for(chrono::milliseconds(100));
        checks++;

        // Print progress every second (10 checks)
        if (checks % 10 == 0) {
            safe_print("[" + proc_name + "] [" + to_string(checks / 10) + "s] logs_applied=" +
                       to_string(current_applied) + "/" + to_string(NUM_LOGS));
        }
    }
    safe_print("");

    // =========================================================================
    // Step 9: Stabilization Period - Let Raft Settle
    // =========================================================================
    safe_print("[" + proc_name + "] Step 9: Stabilization period (2s) - letting Raft settle...");
    this_thread::sleep_for(chrono::seconds(2));

    // Re-check leadership status after stabilization
    bool final_is_leader = i_am_leader.load();
    safe_print("[" + proc_name + "] ✓ Stabilization complete - Final role: " +
               string(final_is_leader ? "LEADER" : "FOLLOWER"));
    safe_print("");

    // =========================================================================
    // Step 10: Final Results and Verification
    // =========================================================================
    safe_print("=================================================================");
    safe_print("[" + proc_name + "] FINAL TEST RESULTS");
    safe_print("=================================================================");

    int final_applied = logs_applied_count.load();
    int final_submitted = logs_submitted_count.load();

    safe_print("[" + proc_name + "] Role:              " + string(final_is_leader ? "LEADER" : "FOLLOWER"));
    safe_print("[" + proc_name + "] Logs submitted:    " + to_string(final_submitted));
    safe_print("[" + proc_name + "] Logs applied:      " + to_string(final_applied) + "/" + to_string(NUM_LOGS));

    if (first_log_applied_time > 0 && last_log_applied_time > 0) {
        uint64_t replication_duration = last_log_applied_time - first_log_applied_time;
        safe_print("[" + proc_name + "] Replication time:  " + to_string(replication_duration) + "ms");

        if (final_applied > 1) {
            double throughput = (final_applied - 1) * 1000.0 / replication_duration;
            safe_print("[" + proc_name + "] Throughput:        " + to_string(throughput) + " logs/sec");
        }
    }
    safe_print("");

    // Verification
    if (final_applied >= NUM_LOGS) {
        safe_print("[" + proc_name + "] ✅ PASS: All " + to_string(NUM_LOGS) + " logs replicated successfully!");
    } else {
        safe_print("[" + proc_name + "] ❌ FAIL: Only " + to_string(final_applied) + "/" +
                   to_string(NUM_LOGS) + " logs applied!");
    }
    safe_print("");
    safe_print("=================================================================");

    // =========================================================================
    // Step 11: Fast Exit (Skip clean shutdown to avoid hangs)
    // =========================================================================
    // We've verified replication works - just exit immediately
    // Clean shutdown has known issues (election storms, deadlocks)
    // For this test, we only care about replication correctness

    int exit_code = (final_applied >= NUM_LOGS) ? 0 : 1;

    if (exit_code == 0) {
        safe_print("[" + proc_name + "] Exiting with SUCCESS (exit code 0)");
    } else {
        safe_print("[" + proc_name + "] Exiting with FAILURE (exit code 1)");
    }

    // Force exit immediately without cleanup
    _exit(exit_code);
}
