#pragma once

#include <cstring>
#include <stdexcept>
#include <string>
#include "lib/server.h"
#include "sto/Transaction.hh"
#include "storage/rust_sto_tpcc_wrapper.hh"

// Run on an attached owner thread with two distinct, small rows. Drive the
// receiver entry point used by RPC so that its reply cache and helper frame
// both participate in the two reads.
inline void check_rust_sharding_receiver(rust_sto_tpcc_wrapper& db,
        abstract_ordered_index* index, uint64_t table, const std::string& config,
        const std::string& first_key, const std::string& second_key) {
    auto check = [](bool ok, const char* message) {
        if (!ok) throw std::runtime_error(message);
    };
    mako::ShardReceiver receiver(config);
    const int table_id = index->get_table_id();
    const int saved_mode = TThread::mode();
    TThread::set_mode(1);
    struct Restore {
        rust_sto_tpcc_wrapper& db;
        int mode;
        ~Restore() {
            db.shard_abort_txn(nullptr);
            mako::sharding_finish_request();
            TThread::set_mode(mode);
        }
    } restore{db, saved_mode};
    receiver.Register(&db, {{table_id, index}});
    const MakoShardTxn txn{0x52535049454345ULL, 1};
    const auto read = [&](const std::string& key, uint32_t number) {
        mako::get_request_t request{};
        check(key.size() <= sizeof(request.key) && key.size() <= sizeof(request.sharding.coordinate),
              "receiver fixture key exceeds wire capacity");
        MakoShardRoute route{};
        const MakoShardBytes coordinate{reinterpret_cast<const uint8_t*>(key.data()), key.size()};
        check(mako_sharding_route(table, coordinate, &route) == MAKO_SHARD_OK,
              "receiver fixture could not route key");
        request.identity = {1, 1, number, 0};
        request.req_nr = number;
        request.table_id = table_id;
        request.len = key.size();
        std::memcpy(request.key, key.data(), key.size());
        request.sharding.transaction = txn;
        request.sharding.table = table;
        request.sharding.grant = route.grant;
        request.sharding.snapshot_version = route.snapshot_version;
        request.sharding.coordinate_length = key.size();
        std::memcpy(request.sharding.coordinate, key.data(), key.size());
        mako::get_response_t reply{};
        receiver.ReceiveRequest(mako::getReqType, reinterpret_cast<char*>(&request),
                                reinterpret_cast<char*>(&reply));
        check(reply.status == mako::ErrorCode::SUCCESS && reply.identity == request.identity
              && reply.req_nr == number && reply.len <= sizeof(reply.value),
              "receiver fixture read failed or returned a different request's reply");
        return std::string(reply.value, reply.len);
    };
    const auto first = read(first_key, 1);
    check(db.shard_participant_active() && !db.shard_participant_empty(),
          "first read lost its Rust transaction");
    const auto cut = Transaction::epoch_cut();
    check(cut != 0 && !Transaction::epoch_drained(cut), "first read lost its epoch pin");
    const auto second = read(second_key, 2);
    check(first != second, "second read replayed the first row");
    check(read(second_key, 2) == second, "duplicate second read changed its reply");

    mako::basic_request_t terminal{};
    terminal.sharding.transaction = txn;
    for (uint32_t number : {3u, 4u}) {
        terminal.req_nr = number;
        terminal.identity = {1, 1, number, 0};
        mako::basic_response_t reply{};
        receiver.ReceiveRequest(mako::abortReqType, reinterpret_cast<char*>(&terminal),
                                reinterpret_cast<char*>(&reply));
        check(reply.status == mako::ErrorCode::SUCCESS && reply.identity == terminal.identity
              && reply.req_nr == number, "receiver fixture abort receipt mismatch");
    }
    check(!mako::sharding_has_request() && db.shard_participant_empty()
          && Transaction::epoch_drained(cut), "abort left the Rust participant pinned");
}
