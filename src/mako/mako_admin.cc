// mako_admin — operator CLI for cluster admin RPCs.
//
//   mako_admin migrate <host:port> <table> <lo> <hi> <src> <dst> [connect_retries]
//
// <host:port> is shard 0's MigrationAdmin service (its mako data-plane server:
// shard-0 leader's mako port + the data-plane delta; the serving process logs
// "data plane listening on ..." with the exact address). One call runs the full
// online range migration on the standing shard-0 ShardMaster; see
// src/mako/cluster_bootstrap.cc.
//
// Exit codes: 0 = migration committed; 1 = server rejected it (msg printed);
// 2 = usage / connect / RPC-layer failure.
//
// TU profile: rcc_rpc.h + rrr.hpp, no Masstree, no gtest (the fast-compiling
// pair used by cluster_bootstrap.cc and the RPC test harness).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // sleep

#include <string>

#include "rcc_rpc.h"     // MigrationAdminProxy (generated)
#include "rrr/rrr.hpp"   // rrr::Client / PollThread

int main(int argc, char** argv) {
    if (argc < 8 || strcmp(argv[1], "migrate") != 0) {
        fprintf(stderr,
                "usage: %s migrate <host:port> <table> <lo> <hi> <src> <dst> "
                "[connect_retries]\n",
                argv[0]);
        return 2;
    }
    const std::string addr = argv[2];
    const int retries = (argc > 8) ? atoi(argv[8]) : 30;

    auto poll = rrr::PollThread::create();
    auto client = rrr::Client::create(poll.clone());
    int attempt = 0;
    while (client->connect(addr.c_str(), false) != 0) {
        if (++attempt > retries) {
            fprintf(stderr, "mako_admin: cannot connect to %s after %d tries\n",
                    addr.c_str(), retries);
            return 2;
        }
        sleep(1);
    }

    // @unsafe { const_cast at the rrr boundary, same as the service harnesses }
    janus::MigrationAdminProxy proxy(const_cast<rrr::Client*>(client.get()));
    janus::MigrationAdminProxy::RpcMigrateRequest req;
    req.table_name = argv[3];
    req.lo = argv[4];
    req.hi = argv[5];
    req.src = atoi(argv[6]);
    req.dst = atoi(argv[7]);

    auto r = proxy.Migrate(req);
    if (r.is_err()) {
        fprintf(stderr, "mako_admin: Migrate RPC failed (rrr err=%d)\n",
                static_cast<int>(r.unwrap_err()));
        return 2;
    }
    auto resp = r.unwrap();
    printf("ok=%d moved=%lld msg=%s\n", static_cast<int>(resp.ok),
           static_cast<long long>(resp.moved), resp.msg.c_str());
    return resp.ok == 1 ? 0 : 1;
}
