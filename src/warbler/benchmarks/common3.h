//
// Created by weihshen on 3/29/21.
//

// ONLY for dbtest
#ifndef SILO_STO_COMMON_3_H
#define SILO_STO_COMMON_3_H

#define INIT_SYNC_UTIL_VARS \
    int sync_util::sync_logger::shardIdx = 0; \
    vector<std::atomic<uint32_t>> sync_util::sync_logger::local_timestamp_(80) ; \
    vector<std::atomic<uint32_t>> sync_util::sync_logger::vectorized_w_(10) ; \
    int sync_util::sync_logger::nshards = 0; \
    int sync_util::sync_logger::local_replica_id = 0; \
    std::chrono::time_point<std::chrono::high_resolution_clock>  sync_util::sync_logger::last_update = std::chrono::high_resolution_clock::now(); \ 
    int sync_util::sync_logger::nthreads = 0; \
    bool sync_util::sync_logger::worker_running = false; \
    bool sync_util::sync_logger::is_leader = true; \
    string sync_util::sync_logger::cluster = srolis::LOCALHOST_CENTER; \
    transport::Configuration *sync_util::sync_logger::config = nullptr; \
    bool sync_util::sync_logger::toLeader = false ; \
    std::mutex sync_util::sync_logger::m ; \
    std::condition_variable sync_util::sync_logger::cv ; \
    unordered_map<int, uint32_t> sync_util::sync_logger::hist_timestamp = {}; \
    unordered_map<int, vector<uint32_t>> sync_util::sync_logger::hist_timestamp_vec = {}; \
    std::atomic<uint32_t> sync_util::sync_logger::noops_cnt{0}; \
    std::atomic<uint32_t> sync_util::sync_logger::noops_cnt_hole{0}; \
    int sync_util::sync_logger::exchange_refresh_cnt = 0; \
    std::atomic<bool> sync_util::sync_logger::exchange_running{true}; \
    int sync_util::sync_logger::failed_shard_index = -1; \
    uint32_t sync_util::sync_logger::failed_shard_ts = -1; \

#endif // SILO_STO_COMMON_3_H