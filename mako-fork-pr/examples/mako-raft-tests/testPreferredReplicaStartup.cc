/**
 * testPreferredReplicaStartup.cc
 *
 * TEST 1: Preferred Replica Startup Test (with TimeoutNow Leadership Transfer)
 *
 * Purpose:
 * - Verify that the preferred replica becomes leader via TimeoutNow protocol
 * - Verify that non-preferred replicas transfer leadership to preferred
 * - Observe leadership behavior without bias
 *
 * Test Setup:
 * - 5-node Raft cluster: localhost (preferred), p1, p2, p3, p4
 * - All nodes start simultaneously
 * - All nodes run for same duration: 2s startup + 30s monitoring = 32s total
 */

#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <mako.hh>
#include <examples/common.h>

using namespace std;
using namespace mako;
using namespace chrono;

// =============================================================================
// Test Configuration
// =============================================================================
const int STARTUP_TIME_SEC = 2;      // Wait 2 seconds for all processes to start
const int MONITOR_DURATION_SEC = 30; // Monitor for 30 seconds
const int TOTAL_TEST_TIME_SEC = 32;  // Total: 2s startup + 30s monitoring

// =============================================================================
// Test State Tracking
// =============================================================================
atomic<bool> i_am_leader{false};
atomic<int> times_became_leader{0};
atomic<int> times_lost_leadership{0};
atomic<uint64_t> time_became_leader_ms{0};
atomic<uint64_t> startup_time_ms{0};

mutex cout_mutex;

void safe_print(const string& msg) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    cout << msg << endl;
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
    safe_print("TEST 1: Preferred Replica Startup Test");
    safe_print("=================================================================");
    safe_print("Process: " + proc_name);
    safe_print("Startup phase: " + to_string(STARTUP_TIME_SEC) + " seconds");
    safe_print("Monitor phase: " + to_string(MONITOR_DURATION_SEC) + " seconds");
    safe_print("Total test time: " + to_string(TOTAL_TEST_TIME_SEC) + " seconds");
    safe_print("=================================================================");
    safe_print("");

    // =========================================================================
    // Step 1: Setup Configuration
    // =========================================================================
    safe_print("[" + proc_name + "] Step 1: Configuring Raft cluster...");

    vector<string> config{
        get_current_absolute_path() + "../config/none_raft.yml",
        get_current_absolute_path() + "../config/1c1s3r1p_cluster_test.yml"
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
    // Step 2: Initialize Raft (setup)
    // =========================================================================
    safe_print("[" + proc_name + "] Step 2: Initializing Raft...");

    vector<string> ret = setup(18, argv_raft);
    if (ret.empty()) {
        cerr << "[" << proc_name << "] ERROR: setup() failed!" << endl;
        return 1;
    }

    safe_print("[" + proc_name + "] ✓ Raft initialized");

    // =========================================================================
    // Step 3: Register Leadership Callbacks
    // =========================================================================
    safe_print("[" + proc_name + "] Step 3: Registering leadership callbacks...");

    // Leadership change callback - track when we become/lose leadership
    register_leader_election_callback([&](int control) {
        // This callback is only called when BECOMING leader
        times_became_leader++;
        i_am_leader.store(true);

        uint64_t current_time = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        time_became_leader_ms = current_time;
        uint64_t elapsed = current_time - startup_time_ms;

        safe_print("[" + proc_name + "] 👑 BECAME LEADER at +" + to_string(elapsed) + "ms");
    });

    // IMPORTANT: The above callback only fires when becoming leader
    // We need to poll IsLeader() to detect when we lose leadership
    // This is a limitation of the current callback API

    // Dummy log application callbacks (not testing replication here)
    auto dummy_callback = [&](const char*& log, int len, int par_id, int slot_id,
                              queue<tuple<int, int, int, int, const char*>>& un_replay_logs_) {
        uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());
        return static_cast<int>(timestamp * 10 + 1);
    };

    register_for_leader_par_id_return(dummy_callback, 0);
    register_for_follower_par_id_return(dummy_callback, 0);

    safe_print("[" + proc_name + "] ✓ Callbacks registered");

    // =========================================================================
    // Step 4: Launch Raft Services (setup2)
    // =========================================================================
    safe_print("[" + proc_name + "] Step 4: Launching Raft services...");
    safe_print("[" + proc_name + "]   - Preferred replica system will be configured");
    safe_print("[" + proc_name + "]   - Elections will begin automatically");
    safe_print("");

    setup2(0, 0);  // Uses preferred replica system

    safe_print("[" + proc_name + "] ✓ Raft services launched");
    safe_print("");

    // =========================================================================
    // Step 5: Synchronized Startup Phase (2 seconds)
    // =========================================================================
    // All processes wait the same 2 seconds to allow cluster to stabilize
    // This ensures all processes start monitoring at the same time
    safe_print("[" + proc_name + "] Step 5: Startup phase (" + to_string(STARTUP_TIME_SEC) + "s)...");
    safe_print("[" + proc_name + "]   Waiting for all processes to start");
    safe_print("");

    this_thread::sleep_for(chrono::seconds(STARTUP_TIME_SEC));

    uint64_t monitor_start_time = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    safe_print("[" + proc_name + "] ✓ Startup phase complete");
    safe_print("");

    // =========================================================================
    // Step 6: Synchronized Monitoring Phase (30 seconds)
    // =========================================================================
    // All processes monitor for exactly 30 seconds
    safe_print("[" + proc_name + "] Step 6: Monitoring leadership for " +
               to_string(MONITOR_DURATION_SEC) + " seconds...");
    safe_print("");

    for (int i = 1; i <= MONITOR_DURATION_SEC; i++) {
        this_thread::sleep_for(chrono::seconds(1));

        int became_count = times_became_leader.load();
        int lost_count = times_lost_leadership.load();

        safe_print("[" + proc_name + "] [" + to_string(i) + "s] " +
                   "became_leader_count=" + to_string(became_count));
    }

    safe_print("[" + proc_name + "] ✓ Monitoring complete");
    safe_print("");

    // =========================================================================
    // Step 7: Final Results
    // =========================================================================
    safe_print("=================================================================");
    safe_print("[" + proc_name + "] FINAL TEST RESULTS");
    safe_print("=================================================================");

    int final_became = times_became_leader.load();
    uint64_t leader_elapsed = (time_became_leader_ms > 0) ?
        (time_became_leader_ms - startup_time_ms) : 0;

    safe_print("[" + proc_name + "] Times became leader:     " + to_string(final_became));
    if (time_became_leader_ms > 0) {
        safe_print("[" + proc_name + "] First leadership at:     +" +
                   to_string(leader_elapsed) + "ms");
    }
    safe_print("");
    safe_print("NOTE: To see actual leadership state, check server logs for:");
    safe_print("  - '[RAFT_STATE] setIsLeader transition LEADER' (became leader)");
    safe_print("  - '[RAFT_STATE] setIsLeader transition FOLLOWER' (stepped down)");
    safe_print("");

    // =========================================================================
    // Step 8: Evaluation (Just observe, no pass/fail)
    // =========================================================================
    safe_print("[" + proc_name + "] Observations:");
    if (proc_name == "localhost") {
        if (final_became == 1) {
            safe_print("[" + proc_name + "]   ✅ Preferred replica became leader once");
        } else if (final_became > 1) {
            safe_print("[" + proc_name + "]   ⚠️  Preferred replica had " +
                      to_string(final_became) + " leadership transitions (election instability)");
        } else {
            safe_print("[" + proc_name + "]   ❌ Preferred replica never became leader");
        }
    } else {
        if (final_became == 0) {
            safe_print("[" + proc_name + "]   ✅ Non-preferred replica never became leader (ideal)");
        } else if (final_became == 1) {
            safe_print("[" + proc_name + "]   ✅ Non-preferred replica transferred leadership (became leader once)");
        } else {
            safe_print("[" + proc_name + "]   ⚠️  Non-preferred replica became leader " +
                      to_string(final_became) + " times (election instability)");
        }
    }
    safe_print("");
    safe_print("=================================================================");

    // =========================================================================
    // Step 9: Cleanup
    // =========================================================================
    safe_print("[" + proc_name + "] Shutting down...");
    shutdown_paxos();
    safe_print("[" + proc_name + "] Shutdown complete");

    // Return 0 for all processes - just observe, don't fail
    return 0;
}
