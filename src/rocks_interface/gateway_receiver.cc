#include <std_compat.hpp>
#include "mako/lib/server.h"
#include "mako/lib/table_registry.h"
#include "mako/sto/Transaction.hh"
#include "mako/lib/shardClient.h"
#include "mako/benchmarks/bench.h"
#include <rusty/box.hpp>
#include <rusty/vec.hpp>
#include <cstring>

import cluster;

namespace mako {
// @unsafe - effect-free immutable catalog/legacy index bridge. The caller pins
// this complete incarnation BEFORE submitting its storage operation.
void ShardReceiver::ClientRoute(const MakoGatewayRequest& req, MakoGatewayResponse& resp) {
    resp = {};
    resp.client = req.client;
    resp.sequence = req.sequence;
    resp.status = ErrorCode::ERROR;
    resp.outcome = MAKO_GATEWAY_REJECTED;
    if (req.version != MAKO_GATEWAY_VERSION || req.name_length > sizeof(req.name)
        || req.key_length > sizeof(req.key)) return;
    int table = static_cast<int>(req.physical_table);
    if (req.name_length) {
        auto id = get_table_registry().get_table_id(
            std::string(reinterpret_cast<const char*>(req.name), req.name_length));
        if (id.is_none()) return;
        table = id.unwrap();
    }
    if (table <= 0 || table > UINT16_MAX) return;
    resp.physical_table = static_cast<uint32_t>(table);
    resp.owner = static_cast<uint32_t>(TThread::get_shard_index());
    if (sharding_leases_enabled()) {
        auto binding = get_table_registry().native_binding(table);
        if (binding.is_none()) return;
        const auto& b = *binding.as_ref().unwrap();
        resp.table = b.table;
        resp.fixed_coordinate = b.fixed_coordinate.is_some();
        const char* coordinate = reinterpret_cast<const char*>(req.key);
        size_t length = req.key_length;
        if (b.fixed_coordinate.is_some()) {
            const auto& fixed = b.fixed_coordinate.as_ref().unwrap();
            coordinate = fixed.data();
            length = fixed.size();
        }
        if (length > sizeof(resp.coordinate)) return;
        resp.coordinate_length = static_cast<uint32_t>(length);
        if (length) std::memcpy(resp.coordinate, coordinate, length);
        if (b.kind == 0) {
            MakoShardGrant grant{};
            if (mako_sharding_route(b.table,
                {reinterpret_cast<const uint8_t*>(coordinate), length}, &grant)
                != MAKO_SHARD_OK) return;
            resp.owner = grant.owner;
            resp.epoch = grant.epoch;
        } else {
            const int owner = compute_shard_for_key(table,
                std::string(reinterpret_cast<const char*>(req.key), req.key_length));
            if (owner < 0) return;
            resp.owner = static_cast<uint32_t>(owner);
        }
    } else {
        const int owner = compute_shard_for_key(table,
            std::string(reinterpret_cast<const char*>(req.key), req.key_length));
        if (owner < 0) return;
        resp.owner = static_cast<uint32_t>(owner);
        if (owner == TThread::get_shard_index() && !table_for(table)) return;
    }
    if (resp.owner >= static_cast<uint32_t>(TThread::get_nshards())) return;
    resp.status = ErrorCode::SUCCESS;
    resp.outcome = MAKO_GATEWAY_COMMITTED;
}

// @unsafe - sole production gateway storage callback. Rust admitted this exact
// request before entry. Engine lifetime hooks, not a detached permission bit,
// protect every read/write until actual commit/abort cleanup has completed.
void ShardReceiver::ExecuteClientOperation(void* context, const MakoGatewayRequest* req,
                                           MakoGatewayResponse* resp) {
    auto* receiver = static_cast<ShardReceiver*>(context);
    resp->status = ErrorCode::ERROR;
    resp->outcome = MAKO_GATEWAY_ABORTED;
    if (req->physical_table > UINT16_MAX || req->key_length >= max_key_length
        || req->value_length >= max_value_length) return;
    // Do not bind over, abort, or finish another live participant's staged work.
    if (TThread::txn && TThread::txn->has_staged_items()) {
        resp->status = ErrorCode::SERVER_BUSY;
        return;
    }
    ShardingRequest sharding{};
    sharding.transaction = {req->client, req->sequence};
    sharding.table = req->table;
    sharding.grant = {req->owner, req->epoch};
    sharding.fixed_coordinate = req->fixed_coordinate != 0;
    sharding.coordinate_length = static_cast<uint16_t>(req->coordinate_length);
    if (req->coordinate_length)
        std::memcpy(sharding.coordinate, req->coordinate, req->coordinate_length);
    bool bound = false;
    bool attempted = false;
    try {
        const uint8_t kind = req->kind == MAKO_GATEWAY_PUT ? nontxnPutReqType
            : req->kind == MAKO_GATEWAY_GET ? nontxnGetReqType
            : req->kind == MAKO_GATEWAY_DELETE ? nontxnRemoveReqType
            : nontxnInsertReqType;
        std::string key(reinterpret_cast<const char*>(req->key), req->key_length);
        std::string value(reinterpret_cast<const char*>(req->value), req->value_length);
        std::string output;
        bool result = false;
        if (req->owner != static_cast<uint32_t>(TThread::get_shard_index())) {
            // Client-only ephemeral endpoint cannot collide with a benchmark
            // worker listener. Its helper partition is the existing partition0.
            // It has no native ingress lease; the destination owns that lifetime.
            struct Forwarder {
                int owner;
                rusty::Box<ShardClient> client;
            };
            static thread_local rusty::Vec<Forwarder> forwarders;
            ShardClient* forwarder = nullptr;
            const int ingress = TThread::get_shard_index();
            for (const auto& entry : forwarders)
                if (entry.owner == ingress) { forwarder = entry.client.get(); break; }
            if (!forwarder) {
                auto& cfg = BenchmarkConfig::getInstance();
                if (!cfg.getConfig()) return;
                auto client = rusty::make_box<ShardClient>(
                    cfg.getConfig()->configFile, cfg.getCluster(), ingress, 0, true);
                forwarder = client.get();
                forwarders.push(Forwarder{ingress, std::move(client)});
            }
            attempted = true;
            resp->status = forwarder->forwardNontxn(sharding, kind,
                static_cast<uint16_t>(req->physical_table), key, value, &result,
                kind == nontxnGetReqType ? &output : nullptr);
        } else {
            if (!sharding_bind_request(sharding)) return;
            bound = true;
            const int table = sharding_request_table(sharding, req->physical_table);
            if (table <= 0 || table > UINT16_MAX) {
                sharding_finish_request();
                return;
            }
            // Reject a fenced incarnation before invoking any engine effect.
            // RunNontxnOp's local wrapper reuses this same exact held lease.
            sharding_require_point(table, key.data(), key.size());
            attempted = true;
            resp->status = receiver->RunNontxnOp(kind, static_cast<uint16_t>(table),
                                                key, value, &result, &output);
            sharding_finish_request(); // follows actual engine completion
            bound = false;
        }
        resp->op_result = result;
        // ERROR includes unexpected engine exceptions; retain ambiguity rather
        // than assert an unobserved successful stutter or safely aborted write.
        resp->outcome = (resp->status == ErrorCode::ERROR || resp->status == ErrorCode::TIMEOUT)
            ? MAKO_GATEWAY_UNKNOWN
            : resp->status == ErrorCode::SERVER_BUSY ? MAKO_GATEWAY_ABORTED
            : MAKO_GATEWAY_COMMITTED;
        if (output.size() > sizeof(resp->value)) {
            resp->status = ErrorCode::ERROR;
            resp->outcome = MAKO_GATEWAY_UNKNOWN;
            return;
        }
        resp->value_length = static_cast<uint32_t>(output.size());
        if (!output.empty()) std::memcpy(resp->value, output.data(), output.size());
    } catch (...) {
        resp->status = ErrorCode::ERROR;
        resp->outcome = attempted ? MAKO_GATEWAY_UNKNOWN : MAKO_GATEWAY_ABORTED;
        // No exception crosses Rust. If completion cannot be established, the
        // retained unknown result fences the stream instead of guessing success.
        if (bound) {
            try { sharding_finish_request(); }
            catch (...) { resp->outcome = MAKO_GATEWAY_UNKNOWN; }
        }
    }
}
} // namespace mako
