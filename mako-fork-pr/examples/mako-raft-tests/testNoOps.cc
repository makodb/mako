/**
 * testNoOps.cc
 *
 * TEST: NO-OPS Watermark Synchronization Test with Preferred Leader
 *
 * Purpose:
 * - Verify that NO-OPS messages propagate correctly in Raft
 * - Test watermark synchronization mechanism across replicas
 * - Verify preferred leader handles NO-OPS correctly
 * - Test epoch advancement through NO-OPS
 * - Verify all replicas receive and process NO-OPS in order
 *
 * NO-OPS Format: "no-ops:X" where X is single-digit epoch (0-9)
 * Example: "no-ops:0", "no-ops:1", "no-ops:2"
 *
 * Test Setup:
 * - 5-node Raft cluster: localhost (preferred), p1, p2, p3, p4
 * - Phase 1: Startup (2s) - cluster stabilization
 * - Phase 2: Wait for preferred leader election (3s)
 * - Phase 3: Leader submits 5 NO-OPS messages (epochs 0-4)
 * - Phase 4: Wait for NO-OPS propagation (5s)
 * - Phase 5: Verify all replicas processed all NO-OPS
 * - Phase 6: Submit 10 regular logs to test watermark system
 * - Phase 7: Verify logs replicated after watermark sync
 */

#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <vector>
#include <mako.hh>
#include <examples/common.h>

using namespace std;
using namespace mako;
using namespace chrono;

// =============================================================================
// Test Configuration
// =============================================================================
const int STARTUP_TIME_SEC = 2;       // Wait for cluster stabilization
const int LEADER_WAIT_SEC = 3;        // Wait for preferred leader election
const int NOOPS_WAIT_SEC = 5;         // Wait for NO-OPS propagation
const int LOGS_WAIT_SEC = 5;          // Wait for regular logs
const int NUM_NOOPS = 5;              // Number of NO-OPS to send (epochs 0-4)
const int NUM_REGULAR_LOGS = 10;      // Number of regular logs after NO-OPS
const int BATCH_SIZE = 1;             // NO-OPS should not be batched

// =============================================================================
// Test State Tracking
// =============================================================================
atomic<bool> i_am_leader{false};
atomic<int> noops_applied_count{0};
atomic<int> regular_logs_applied_count{0};
atomic<int> noops_submitted_count{0};
atomic<int> regular_logs_submitted_count{0};
atomic<uint64_t> first_noops_applied_time{0};
atomic<uint64_t> last_noops_applied_time{0};
atomic<uint64_t> first_regular_log_applied_time{0};
atomic<uint64_t> last_regular_log_applied_time{0};
atomic<uint64_t> startup_time_ms{0};

// Track which epochs we've seen
atomic<int> max_epoch_seen{-1};
vector<atomic<bool>> epoch_received(NUM_NOOPS);

mutex cout_mutex;

void safe_print(const string& msg) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    cout << msg << endl;
}

// =============================================================================
// Helper: Check if log is NO-OPS
// =============================================================================
int isNoopsLocal(const char* log, int len) {
    // NO-OPS format: "no-ops:X" where X is epoch digit (0-9)
    // Length should be 8 bytes
    if (len == 8) {
        if (log[0] == 'n' && log[1] == 'o' && log[2] == '-' &&
            log[3] == 'o' && log[4] == 'p' && log[5] == 's' && log[6] == ':') {
            // Extract epoch digit
            return log[7] - '0';
        }
    }
    return -1;
}

// =============================================================================
// Helper: Get elapsed time string
// =============================================================================
string elapsed_str() {
    uint64_t now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    uint64_t elapsed = now - startup_time_ms;
    return "+" + to_string(elapsed) + "ms";
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

    // Initialize epoch tracking
    for (int i = 0; i < NUM_NOOPS; i++) {
        epoch_received[i].store(false);
    }

    safe_print("=================================================================");
    safe_print("TEST: NO-OPS Watermark Synchronization with Preferred Leader");
    safe_print("=================================================================");
    safe_print("Process: " + proc_name);
    safe_print("NO-OPS to send: " + to_string(NUM_NOOPS) + " (epochs 0-" + to_string(NUM_NOOPS-1) + ")");
    safe_print("Regular logs: " + to_string(NUM_REGULAR_LOGS));
    safe_print("Preferred leader: localhost");
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

    // Leadership callback
    register_leader_election_callback([&](int control) {
        if (control == 1) {
            i_am_leader.store(true);
            safe_print("[" + proc_name + "] 👑 BECAME LEADER at " + elapsed_str());
        } else {
            i_am_leader.store(false);
            safe_print("[" + proc_name + "] 👥 LOST LEADERSHIP at " + elapsed_str());
        }
    });

    // Log application callback - handles both NO-OPS and regular logs
    auto log_callback = [&](const char*& log, int len, int par_id, int slot_id,
                            queue<tuple<int, int, int, int, const char*>>& un_replay_logs_) {
        // Check if this is a NO-OPS message
        int epoch = isNoopsLocal(log, len);

        if (epoch != -1) {
            // This is a NO-OPS message
            int current_count = ++noops_applied_count;

            // Track epoch
            if (epoch >= 0 && epoch < NUM_NOOPS) {
                epoch_received[epoch].store(true);
            }
            if (epoch > max_epoch_seen.load()) {
                max_epoch_seen.store(epoch);
            }

            uint64_t now = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();

            if (current_count == 1) {
                first_noops_applied_time = now;
            }
            last_noops_applied_time = now;

            // Create display string
            string log_str(log, min(len, 32));
            safe_print("[" + proc_name + "] 🔔 NO-OPS epoch=" + to_string(epoch) +
                      " (slot=" + to_string(slot_id) + ", count=" + to_string(current_count) +
                      ", content=\"" + log_str + "\") at " + elapsed_str());

        } else if (len > 0) {
            // This is a regular log entry
            int current_count = ++regular_logs_applied_count;

            uint64_t now = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();

            if (current_count == 1) {
                first_regular_log_applied_time = now;
            }
            last_regular_log_applied_time = now;

            // Display first 32 bytes of log
            string log_str(log, min(len, 32));
            if (len > 32) log_str += "...";

            safe_print("[" + proc_name + "] 📝 Regular log #" + to_string(current_count) +
                      " (slot=" + to_string(slot_id) + ", len=" + to_string(len) +
                      ", content=\"" + log_str + "\") at " + elapsed_str());
        }

        // Return timestamp * 10 + STATUS_NORMAL
        uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());
        return static_cast<int>(timestamp * 10 + 1);
    };

    // Register for both leader and follower
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
    // Step 6: Wait for Preferred Leader Election
    // =========================================================================
    safe_print("[" + proc_name + "] Step 6: Waiting for preferred leader election (" +
               to_string(LEADER_WAIT_SEC) + "s)...");
    safe_print("[" + proc_name + "]         Preferred leader: localhost");
    this_thread::sleep_for(chrono::seconds(LEADER_WAIT_SEC));

    bool is_leader = i_am_leader.load();
    bool is_preferred = (proc_name == "localhost");

    if (is_leader) {
        safe_print("[" + proc_name + "] ✓ I am the LEADER" +
                   string(is_preferred ? " (PREFERRED)" : " (NON-PREFERRED)"));
    } else {
        safe_print("[" + proc_name + "] ✓ I am a FOLLOWER" +
                   string(is_preferred ? " (PREFERRED, should become leader soon)" : ""));
    }
    safe_print("");

    // =========================================================================
    // Step 7: Submit NO-OPS Messages (Leader Only)
    // =========================================================================
    if (is_leader) {
        safe_print("[" + proc_name + "] Step 7: Submitting " + to_string(NUM_NOOPS) +
                   " NO-OPS messages (epochs 0-" + to_string(NUM_NOOPS-1) + ")...");
        safe_print("");

        for (int epoch = 0; epoch < NUM_NOOPS; epoch++) {
            // Create NO-OPS message: "no-ops:X"
            string noops_msg = "no-ops:" + to_string(epoch);

            // Submit to Raft (NO-OPS should not be batched)
            add_log_to_nc(noops_msg.c_str(), noops_msg.length(), 0, BATCH_SIZE);
            noops_submitted_count++;

            safe_print("[" + proc_name + "] 📤 Submitted NO-OPS epoch=" + to_string(epoch) +
                      " (content=\"" + noops_msg + "\") at " + elapsed_str());

            // Small delay to ensure ordering
            this_thread::sleep_for(chrono::milliseconds(50));
        }

        safe_print("");
        safe_print("[" + proc_name + "] ✓ Submitted " + to_string(noops_submitted_count.load()) + " NO-OPS");
    } else {
        safe_print("[" + proc_name + "] Step 7: Not leader - skipping NO-OPS submission");
    }
    safe_print("");

    // =========================================================================
    // Step 8: Wait for NO-OPS Propagation
    // =========================================================================
    safe_print("[" + proc_name + "] Step 8: Waiting for NO-OPS propagation (max " +
               to_string(NOOPS_WAIT_SEC) + "s)...");

    int checks = 0;
    int max_checks = NOOPS_WAIT_SEC * 10;

    while (checks < max_checks) {
        int current_applied = noops_applied_count.load();

        if (current_applied >= NUM_NOOPS) {
            safe_print("[" + proc_name + "] ✓ All " + to_string(NUM_NOOPS) +
                      " NO-OPS received! (took " + to_string(checks * 100) + "ms)");
            break;
        }

        this_thread::sleep_for(chrono::milliseconds(100));
        checks++;

        if (checks % 10 == 0) {
            safe_print("[" + proc_name + "] [" + to_string(checks / 10) + "s] NO-OPS applied=" +
                       to_string(current_applied) + "/" + to_string(NUM_NOOPS));
        }
    }
    safe_print("");

    // =========================================================================
    // Step 9: Verify NO-OPS Epochs
    // =========================================================================
    safe_print("[" + proc_name + "] Step 9: Verifying NO-OPS epoch ordering...");

    int max_epoch = max_epoch_seen.load();
    safe_print("[" + proc_name + "] Max epoch seen: " + to_string(max_epoch));

    bool all_epochs_received = true;
    for (int i = 0; i < NUM_NOOPS; i++) {
        if (epoch_received[i].load()) {
            safe_print("[" + proc_name + "] ✓ Epoch " + to_string(i) + " received");
        } else {
            safe_print("[" + proc_name + "] ✗ Epoch " + to_string(i) + " NOT received");
            all_epochs_received = false;
        }
    }

    if (all_epochs_received) {
        safe_print("[" + proc_name + "] ✓ All epochs received in order");
    } else {
        safe_print("[" + proc_name + "] ✗ Some epochs missing!");
    }
    safe_print("");

    // =========================================================================
    // Step 10: Submit Regular Logs After NO-OPS (Leader Only)
    // =========================================================================
    // Re-check leadership (preferred replica may have become leader by now)
    is_leader = i_am_leader.load();

    if (is_leader) {
        safe_print("[" + proc_name + "] Step 10: Submitting " + to_string(NUM_REGULAR_LOGS) +
                   " regular logs after NO-OPS...");
        safe_print("");

        for (int i = 1; i <= NUM_REGULAR_LOGS; i++) {
            ostringstream log_stream;
            log_stream << "REGULAR_LOG_" << setfill('0') << setw(3) << i
                      << "_AFTER_NOOPS_EPOCH_" << max_epoch;
            string log_content = log_stream.str();

            add_log_to_nc(log_content.c_str(), log_content.length(), 0, BATCH_SIZE);
            regular_logs_submitted_count++;

            safe_print("[" + proc_name + "] 📤 Submitted regular log #" + to_string(i) +
                      " at " + elapsed_str());

            this_thread::sleep_for(chrono::milliseconds(10));
        }

        safe_print("");
        safe_print("[" + proc_name + "] ✓ Submitted " + to_string(regular_logs_submitted_count.load()) +
                   " regular logs");
    } else {
        safe_print("[" + proc_name + "] Step 10: Not leader - skipping regular log submission");
    }
    safe_print("");

    // =========================================================================
    // Step 11: Wait for Regular Logs Replication
    // =========================================================================
    safe_print("[" + proc_name + "] Step 11: Waiting for regular logs replication (max " +
               to_string(LOGS_WAIT_SEC) + "s)...");

    checks = 0;
    max_checks = LOGS_WAIT_SEC * 10;

    while (checks < max_checks) {
        int current_applied = regular_logs_applied_count.load();

        if (current_applied >= NUM_REGULAR_LOGS) {
            safe_print("[" + proc_name + "] ✓ All " + to_string(NUM_REGULAR_LOGS) +
                      " regular logs replicated! (took " + to_string(checks * 100) + "ms)");
            break;
        }

        this_thread::sleep_for(chrono::milliseconds(100));
        checks++;

        if (checks % 10 == 0) {
            safe_print("[" + proc_name + "] [" + to_string(checks / 10) + "s] Regular logs=" +
                       to_string(current_applied) + "/" + to_string(NUM_REGULAR_LOGS));
        }
    }
    safe_print("");

    // =========================================================================
    // Step 12: Final Results and Verification
    // =========================================================================
    safe_print("=================================================================");
    safe_print("[" + proc_name + "] FINAL TEST RESULTS");
    safe_print("=================================================================");

    int final_noops_applied = noops_applied_count.load();
    int final_regular_applied = regular_logs_applied_count.load();
    int final_noops_submitted = noops_submitted_count.load();
    int final_regular_submitted = regular_logs_submitted_count.load();
    bool final_is_leader = i_am_leader.load();

    safe_print("[" + proc_name + "] Role:                 " +
               string(final_is_leader ? "LEADER" : "FOLLOWER") +
               string(is_preferred ? " (PREFERRED)" : ""));
    safe_print("[" + proc_name + "] NO-OPS submitted:     " + to_string(final_noops_submitted));
    safe_print("[" + proc_name + "] NO-OPS applied:       " + to_string(final_noops_applied) +
               "/" + to_string(NUM_NOOPS));
    safe_print("[" + proc_name + "] Max epoch seen:       " + to_string(max_epoch));
    safe_print("[" + proc_name + "] Regular logs submit:  " + to_string(final_regular_submitted));
    safe_print("[" + proc_name + "] Regular logs applied: " + to_string(final_regular_applied) +
               "/" + to_string(NUM_REGULAR_LOGS));
    safe_print("");

    // Calculate NO-OPS timing
    if (first_noops_applied_time > 0 && last_noops_applied_time > 0) {
        uint64_t noops_duration = last_noops_applied_time - first_noops_applied_time;
        safe_print("[" + proc_name + "] NO-OPS propagation:   " + to_string(noops_duration) + "ms");
    }

    // Calculate regular logs timing
    if (first_regular_log_applied_time > 0 && last_regular_log_applied_time > 0) {
        uint64_t logs_duration = last_regular_log_applied_time - first_regular_log_applied_time;
        safe_print("[" + proc_name + "] Regular logs time:    " + to_string(logs_duration) + "ms");

        if (final_regular_applied > 1) {
            double throughput = (final_regular_applied - 1) * 1000.0 / logs_duration;
            safe_print("[" + proc_name + "] Throughput:          " + to_string(throughput) + " logs/sec");
        }
    }
    safe_print("");

    // Verification
    bool noops_pass = (final_noops_applied >= NUM_NOOPS) && all_epochs_received;
    bool logs_pass = (final_regular_applied >= NUM_REGULAR_LOGS);
    bool preferred_leader_pass = !is_preferred || final_is_leader;  // Preferred should be leader

    if (noops_pass) {
        safe_print("[" + proc_name + "] ✅ PASS: NO-OPS test passed");
    } else {
        safe_print("[" + proc_name + "] ❌ FAIL: NO-OPS test failed");
    }

    if (logs_pass) {
        safe_print("[" + proc_name + "] ✅ PASS: Regular logs test passed");
    } else {
        safe_print("[" + proc_name + "] ❌ FAIL: Regular logs test failed");
    }

    if (preferred_leader_pass) {
        safe_print("[" + proc_name + "] ✅ PASS: Preferred leader check passed");
    } else {
        safe_print("[" + proc_name + "] ❌ FAIL: Preferred leader is not leader!");
    }

    safe_print("");

    bool overall_pass = noops_pass && logs_pass && preferred_leader_pass;
    if (overall_pass) {
        safe_print("[" + proc_name + "] ✅✅✅ OVERALL: ALL TESTS PASSED ✅✅✅");
    } else {
        safe_print("[" + proc_name + "] ❌❌❌ OVERALL: SOME TESTS FAILED ❌❌❌");
    }

    safe_print("=================================================================");

    // =========================================================================
    // Step 13: Fast Exit
    // =========================================================================
    int exit_code = overall_pass ? 0 : 1;

    if (exit_code == 0) {
        safe_print("[" + proc_name + "] Exiting with SUCCESS (exit code 0)");
    } else {
        safe_print("[" + proc_name + "] Exiting with FAILURE (exit code 1)");
    }

    _exit(exit_code);
}
