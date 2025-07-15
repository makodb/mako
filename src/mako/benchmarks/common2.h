//
// Created by weihshen on 3/29/21.
//

// ONLY for dbtest
#ifndef SILO_STO_COMMON_2_H
#define SILO_STO_COMMON_2_H
#include "sto/ThreadPool.h"
#include <unistd.h>
#include <unordered_map>
#include <thread>
#include <vector>
#include "bench.h"
#include "benchmarks/sto/ReplayDB.h"
#include "benchmarks/sto/sync_util.hh"
#include <mutex>

using namespace std;

static vector<string>
split_ws(const string &s)
{
    vector<string> r;
    istringstream iss(s);
    copy(istream_iterator<string>(iss),
         istream_iterator<string>(),
         back_inserter<vector<string>>(r));
    return r;
}

// no-ops with epoch number, the pattern is "no-ops:4" with the new epoch number 4
// return: epoch number
int isNoops(const char *log, int len) {
    if (len==8) {
        if (log[0] == 'n' && log[1] == 'o' && log[2] == '-' &&
            log[3] == 'o' && log[4] == 'p' && log[5] == 's' && log[6] == ':') {
                return log[7]-'0';
            }
    }

    return -1;
}

bench_runner * start_workers_tpcc(int leader_config, /*leader or learner (new leader)*/
                        abstract_db *db,
                        int threads_nums,
                        bool skip_load = false /* for failover */,
                        int run = 0, /* for run() and rest, 0: threads start; 1: start run */
                        bench_runner *rc = NULL)
{
    std::string bench_type = "tpcc";
    std::string bench_opts = "--f_mode=0";
    if (skip_load) {
        bench_opts = "--f_mode=1";
    }

    vector<string> bench_toks = split_ws(bench_opts);
    int argc_bench = 1 + bench_toks.size();
    char *argv_bench[argc_bench];
    argv_bench[0] = (char *)bench_type.c_str();
    for (size_t i = 1; i <= bench_toks.size(); i++)
    {
        argv_bench[i] = (char *)bench_toks[i - 1].c_str();
    }
    bench_runner *R = tpcc_do_test(db, argc_bench, argv_bench, run, rc);
    return R;
}

void modeMonitorRun(abstract_db *db, int thread_nums, bench_runner * R) {
    // Wait until mainPaxos sends data
    std::unique_lock<std::mutex> lk((sync_util::sync_logger::m));
    sync_util::sync_logger::cv.wait(lk, [] { return sync_util::sync_logger::toLeader; });

    Warning("start for modeMonitorRun, running:%d",sync_util::sync_logger::worker_running);   
    if (!sync_util::sync_logger::worker_running) {
        return;
    }
    //bench_runner * r = start_workers_tpcc(1 /*leader_config*/, db, thread_nums, true);
    start_workers_tpcc(1 /*leader_config*/, db, thread_nums, true, 1, R);

    lk.unlock();
    sync_util::sync_logger::cv.notify_one();
}

void modeMonitor(abstract_db *db, int thread_nums, bench_runner *R) {
    Warning("start for modeMonitor, running:%d",sync_util::sync_logger::worker_running);
    thread mimic_thread(&modeMonitorRun, db, thread_nums, R);
    pthread_setname_np(mimic_thread.native_handle(), "modeMonitor");
    mimic_thread.detach();  // thread detach
}

abstract_db *ThreadDBWrapperMbta::replay_thread_wrapper_db = new mbta_wrapper;

#endif // SILO_STO_COMMON_2_H