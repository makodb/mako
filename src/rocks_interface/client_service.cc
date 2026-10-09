#include <std_compat.hpp>
#include "mako/lib/server.h"
#include "mako/benchmarks/bench.h"
#include "mako/sto/Transaction.hh"
#include <rusty/option.hpp>
#include <cstring>
#include <cerrno>
#include "client_service.h"

namespace mako {
namespace {
// @unsafe - once-per-SRPC-thread legacy engine registration. The database has
// process lifetime and outlives RPC threads. Routing still uses the request's
// original grant; this only selects the executing shard's allocator/RCU state.
bool BindGatewayThread(ShardReceiver& receiver) {
    auto* db = receiver.GetDb();
    if (!db) return false;
    const int owner = receiver.GetOwner();
    auto& config = BenchmarkConfig::getInstance();
    BenchmarkConfig::setThreadLocalShardIndex(owner);
    if (config.getConfig() && config.getConfig()->multi_shard_mode) {
        auto* shard = config.getShardContext(owner);
        if (!shard || !shard->runtime.get()) return false;
        const_cast<SiloRuntime*>(shard->runtime.get())->BindToCurrentThread();
    } else {
        SiloRuntime::Current()->BindToCurrentThread();
    }
    static thread_local abstract_db* registered_db = nullptr;
    static thread_local rusty::Option<rusty::Box<scoped_db_thread_ctx>> engine = rusty::None;
    if (registered_db != db) {
        engine = rusty::None;
        engine = rusty::Some(rusty::make_box<scoped_db_thread_ctx>(db, true));
        registered_db = db;
    }
    TThread::in_loading_phase = false; // RPC handlers are never bootstrap loaders
    TThread::set_shard_index(owner);
    TThread::set_nshards(config.getNshards());
    TThread::set_pid(0);
    return true;
}
} // namespace

// @unsafe - SRPC service registration is an existing transport boundary.
int MakoClientService::__reg_to__(srpc::Server& server, size_t svc_index) {
    for (const auto id : {BEGIN_TXN, COMMIT, ROLLBACK, PUT, GET, DELETE_KEY, ROUTE, INSERT}) {
        const int result = server.reg_rpc(id, svc_index);
        if (result) return result;
    }
    return 0;
}

// @unsafe - deserialize a bounded wire envelope, then call the same native
// identity/lease/engine path used by TCP. Never mint or refresh an operation ID.
void MakoClientService::__dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req,
                                     srpc::WeakServerConnection sconn) const {
    auto connection = sconn.upgrade();
    if (connection.is_none()) return;
    std::string payload;
    srpc::BinaryReadArchive archive(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(payload, archive);
    MakoGatewayRequest request{};
    const size_t header = offsetof(MakoGatewayRequest, value);
    bool valid = payload.size() >= header && payload.size() <= sizeof(request);
    if (valid) {
        std::memcpy(&request, payload.data(), payload.size());
        valid = request.version == MAKO_GATEWAY_VERSION && request.kind == uint32_t(rpc_id)
            && request.key_length <= sizeof(request.key)
            && request.coordinate_length <= sizeof(request.coordinate)
            && request.name_length <= sizeof(request.name)
            && request.value_length <= sizeof(request.value)
            && payload.size() == header + request.value_length;
    }
    if (!valid || !receiver_ || !BindGatewayThread(*receiver_)) {
        connection.unwrap()->reply(*req, EINVAL, [](srpc::BinaryWriteArchive&) {});
        return;
    }
    MakoGatewayResponse response{};
    if (rpc_id == ROUTE) {
        receiver_->ClientRoute(request, response);
    } else {
        receiver_->ClientOperation(request, response);
    }
    const std::string bytes(reinterpret_cast<const char*>(&response),
                            offsetof(MakoGatewayResponse, value) + response.value_length);
    connection.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& output) {
        srpc::Serialize_::serialize(bytes, output);
    });
}
} // namespace mako
