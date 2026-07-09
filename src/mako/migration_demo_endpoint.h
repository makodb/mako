#pragma once

// Cross-process online-migration DEMO endpoint (gated by MAKO_XPROC_MIGRATION_DEMO).
//
// Proves the migration coordinator moves a key range between two SEPARATE dbtest
// processes over the real ShardDataService rrr socket, alongside the live
// workload, WITHOUT touching the workload's (transactional) tables. The demo data
// plane is an isolated in-memory std::map (MapShardData): the served side runs on
// the rrr poll thread, which is not mbta-initialized, so a map backend is used
// here exactly as tests/shard_migration_rpc_test.cc does -- the real mbta data
// plane is proven separately (tests/test_mako_migration_reroute.cc,
// tests/shard_migration_mbta_test.cc).
//
// This header is deliberately THIN (std::string only) so the masstree-heavy
// dbtest.cc can include it: the rrr framework header (rrr/rrr.hpp, pulled in via
// rcc_rpc.h) and Masstree in one -O3 TU compile pathologically, so all the rrr +
// rcc_rpc + cluster-module usage lives in migration_demo_endpoint.cc, mirroring
// src/mako/cluster_bootstrap.cc.

#include <string>

namespace mako {

// SOURCE role: seed `count` demo keys ("dNN") and serve them over a
// ShardDataService rrr server bound to `bind_addr` ("0.0.0.0:port"). Non-blocking
// -- the server runs on its own poll thread; the server + data are leaked
// (process-lifetime). Returns 0 on success, non-zero if the bind failed.
int xproc_migration_serve(const std::string& bind_addr, int count);

// DEST / MASTER role: connect to the source's ShardDataService at `src_addr`
// ("host:port"), then drive the cluster ShardMaster to migrate [lo,hi) from the
// (remote) source into a local map -- background copy over ScanRange RPCs, 2PC
// checksum-verify, commit with the source dropping the range over a DropRange RPC.
// Retries the connect up to `connect_retries` times (1s apart) so a source still
// coming up is tolerated. Returns the number of rows now held locally for [lo,hi)
// (== rows moved), or -1 on any error.
long xproc_migration_run(const std::string& src_addr,
                         const std::string& lo, const std::string& hi,
                         int connect_retries);

}  // namespace mako
