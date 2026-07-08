// rrr server/client wiring for ShardDataService (see shard_data_rpc_harness.h
// for why this is its own TU). Includes rrr/rrr.hpp + rcc_rpc.h but NOT gtest;
// mirrors src/mako/cluster_bootstrap.cc, which compiles fine with this same
// header pair.

#include "shard_data_rpc_harness.h"

#include "shard_data_service.h"   // ShardDataServiceImpl + ShardDataServiceProxy (rcc_rpc.h)
#include "rrr/rrr.hpp"            // rrr::Server / Client / PollThread

namespace rpc_harness {

// @unsafe - rrr framework wiring (raw Server, leaked singletons), same shape as
// cluster_bootstrap.cc's ConfigKvService bring-up.
int start_server(janus::ShardData* shard, const std::string& addr) {
    auto* spoll  = new rusty::Arc<rrr::PollThread>(rrr::PollThread::create());
    auto* server = new rrr::Server(rusty::Some(spoll->clone()));
    server->reg_service(rusty::make_box<janus::ShardDataServiceImpl>(shard));
    return server->start(addr.c_str());
}

janus::ShardDataServiceProxy* connect_client(const std::string& addr) {
    auto* cpoll  = new rusty::Arc<rrr::PollThread>(rrr::PollThread::create());
    auto* client = new rusty::Arc<rrr::Client>(rrr::Client::create(cpoll->clone()));
    if ((*client)->connect(addr.c_str(), false) != 0) return nullptr;
    return new janus::ShardDataServiceProxy(const_cast<rrr::Client*>(client->get()));
}

}  // namespace rpc_harness
