// @unsafe: uses template instantiations with unknown key functions and returns references
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

// bench.h first: its textual std/rcu/ticker includes must precede
// any header that opens namespace std into the global namespace
// (house pattern for import-std TUs; see server.cc).
#include "benchmarks/bench.h"
#include "ThreadPool.h"
#include "replay_record.h"
#include "storage/mbta_wrapper.hh"  // mbta_ordered_index (put_mbta cast)
#include "lib/common.h"

import std;

thread_local str_arena arena;
thread_local void *buf = NULL;
thread_local string obj_k;
thread_local string obj_v;
static thread_local std::vector<mako::ReplayRecordView> replay_records;
static thread_local std::vector<mbta_ordered_index*> replay_tables;

bool cmpFunc2_v2(const std::string& newValue,
                 const std::string& oldValue);

namespace {

class transaction_epoch_quiesce_guard {
public:
    transaction_epoch_quiesce_guard() = default;
    transaction_epoch_quiesce_guard(
        const transaction_epoch_quiesce_guard&) = delete;
    transaction_epoch_quiesce_guard& operator=(
        const transaction_epoch_quiesce_guard&) = delete;

    ~transaction_epoch_quiesce_guard() {
        Transaction::rcu_quiesce();
    }
};

void resolve_replay_tables(const std::vector<mako::ReplayRecordView>& records,
                           abstract_db* db) {
    if (db == nullptr) {
        Panic("replay requires a database");
    }
    replay_tables.clear();
    replay_tables.reserve(records.size());
    for (const auto& record : records) {
        abstract_ordered_index* const index =
            db->get_index_by_table_id(record.table_id);
        if (index == nullptr) {
            Panic("replay table_id is not registered: %u",
                  static_cast<unsigned>(record.table_id));
        }
        mbta_ordered_index* const table =
            dynamic_cast<mbta_ordered_index*>(index);
        if (table == nullptr) {
            Panic("replay table_id %u is not an mbta table",
                  static_cast<unsigned>(record.table_id));
        }
        replay_tables.push_back(table);
    }
}

size_t apply_replay_records(
    const std::vector<mako::ReplayRecordView>& records,
    size_t first_record_index, size_t record_count, uint32_t cid,
    abstract_db* db) {
    size_t put_ops = 0;
    for (size_t record_index = first_record_index;
         record_index < first_record_index + record_count; ++record_index) {
        const auto& record = records[record_index];
        obj_k.assign(record.key.data(), record.key.size());
        mako::materialize_replay_value(
            obj_v, record.value, record.is_delete, cid);

        int try_cnt = 1;
        while (1) {
            try {
                void *txn = db->new_txn(
                    0, arena, buf, abstract_db::HINT_DEFAULT);
                replay_tables[record_index]->put_mbta(
                    txn, obj_k, cmpFunc2_v2, obj_v);
                db->commit_txn_no_paxos(txn);
                if (try_cnt > 1 && try_cnt % 20 == 0) {
                    std::cout << "succeed at retry#:" << try_cnt << std::endl;
                }
                break;
            } catch (...) {
                ++try_cnt;
            }
        }
        ++put_ops;
    }
    return put_ops;
}

}  // namespace

// @unsafe: reads metadata through a raw byte pointer
inline uint32_t keystore_decode3_v2(const std::string& s){
    return mako::load_value_time_term(s.data(), s.length());
}

// @unsafe: calls unsafe keystore_decode3_v2
bool cmpFunc2_v2(const std::string& newValue,const std::string& oldValue)
{
    uint32_t commit_id_new = keystore_decode3_v2(newValue);
    uint32_t commit_id_old = keystore_decode3_v2(oldValue);

    return (commit_id_new%10 > commit_id_old%10) || (commit_id_new/10 > commit_id_old/10);
}

// @unsafe: parses and copies a raw replay buffer
size_t getFileContentNew_OneLogOptimized_mbta_v2(const char *buffer, /* K-V pairs */
                                                 uint32_t cid,  /* timestamp on current shard */
                                                 unsigned short int count,
                                                 unsigned int len,
                                                 abstract_db* db) {
    if (!mako::parse_replay_record_batch(
            buffer, len, count, replay_records)) {
        Panic("malformed replay record batch: count=%u len=%u",
              static_cast<unsigned>(count), len);
    }
    resolve_replay_tables(replay_records, db);
    transaction_epoch_quiesce_guard quiesce_on_exit;
    return apply_replay_records(
        replay_records, 0, replay_records.size(), cid, db);
}

size_t replay_validated_mbta_v2(const mako::ReplayLogView& log,
                                abstract_db* db) {
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
    transaction_epoch_quiesce_guard quiesce_on_exit;
    size_t put_ops = 0;
    for (const auto& transaction : log.transactions) {
        put_ops += apply_replay_records(
            log.records, transaction.first_record_index,
            transaction.record_count, transaction.time_term, db);
    }
    return put_ops;
}
