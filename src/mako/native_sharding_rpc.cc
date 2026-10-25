#include "native_sharding_host.h"

#include <cerrno>
#include <stdexcept>
#include <utility>
#include "benchmarks/benchmark_config.h"
#include "deptran/rcc_rpc.h"
#include "lib/configuration.h"

namespace mako {
namespace {
constexpr unsigned kNativeShardingPortDelta = 20000;

// @unsafe - std::string is the generated srpc wire type; bytes remain borrowed.
MakoShardBytes bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

struct PendingReply {
    janus::NativeShardingService::RpcInvokeResponse* response;
    srpc::DeferredReply deferred;
    // @unsafe - DeferredReply owns the generated response and connection handle.
    PendingReply(janus::NativeShardingService::RpcInvokeResponse& response,
                 srpc::DeferredReply deferred)
        : response(&response), deferred(std::move(deferred)) {}
};

// @unsafe - exactly-once Rust completion returns the transferred Box to C++.
// The response bytes are copied before Rust releases its temporary buffer.
void complete(void* context, uint32_t status, MakoShardBytes reply) {
    auto pending = rusty::Box<PendingReply>::from_raw(static_cast<PendingReply*>(context));
    try {
        pending->response->status = status;
        if (reply.len)
            pending->response->reply.assign(reinterpret_cast<const char*>(reply.data), reply.len);
        pending->deferred.reply();
    } catch (...) {
        pending->deferred.reply_error(EIO);
    }
}

class NativeService final : public janus::NativeShardingService {
    const uint32_t owner_;
public:
    // @safe - immutable actual participant identity, never read from a request.
    explicit NativeService(uint32_t owner) : owner_(owner) {}

    // @unsafe - poll-thread work is limited to copying the payload into Rust's
    // queue and moving deferred ownership. No storage or peer RPC runs here.
    void Invoke(const RpcInvokeRequest& request, RpcInvokeResponse& response,
                srpc::DeferredReply deferred) const override {
        auto pending = rusty::make_box<PendingReply>(response, std::move(deferred));
        auto* context = pending.into_raw();
        const uint32_t status = mako_sharding_dispatch(
            owner_, request.operation, bytes(request.payload), complete, context);
        if (status != MAKO_SHARD_OK) complete(context, status, {nullptr, 0});
    }
};
} // namespace

// @unsafe - address/port conversion at the shared transport configuration edge.
std::string native_sharding_address(const transport::Configuration& config,
                                     uint32_t owner, bool bind) {
    if (owner >= static_cast<uint32_t>(config.nshards))
        throw std::invalid_argument("native shard owner outside topology");
    const auto address = config.shard(static_cast<int>(owner), 0);
    size_t consumed = 0;
    const unsigned long base = std::stoul(address.port, &consumed);
    if (consumed != address.port.size() || base == 0
        || base > 65535 - kNativeShardingPortDelta)
        throw std::invalid_argument("native shard control port outside TCP range");
    return (bind ? std::string("0.0.0.0") : address.host) + ":"
        + std::to_string(base + kNativeShardingPortDelta);
}

// @unsafe - owns only native srpc resources; engine/runtime are lifetime borrows.
NativeShardingHost::NativeShardingHost(abstract_db* db, uint32_t owner,
                                       uint32_t nshards, SiloRuntime* runtime)
    : db(db), owner(owner), runtime(runtime), poll(srpc::PollThread::create()),
      server(rusty::None), peers(rusty::Vec<rusty::Option<rusty::Arc<srpc::Client>>>{}) {
    auto connections = peers.lock().unwrap();
    for (uint32_t i = 0; i < nshards; ++i) connections->push(rusty::None);
}

// @unsafe - caller has stopped and joined Rust before releasing the DB borrow.
NativeShardingHost::~NativeShardingHost() {
    if (server.is_some()) {
        server.as_mut().unwrap()->graceful_shutdown(0);
        server = rusty::None;
    }
    {
        auto connections = peers.lock().unwrap();
        for (auto& connection : *connections)
            if (connection.is_some()) connection.as_ref().unwrap()->close();
        connections->clear();
    }
    poll->shutdown();
}

// @unsafe - listener is bound to this owner, using the same mapping as clients.
uint32_t NativeShardingHost::start_service() {
    try {
        auto* config = BenchmarkConfig::getInstance().getConfig();
        if (!config || server.is_some()) return MAKO_SHARD_INVALID;
        const auto address = native_sharding_address(*config, owner, true);
        server = rusty::Some(rusty::make_box<srpc::Server>(
            srpc::Server::new_(rusty::Some(poll.clone()))));
        server.as_mut().unwrap()->reg_service_typed(rusty::make_box<NativeService>(owner));
        if (server.as_mut().unwrap()->start(reinterpret_cast<const int8_t*>(address.c_str())) != 0)
            return MAKO_SHARD_IO;
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @unsafe - acceptance is fenced before stopping Rust jobs; polling stays live
// until already-owned deferred responses and outgoing calls have completed.
void NativeShardingHost::stop_accepting() {
    if (server.is_some()) server.as_mut().unwrap()->stop_accepting();
}

// @unsafe - only native Rust workers call this synchronous srpc I/O kernel.
uint32_t native_sharding_peer_call(void* context, uint32_t owner, uint32_t operation,
                                    MakoShardBytes payload, MakoShardReplySink sink,
                                    void* sink_context) {
    try {
        auto& host = *static_cast<NativeShardingHost*>(context);
        auto client = [&]() -> rusty::Option<rusty::Arc<srpc::Client>> {
            auto connections = host.peers.lock().unwrap();
            if (owner >= connections->size()) return rusty::None;
            auto& connection = (*connections)[owner];
            if (connection.is_none()) {
                auto* config = BenchmarkConfig::getInstance().getConfig();
                if (!config) return rusty::None;
                const auto address = native_sharding_address(*config, owner, false);
                auto created = srpc::Client::create(host.poll.clone());
                if (created->connect(reinterpret_cast<const int8_t*>(address.c_str()), false) != 0)
                    return rusty::None;
                // Rust owns command retries and retained identities. Offline
                // srpc buffering would strand a request after reconnect exhausts.
                created->set_buffering_config(srpc::BufferingConfig::disabled());
                connection = rusty::Some(std::move(created));
            }
            return rusty::Some(connection.as_ref().unwrap().clone());
        }();
        if (client.is_none()) return MAKO_SHARD_IO;
        janus::NativeShardingProxy proxy(const_cast<srpc::Client*>(client.as_ref().unwrap().get()));
        janus::NativeShardingProxy::RpcInvokeRequest request;
        request.operation = operation;
        if (payload.len) request.payload.assign(reinterpret_cast<const char*>(payload.data), payload.len);
        auto result = proxy.Invoke(request);
        if (result.is_err()) {
            auto connections = host.peers.lock().unwrap();
            auto& cached = (*connections)[owner];
            // A concurrent retry may already have installed a fresh connection.
            if (cached.is_some()
                    && cached.as_ref().unwrap().get() == client.as_ref().unwrap().get())
                cached = rusty::None;
            return MAKO_SHARD_IO;
        }
        auto response = result.unwrap();
        sink(sink_context, response.status, bytes(response.reply));
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}
} // namespace mako
