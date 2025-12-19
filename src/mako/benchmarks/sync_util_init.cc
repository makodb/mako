// This file contains the single definition of sync_util static variables
// to avoid multiple definition errors when including mako.hh

#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <string>

#include "benchmarks/sto/sync_util.hh"
#include "lib/configuration.h"
#include "lib/common.h"

// Define all sync_util::sync_logger static variables once
int sync_util::sync_logger::shardIdx = 0;
std::vector<std::atomic<uint32_t>> sync_util::sync_logger::local_timestamp_(80);
#ifndef DISABLE_DISK
std::vector<std::atomic<uint32_t>> sync_util::sync_logger::disk_timestamp_(80);
#endif
std::atomic<uint32_t> sync_util::sync_logger::single_watermark_(0);
int sync_util::sync_logger::nshards = 0;
int sync_util::sync_logger::local_replica_id = 0;
std::chrono::time_point<std::chrono::high_resolution_clock> sync_util::sync_logger::last_update = std::chrono::high_resolution_clock::now();
int sync_util::sync_logger::nthreads = 0;
bool sync_util::sync_logger::worker_running = false;
bool sync_util::sync_logger::is_leader = true;
std::string sync_util::sync_logger::cluster = mako::LOCALHOST_CENTER;
transport::Configuration *sync_util::sync_logger::config = nullptr;
bool sync_util::sync_logger::toLeader = false;
std::mutex sync_util::sync_logger::m;
std::condition_variable sync_util::sync_logger::cv;
std::unordered_map<int, uint32_t> sync_util::sync_logger::hist_timestamp = {};
std::atomic<uint32_t> sync_util::sync_logger::noops_cnt{0};
std::atomic<uint32_t> sync_util::sync_logger::noops_cnt_hole{0};
int sync_util::sync_logger::exchange_refresh_cnt = 0;
std::atomic<bool> sync_util::sync_logger::exchange_running{true};
int sync_util::sync_logger::failed_shard_index = -1;
uint32_t sync_util::sync_logger::failed_shard_ts = -1;