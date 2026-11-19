// @unsafe: uses template instantiations with unknown key functions and returns references
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// bench.h first: its textual std/rcu/ticker includes must precede
// any header that opens namespace std into the global namespace
// (house pattern for import-std TUs; see server.cc).
#include "benchmarks/bench.h"
#include "ThreadPool.h"
#include "replay_record.h"
#include "lib/common.h"
#include "lib/sharding_admission.h"

import std;

thread_local ThreadDBWrapperMbta::replay_attachment
    ThreadDBWrapperMbta::replay_thread_attachment_;
static thread_local std::vector<mako::ReplayRecordView> replay_records;

namespace {

// @unsafe - Opt-in functional-test receipt after the selected backend consumed
// a nonempty batch. This is not a payload digest: replay may compare-skip an
// already newer version. Native integration tests check exact payload/metadata.
void emit_protocol_replay_receipt(size_t processed_records,
                                  uint32_t recovered_time_term) {
    static const bool enabled = [] {
        const char* value = getenv("MAKO_STO_PROTOCOL_RECEIPTS");
        return value != nullptr && strcmp(value, "1") == 0;
    }();
    static thread_local bool emitted = false;
    if (!enabled || emitted || processed_records == 0 || recovered_time_term == 0)
        return;
    const auto& config = BenchmarkConfig::getInstance();
    fprintf(stderr,
            "STO_PROTOCOL_REPLAY_RESULT {\"schema_version\":1,"
            "\"engine\":\"%s\",\"shard\":%zu,"
            "\"processed_records\":%zu,\"recovered_time_term\":%u}\n",
            config.getStorageEngine().c_str(), config.getShardIndex(),
            processed_records, recovered_time_term);
    fflush(stderr);
    emitted = true;
}

// @unsafe - Resolve every table before applying any record in the batch.
void resolve_replay_tables(const std::vector<mako::ReplayRecordView>& records,
                           abstract_db* db) {
    if (db == nullptr) {
        Panic("replay requires a database");
    }
    for (const auto& record : records) {
        abstract_ordered_index* const index =
            db->get_index_by_table_id(record.table_id);
        if (index == nullptr) {
            Panic("replay table_id is not registered: %u",
                  static_cast<unsigned>(record.table_id));
        }
    }
}

// @unsafe - Calls the backend's source-metadata replay capability.
size_t apply_replay_records(
    const std::vector<mako::ReplayRecordView>& records,
    size_t first_record_index, size_t record_count, uint32_t cid,
    abstract_db* db) {
    size_t put_ops = 0;
    for (size_t record_index = first_record_index;
         record_index < first_record_index + record_count; ++record_index) {
        const auto& record = records[record_index];
        // false means a definite conflict before publication. Fatal and
        // indeterminate outcomes must escape, never enter the retry loop.
        while (!db->replay_apply_record(
            record.table_id, record.key, record.value, record.is_delete, cid)) {
        }
        ++put_ops;
    }
    return put_ops;
}

}  // namespace


// @unsafe: parses and copies a raw replay buffer
size_t getFileContentNew_OneLogOptimized_mbta_v2(const char *buffer, /* K-V pairs */
                                                 uint32_t cid,  /* timestamp on current shard */
                                                 unsigned short int count,
                                                 unsigned int len,
                                                 abstract_db* db) {
    // @unsafe - this function is exclusively replication-log row application.
    // Admission is outside the OCC retry loop: a forbidden replay is not a
    // transient transaction conflict and must not spin forever.
    mako::ShardingReplicationReplay replay;
    if (!mako::valid_replay_time_term(cid)) {
        Panic("invalid replay source timestamp: %u", cid);
    }
    if (!mako::parse_replay_record_batch(
            buffer, len, count, replay_records)) {
        Panic("malformed replay record batch: count=%u len=%u",
              static_cast<unsigned>(count), len);
    }
    resolve_replay_tables(replay_records, db);
    db->observe_replay_timestamp(cid);
    return apply_replay_records(
        replay_records, 0, replay_records.size(), cid, db);
}

// @unsafe - Apply a fully validated log with original source metadata.
size_t replay_validated_mbta_v2(const mako::ReplayLogView& log,
                                abstract_db* db) {
    // @unsafe - replication-log application only: one native replay admission
    // for the whole validated log, outside every per-record OCC retry loop.
    mako::ShardingReplicationReplay replay;
    size_t next_record_index = 0;
    for (const auto& transaction : log.transactions) {
        if (transaction.first_record_index != next_record_index ||
            transaction.record_count >
                log.records.size() - next_record_index) {
            Panic("validated replay log has an invalid record range");
        }
        next_record_index += transaction.record_count;
    }
    if (next_record_index != log.records.size()) {
        Panic("validated replay log does not cover every record");
    }

    resolve_replay_tables(log.records, db);
    // Observe even an empty log's footer and transactions that compare/skip.
    // Recovery must never admit a new coordinator below any recovered source.
    db->observe_replay_timestamp(log.recovered_time_term);
    size_t put_ops = 0;
    for (const auto& transaction : log.transactions) {
        put_ops += apply_replay_records(
            log.records, transaction.first_record_index,
            transaction.record_count, transaction.time_term, db);
    }
    emit_protocol_replay_receipt(put_ops, log.recovered_time_term);
    return put_ops;
}
