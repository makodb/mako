#include <iostream>
#include <cstring>
#include <vector>
#include <getopt.h>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include "../util.h"
#include "deptran/s_main.h"
#include "mutex"
#include <chrono>
#include <sstream>
#include <iomanip>
//#include "benchmarks/sto/sync_util.hh"
//#include "benchmarks/common3.h"
//#include "lib/common.h"

using namespace std;
//using namespace srolis;
bool use_fork=false;
bool new_leader_elected=false;

//INIT_SYNC_UTIL_VARS

// add a latency on the server
/* 
    sudo tc qdisc del dev lo root netem
    sudo tc qdisc add dev lo root netem delay 30ms
    sudo tc qdisc show dev lo*/


bool stop = false;
int last_for = 20;
std::unordered_map<int, int> counters;
std::mutex lm_;
// std::function<int()> callback_paxos_ = nullptr;

// void register_sync_util_paxos(std::function<int()> cb) {
//     callback_paxos_ = cb;
// }

// void callback_paxos_exp(size_t par_id) {
//     unsigned long long int res=0;
//     unsigned long long int cnt=0;
//     while (!stop) {
//         res += callback_paxos_();
//         cnt ++;
//     }
//     std::cout << "callback_paxos_exp is expired: " << res << ", # of invoked: " << cnt << ", TPUT: " << cnt / 30.0 << std::endl;
// }

std::string intToString(long long num) {
    std::ostringstream ss;
    ss << std::setw(16) << std::setfill('0') << num;
    return ss.str();
}

long long getCurrentTimeMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

void db_worker(size_t par_id) {
    size_t sent = 0;
    int base = 300*1000;
    int size = base;
    util::timer t;
    unsigned char *LOG;
    LOG = (unsigned char *) malloc (size+100);
    //for (int i=0; i<1000; i++) {
    //par_id=0;
    int log_id=0;
    while (!stop) {
        sent ++;
        
        size = rand()%100+base;
        // for 16 bytes for log_id
        // the next 16 bytes for x ms
        log_id++;
        string id=intToString(log_id*10+par_id);
        for (int i=0;i<16;i++)
            LOG[i] = id.at(i);
        for (int i=32;i<size;i++)
            LOG[i] = 'i';

        t.lap_nano();
        long long tt_int = getCurrentTimeMillis();
        string tt=intToString(tt_int);
        for (int i=16;i<32;i++)
            LOG[i] = tt.at(i-16);
        
        auto e0 = t.lap_nano();
        add_log_to_nc((char const *)LOG, size, par_id);
        auto e1 = t.lap_nano();
        std::cout << "# of log to send: " << size << ", par_id: " << par_id 
                  << ", generate: " << e0/1000000.0 << " ms" << ", add_log_to_nc: " 
                  << e1/1000000.0 << " ms" << ", log-id: " << (log_id*10+par_id) 
                  << ", time to start: " << tt_int 
                  << ", outstandings: " << get_outstanding_logs(par_id) << std::endl;
        usleep(5*1000);
        // int outstanding = get_outstanding_logs(par_id) ;
        // while (1) {
        //     if (outstanding>10) {
        //         usleep(1*1000);
        //     } else {
        //         break;
        //     }
        //     outstanding = get_outstanding_logs(par_id) ;
        // }
    }
    counters[par_id] = sent;
}

int main(int argc, char **argv) {
    std::vector<std::string> paxos_config{};
    if (use_fork) {
        paxos_config.push_back("third-party/paxos/config/1leader_2followers/paxos3_shardidx0.yml");
        paxos_config.push_back("third-party/paxos/config/occ_paxos.yml");
    }

    string paxos_proc_name = "localhost";
    int leader_config = 0;
    int partitions = 3;
    static int end_received = 0;
    static int end_received_leader = 0;

    if (use_fork) {
        if (fork()) {
            paxos_proc_name = "p1";
        } else if (fork()) {
            paxos_proc_name = "p2";
        } else if (fork()) {
            paxos_proc_name = "localhost";
            leader_config = 1;
        } else {
            paxos_proc_name = "learner";
        }
    }


    while (1) {
        static struct option long_options[] = {
                {"partitions",   required_argument, 0,              'p'},
                {0, 0,                                     0,              0}};

        int option_index = 0;
        int c = getopt_long(argc, argv, "P:F:n:p:", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 0:
                if (long_options[option_index].flag != 0)
                    break;
                abort();
                break;

            case 'F':
                paxos_config.push_back(optarg);
                break;

            case 'P':
                paxos_proc_name = string(optarg);
                if (paxos_proc_name.compare("localhost") == 0) leader_config = 1;
                break;

            case 'p':
                partitions = strtoul(optarg, NULL, 10);
                break;

            case '?':
                /* getopt_long already printed an error message. */
                exit(1);

            default:
                printf("Option not recognized");
                break;
        }
    }

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
    std::vector<string> ret = setup(16, argv_paxos);
    if (ret.empty()) {
        return -1;
    }

    register_leader_election_callback([&, new_leader_elected](int control) {
      std::cout << "notify a new leader is elected! I'm " << paxos_proc_name << ", control: " << control << "\n" ;
      new_leader_elected=true;
    });

    int lCnt = 0, fCnt = 0;
    for (size_t i = 0; i < partitions; i++) {
        size_t par_id = i;
        counters[par_id] = 0;

        register_for_leader_par_id_return([&lCnt](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<std::vector<uint32_t>, int, int, int, const char *>> & un_replay_logs_) {
            vector<uint32_t> ret(1);
            ret[0]=0;
            if (len<10&&len>0) {
                ret[0]=5;
            }
            if (len==0)
                end_received_leader++;

            if (len>32){
                long long log_id = std::stoll(std::string(log,0,16));
                long long st = std::stoll(std::string(log,16,16));
                long long et = getCurrentTimeMillis();
                std::cout << "register_for_leader_par_id_return, par_id: " << par_id << ", epoch:" << get_epoch() <<  ", slot_id:" << slot_id
                            << ", no-ops:" << (len<10&&len>0) << ", len: " << len
                            << ", time spent to commit log: " << (et - st) << ", st: " << st << ", et: " << et << ", log-id: " << log_id << "," << std::endl;
            } else {
                std::cout << "register_for_leader_par_id_return, par_id: " << par_id << ", epoch:" << get_epoch() <<  ", slot_id:" << slot_id << ", no-ops:" << (len<10&&len>0) << ", len: " << len << std::endl;
            }

            lCnt++;
            return ret;
        }, par_id);

        register_for_follower_par_id_return([&fCnt](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<std::vector<uint32_t>, int, int, int, const char *>> & un_replay_logs_) {
            vector<uint32_t> ret(1);
            ret[0]=0;
            if (len == 0) {
                end_received++;
            }
            if (len<10&&len>0) {
                ret[0]=5;
            }

            fCnt++;
            char *newLog = new char[len + 1];
            strcpy(newLog, log);
            std::cout << "register_for_follower_par_id_return, par_id: " << par_id << ", epoch:" << get_epoch() << ", slot_id:" << slot_id << ", no-ops:" << (len<10&&len>0) << ", len: " << len << std::endl;

            std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 5));
            return ret;
        }, par_id);
    }

    setup2();

    std::thread background2([&, partitions, new_leader_elected]{
            while (1) {
                sleep(1);
                if (new_leader_elected) {
                    int base = 1000;
                    int size = base;
                    unsigned char *LOG;
                    LOG = (unsigned char *) malloc (size+1000);
                    for (int i=0;i<100;i++) {
                        size = rand()%1000+base;
                        for (int i=0;i<size;i++)
                            LOG[i] = 'i';
                        add_log_to_nc((char const *)LOG, size, i%partitions);
                        usleep(10*1000);
                    }


                    // send the end Log
                    for (int par_id = 0; par_id < partitions; par_id++) {
                        std::string endS = "";
                        char const *endLog = endS.c_str();
                        add_log_to_nc(endLog, strlen(endLog), par_id);

                    }
                    break;
            }
        }
    });
    background2.detach();

    if (leader_config) {
        std::thread background([]{
            sleep(last_for);
            stop = true;
        });
        background.detach();

        std::vector<std::thread> threads(partitions);
        for (int par_id = 0; par_id < partitions; par_id++) {
            threads[par_id] = std::thread(db_worker, par_id);
        }

        for (int par_id = 0; par_id < partitions; par_id++) {
            threads[par_id].join();
            std::string endS = "";
            char const *endLog = endS.c_str();
            add_log_to_nc(endLog, strlen(endLog), par_id);
        }
    }

    vector<std::thread> wait_threads;
    for (int par_id = 0; par_id < partitions; par_id++) {
        wait_threads.push_back(std::thread([par_id]() {
            std::cout << "Starting wait for " << par_id << std::endl;
            wait_for_submit(par_id);
        }));
    }
    for (auto &th : wait_threads) {
        //th.join();
    }

    // int tries = 0;
    if (!leader_config) {
        while (end_received < partitions && end_received_leader < partitions) {
            // tries ++;
            // if (tries >= last_for + 5) break;
            std::cout << paxos_proc_name << ", received ending: " << end_received << ", partitions:" << partitions << ", received msg: " << fCnt << ", end_received_leader:" << end_received_leader << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (end_received == partitions)
            std::cout << paxos_proc_name << ", received ending: " << end_received << ", received msg: " << fCnt << std::endl;
    } else {
        // should wait for a while for last piece of logs to be proceeded
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    pre_shutdown_step();
    int ret2 = shutdown_paxos();

    std::cout << "Committed (follower) - " << fCnt << ", endReceived - " << end_received << ", leaderEndReceived:" << end_received_leader << ", (leader): " << lCnt << std::endl;
    if (leader_config) {
        std::cout << "TPUT:" << std::endl;
        int tput = 0;
        for (auto it: counters) {
            std::cout << " - par_id[" << it.first << "] " << it.second << ", TPS: " << (it.second/last_for) << std::endl;
            tput += it.second;
        }
        std::cout << " - total: " << tput << std::endl;
    }
    return 0;
}