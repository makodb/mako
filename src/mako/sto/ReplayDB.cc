#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ReplayDB.h"
#include "ThreadPool.h"
#include "replay_record.h"

import std;

namespace {
thread_local mako::ReplayLogView replay_log;
}

// @unsafe: uses memcpy and pointer arithmetic
// Single timestamp system: extracts timestamp and latency tracker from buffer
CommitInfo get_latest_commit_info(const char *buffer, size_t len) {
    if (!mako::parse_replay_log(buffer, len, replay_log)) {
        Panic("malformed replay log while reading commit info: len=%zu", len);
    }
    return CommitInfo{
        replay_log.latest_time_term, replay_log.latency_tracker};
}

// @unsafe: accepts a borrowed raw replay buffer
size_t treplay_in_same_thread_opt_mbta_v2(size_t par_id, const char *buffer,
                                          size_t len, abstract_db* db,
                                          int nshards) {
    //printf("replay a log, par_id:%d, len:%d\n", par_id, len);
    if (!mako::parse_replay_log(buffer, len, replay_log)) {
        Panic("malformed replay log: len=%zu", len);
    }
    return replay_validated_mbta_v2(replay_log, db);
}
