// This file contains the single definition of sync_util static variables
// to avoid multiple definition errors when including mako.hh

#include <stdint.h>


#include "sto/sync_util.hh"
#include "lib/configuration.h"
#include "lib/common.h"

import std;

// Define all sync_util::sync_logger static variables once
int sync_util::sync_logger::shardIdx = 0;
// @unsafe - Never destroyed. sync_logger::advancer() runs on a detached thread
// that nothing joins, so it can still be indexing these vectors while the main
// thread runs the exit-time destructors; freeing their storage there is a
// use-after-free. Every other sync_logger static that thread touches is
// trivially destructible. The attribute is Clang's, the compiler this build
// selects (CMakeLists.txt); one that does not know it ignores it and keeps
// the destructors.
[[clang::no_destroy]] std::vector<std::atomic<uint32_t>> sync_util::sync_logger::local_timestamp_(80);
#ifndef DISABLE_DISK
[[clang::no_destroy]] std::vector<std::atomic<uint32_t>> sync_util::sync_logger::disk_timestamp_(80);
#endif
std::atomic<uint32_t> sync_util::sync_logger::single_watermark_(0);
int sync_util::sync_logger::nshards = 0;
// Local HLC has no published timestamp and its validation gate is clear.
// Cache sequence zero is the empty recovered prefix. The legacy distributed
// u32 clock remains separate until that wire protocol migrates.
alignas(128) std::atomic<uint64_t> sync_util::sync_logger::mako_hlc_state{0};
alignas(128) std::atomic<uint64_t>
    sync_util::sync_logger::cache_sequence_state{0};
alignas(128) std::atomic<uint32_t>
    sync_util::sync_logger::legacy_distributed_timestamp_state{1};
std::chrono::time_point<std::chrono::high_resolution_clock> sync_util::sync_logger::last_update = std::chrono::high_resolution_clock::now();
int sync_util::sync_logger::nthreads = 0;
std::atomic<bool> sync_util::sync_logger::worker_running{false};
bool sync_util::sync_logger::is_leader = true;
std::string sync_util::sync_logger::cluster = mako::LOCALHOST_CENTER;
transport::Configuration *sync_util::sync_logger::config = nullptr;
bool sync_util::sync_logger::toLeader = false;
std::mutex sync_util::sync_logger::m;
std::condition_variable sync_util::sync_logger::cv;
std::atomic<uint32_t> sync_util::sync_logger::noops_cnt{0};
std::atomic<uint32_t> sync_util::sync_logger::noops_cnt_hole{0};
int sync_util::sync_logger::exchange_refresh_cnt = 0;
std::atomic<bool> sync_util::sync_logger::exchange_running{true};
std::atomic<int> sync_util::sync_logger::failed_shard_index{-1};
std::atomic<uint32_t> sync_util::sync_logger::failed_shard_ts{UINT32_MAX};
