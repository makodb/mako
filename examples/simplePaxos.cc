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

// 1 shard server with 3 worker threads (3 Paxos streams)
const int num_workers = 3;
const int message_count = 100;

int end_received = 0;
int end_received_leader = 0;
unordered_map<int, int> counters;


void db_worker(size_t par_id) {
    std::cerr << "[DB_WORKER DEBUG] Starting db_worker for par_id: " << par_id << std::endl;
    size_t sent = 0;
    const int base = 3 * 1000;  // Reduced from 300KB to 3KB to avoid crash
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

        t.lap_nano();
        auto e0 = t.lap_nano();
        std::cerr << "[DB_WORKER DEBUG] Calling add_log_to_nc for par_id: " << par_id << ", size: " << size << std::endl;
        add_log_to_nc((char const *)LOG, size, par_id);
        auto e1 = t.lap_nano();
        
        // cout << "# of log to send: " << size << ", par_id: " << par_id 
        //      << ", generate: " << e0/1000000.0 << " ms, add_log_to_nc: " 
        //      << e1/1000000.0 << " ms, log-id: " << (log_id*10+par_id) 
        //      << ", outstandings: " << get_outstanding_logs(par_id) << endl;
        
        usleep(5 * 1000);
    }
    counters[par_id] = sent;
    free(LOG);
}

int main(int argc, char **argv) {
    // start Paxos applications
    string paxos_proc_name = "localhost";
    int leader_config = 0;
    vector<string> paxos_config{
        get_current_absolute_path() + "../config/1leader_2followers/paxos3_shardidx0.yml",
        get_current_absolute_path() + "../config/occ_paxos.yml"
    };

    paxos_proc_name = std::string(argv[1]);
    if (paxos_proc_name == "localhost") leader_config = 1;
    
    char *argv_paxos[18];
    argv_paxos[0] = (char *)"";
    argv_paxos[1] = (char *)"-b";
    argv_paxos[2] = (char *)"-d";
    argv_paxos[3] = (char *)"60";
    argv_paxos[4] = (char *)"-f";
    argv_paxos[5] = (char *) paxos_config[0].c_str();
    argv_paxos[6] = (char *)"-f";
    argv_paxos[7] = (char *) paxos_config[1].c_str();
    argv_paxos[8] = (char *)"-t";
    argv_paxos[9] = (char *)"30";
    argv_paxos[10] = (char *)"-T";
    argv_paxos[11] = (char *)"100000";
    argv_paxos[12] = (char *)"-n";
    argv_paxos[13] = (char *)"32";
    argv_paxos[14] = (char *)"-P";
    argv_paxos[15] = (char *) paxos_proc_name.c_str();
    argv_paxos[16] = (char *)"-A";
    argv_paxos[17] = (char *)"10000";  // bulkBatchCount
    std::vector<string> ret = setup(18, argv_paxos);
    if (ret.empty()) {
        return -1;
    }

    register_leader_election_callback([&](int control) {
        std::cout << "notify a new leader is elected! I'm " << paxos_proc_name << ", control: " << control << "\n";
    });

    std::atomic<int> lCnt(0), fCnt(0);
    for (size_t i = 0; i < num_workers; i++) {
        counters[i] = 0;

        register_for_leader_par_id_return([&lCnt](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>>& un_replay_logs_) {
            int status = (len < 10 && len > 0) ? mako::PaxosStatus::STATUS_NOOPS : mako::PaxosStatus::STATUS_NORMAL;
            uint32_t timestamp = 0;  // Simple timestamp for this example
            
            if (len == 0) end_received_leader++;

            if (len > 32) {
                long long log_id = stoll(string(log, 0, 16));
                long long st = stoll(string(log, 16, 16));
                long long et = getCurrentTimeMillis();
                timestamp = static_cast<uint32_t>(et);  // Use current time as timestamp
                // cout << "register_for_leader_par_id_return, par_id: " << par_id << ", epoch:" << get_epoch()
                //      << ", slot_id:" << slot_id << ", no-ops:" << (len < 10 && len > 0) << ", len: " << len
                //      << ", time spent to commit log: " << (et - st) << "ms, log-id: " << log_id << endl;
            } else {
                timestamp = static_cast<uint32_t>(getCurrentTimeMillis());
                // cout << "register_for_leader_par_id_return, par_id: " << par_id << ", epoch:" << get_epoch()
                //      << ", slot_id:" << slot_id << ", no-ops:" << (len < 10 && len > 0) << ", len: " << len << endl;
            }
            lCnt++;
            // Return timestamp * 10 + status (for safety check compatibility)
            return timestamp * 10 + status;
        }, i);

        register_for_follower_par_id_return([&fCnt](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>>& un_replay_logs_) {
            int status = (len < 10 && len > 0) ? mako::PaxosStatus::STATUS_NOOPS : mako::PaxosStatus::STATUS_NORMAL;
            uint32_t timestamp = static_cast<uint32_t>(getCurrentTimeMillis());  // Use current time as timestamp
            
            if (len == 0) end_received++;

            fCnt++;
            char *newLog = new char[len + 1];
            strcpy(newLog, log);
            // cout << "register_for_follower_par_id_return, par_id: " << par_id << ", epoch:" << get_epoch()
            //      << ", slot_id:" << slot_id << ", no-ops:" << (len < 10 && len > 0) << ", len: " << len << endl;

            // Return timestamp * 10 + status (for safety check compatibility)
            return timestamp * 10 + status;
        }, i);
    }

    setup2(0, 0);

    // Wait for all sites to complete their communicator setup
    // This ensures p1's rep_commo_ is initialized before commits arrive
    // (p1 forwards commits to learner, needs communicator ready)
    this_thread::sleep_for(chrono::seconds(2));

    if (leader_config) {
        std::cerr << "[LEADER DEBUG] Starting db_worker threads" << std::endl;
        vector<thread> threads;
        for (int par_id = 0; par_id < num_workers; par_id++) {
            threads.emplace_back(db_worker, par_id);
        }

        for (auto& t : threads) {
            t.join();
        }

        for (int par_id = 0; par_id < num_workers; par_id++) {
            add_log_to_nc("", 0, par_id);
        }

        // Wait for ending messages to be committed before shutdown
        for (int par_id = 0; par_id < num_workers; par_id++) {
            wait_for_submit(par_id);
        }
    }

    if (!leader_config) {
        while (end_received < num_workers && end_received_leader < num_workers) {
            cout << paxos_proc_name << ", received ending: " << end_received << ", num_workers:" << num_workers 
                 << ", received msg: " << fCnt << ", end_received_leader:" << end_received_leader << endl;
            this_thread::sleep_for(chrono::seconds(1));
        }

        if (end_received == num_workers) {
            cout << paxos_proc_name << ", received ending: " << end_received << ", received msg: " << fCnt << endl;
        }
    } else {
        this_thread::sleep_for(chrono::seconds(1));
    }

    this_thread::sleep_for(chrono::seconds(3));

    pre_shutdown_step();
    shutdown_paxos();

    cout << "[" << paxos_proc_name << " committed]: " << fCnt << "(follower/learner), " 
         << lCnt << "(leader), endReceived: " << end_received 
         << ", leaderEndReceived:" << end_received_leader << endl;
    
    if (leader_config==1) {
        VERIFY(lCnt-num_workers==message_count*num_workers, "committed logs count verify");
    } else {
        VERIFY(fCnt-num_workers==message_count*num_workers, "committed logs count verify");
    }
    
    // if (leader_config) {
    //     cout << "sent logs count:" << endl;
    //     int tput = 0;
    //     for (const auto& [par_id, count] : counters) {
    //         cout << " - par_id[" << par_id << "] " << count << endl;
    //         tput += count;
    //     }
    //     cout << " - total: " << tput << endl;
    // }

    this_thread::sleep_for(chrono::seconds(3));
    return 0;
}