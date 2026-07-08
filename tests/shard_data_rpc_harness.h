#pragma once

// rrr server/client wiring for the ShardDataService, compiled in ITS OWN TU.
//
// The full rrr framework header (rrr/rrr.hpp) + the generated rcc_rpc.h + gtest
// in one -O3 translation unit compiles pathologically (>2h; each pair alone is
// fine -- cluster_bootstrap.cc has rrr.hpp+rcc_rpc, config_kv_service_test.cc
// has gtest+rcc_rpc). So the rrr::Server/Client construction lives here (no
// gtest), mirroring src/mako/cluster_bootstrap.cc, and the gtest test TU only
// ever sees rcc_rpc.h via shard_data_service.h -- the known-good fast profile.

#include <string>

namespace janus { class ShardData; class ShardDataServiceProxy; }

namespace rpc_harness {

// Start an rrr ShardDataService server over `shard`, bound to `addr`
// ("host:port"). Leaks the server/poll (process-lifetime). Returns 0 on success.
int start_server(janus::ShardData* shard, const std::string& addr);

// Connect a client to `addr` and return a ShardDataServiceProxy (leaked).
// Returns nullptr on connect failure.
janus::ShardDataServiceProxy* connect_client(const std::string& addr);

}  // namespace rpc_harness
