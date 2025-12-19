#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <mako.hh>
#include <examples/common.h>

using namespace std;
using namespace mako;

// 1 shard server with 3 worker threads (3 Raft streams) - matches Paxos test
const int num_workers = 3;        // 3 partitions (same as Paxos)
const int message_count = 100;    // 100 logs per partition (same as Paxos)

int end_received = 0;
int end_received_leader = 0;
unordered_map<int, int> counters;

void db_worker(size_t par_id) {
    size_t sent = 0;
    const int base = 3 * 1000;  // 3KB base (same as Paxos)
    util::timer t;
    unsigned char *LOG = (unsigned char *)malloc(base + 200);
    int log_id = 0;

    for (int i=0; i<message_count; i++) {
        sent++;
        int size = rand() % 100 + base;
        log_id++;

        string id = mako::intToString(log_id * 10 + par_id);
        string tt = mako::intToString(mako::getCurrentTimeMillis());

        memcpy(LOG, id.c_str(), min(16, (int)id.size()));
        memcpy(LOG + 16, tt.c_str(), min(16, (int)tt.size()));
        memset(LOG + 32, 'i', size - 32);

        add_log_to_nc((char const *)LOG, size, par_id);

        usleep(5 * 1000);  // 5ms between submits (same as Paxos)
    }
    counters[par_id] = sent;
    free(LOG);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <process_name>" << endl;
        cerr << "  process_name: localhost, p1, p2, learner" << endl;
        return 1;
    }

    string raft_proc_name = std::string(argv[1]);
    int leader_config = (raft_proc_name == "localhost") ? 1 : 0;

    cout << "[" << raft_proc_name << "] Starting simpleRaft test (leader=" << leader_config << ")" << endl;

    vector<string> raft_config{
        get_current_absolute_path() + "../config/none_raft.yml",
        get_current_absolute_path() + "../config/1c1s3r3p_cluster_test.yml"
    };

    char *argv_raft[18];
    argv_raft[0] = (char *)"";
    argv_raft[1] = (char *)"-b";
    argv_raft[2] = (char *)"-d";
    argv_raft[3] = (char *)"60";
    argv_raft[4] = (char *)"-f";
    argv_raft[5] = (char *) raft_config[0].c_str();
    argv_raft[6] = (char *)"-f";
    argv_raft[7] = (char *) raft_config[1].c_str();
    argv_raft[8] = (char *)"-t";
    argv_raft[9] = (char *)"30";
    argv_raft[10] = (char *)"-T";
    argv_raft[11] = (char *)"100000";
    argv_raft[12] = (char *)"-n";
    argv_raft[13] = (char *)"32";
    argv_raft[14] = (char *)"-P";
    argv_raft[15] = (char *) raft_proc_name.c_str();
    argv_raft[16] = (char *)"-A";
    argv_raft[17] = (char *)"10000";

    std::vector<string> ret = setup(18, argv_raft);
    if (ret.empty()) {
        cerr << "[" << raft_proc_name << "] ERROR: setup() failed!" << endl;
        return -1;
    }

    cout << "[" << raft_proc_name << "] Raft initialized successfully" << endl;

    register_leader_election_callback([&](int control) {
        if (control == 1) {
            cout << "[" << raft_proc_name << "] BECAME LEADER" << endl;
        } else {
            cout << "[" << raft_proc_name << "] LOST LEADERSHIP" << endl;
        }
    });

    std::atomic<int> lCnt(0), fCnt(0);

    for (size_t i = 0; i < num_workers; i++) {
        counters[i] = 0;

        // Leader callback
        register_for_leader_par_id_return([&lCnt](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>>& un_replay_logs_) {
            int status = (len < 10 && len > 0) ? mako::PaxosStatus::STATUS_NOOPS : mako::PaxosStatus::STATUS_NORMAL;
            uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());

            if (len == 0) end_received_leader++;

            lCnt++;
            return timestamp * 10 + status;
        }, i);

        // Follower callback
        register_for_follower_par_id_return([&fCnt](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>>& un_replay_logs_) {
            int status = (len < 10 && len > 0) ? mako::PaxosStatus::STATUS_NOOPS : mako::PaxosStatus::STATUS_NORMAL;
            uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());

            if (len == 0) end_received++;

            fCnt++;
            return timestamp * 10 + status;
        }, i);
    }

    cout << "[" << raft_proc_name << "] Calling setup2()..." << endl;
    setup2(0, 0);
    cout << "[" << raft_proc_name << "] setup2() complete, waiting for leader election..." << endl;

    // Give more time for leader election and transfer
    // Initial election + transfer to preferred can take 3-5 seconds
    cout << "[" << raft_proc_name << "] Waiting for leader election (5 seconds)..." << endl;
    this_thread::sleep_for(chrono::seconds(5));

    if (leader_config) {
        cout << "[" << raft_proc_name << "] Starting log submission (" << message_count << " logs)..." << endl;

        vector<thread> threads;
        for (int par_id = 0; par_id < num_workers; par_id++) {
            threads.emplace_back(db_worker, par_id);
        }

        for (auto& t : threads) {
            t.join();
        }

        cout << "[" << raft_proc_name << "] All logs submitted, sending END markers..." << endl;

        // Send end markers
        for (int par_id = 0; par_id < num_workers; par_id++) {
            add_log_to_nc("", 0, par_id);
        }

        // Wait for replication
        this_thread::sleep_for(chrono::seconds(2));
    } else {
        // Follower: wait for logs
        cout << "[" << raft_proc_name << "] Waiting for logs..." << endl;

        while (end_received < num_workers && end_received_leader < num_workers) {
            int timeout = 0;
            while (timeout < 10 && end_received < num_workers && end_received_leader < num_workers) {
                this_thread::sleep_for(chrono::seconds(1));
                timeout++;
            }

            if (timeout >= 10) {
                cout << "[" << raft_proc_name << "] Timeout waiting for END markers" << endl;
                break;
            }
        }

        cout << "[" << raft_proc_name << "] Received " << fCnt << " logs, end_received: " << end_received << endl;
    }

    // Wait before shutdown
    this_thread::sleep_for(chrono::seconds(2));

    cout << "[" << raft_proc_name << "] Shutting down..." << endl;
    pre_shutdown_step();
    shutdown_paxos();

    // Print results
    cout << "[" << raft_proc_name << " RESULTS] follower_callbacks=" << fCnt
         << ", leader_callbacks=" << lCnt
         << ", end_received=" << end_received
         << ", leader_end_received=" << end_received_leader << endl;

    // Verify counts
    bool pass = false;
    if (leader_config == 1) {
        // Leader: should have received (message_count * num_workers) + num_workers END markers
        int expected = message_count * num_workers + num_workers;
        pass = (lCnt >= message_count * num_workers);
        cout << "[" << raft_proc_name << "] Leader verification: lCnt=" << lCnt
             << ", expected>=" << (message_count * num_workers)
             << " -> " << (pass ? "PASS" : "FAIL") << endl;
    } else {
        // Follower: should have received all logs
        int expected = message_count * num_workers + num_workers;
        pass = (fCnt >= message_count * num_workers);
        cout << "[" << raft_proc_name << "] Follower verification: fCnt=" << fCnt
             << ", expected>=" << (message_count * num_workers)
             << " -> " << (pass ? "PASS" : "FAIL") << endl;
    }

    if (pass) {
        cout << "PASS" << endl;
        return 0;
    } else {
        cout << "FAIL" << endl;
        return 1;
    }
}
