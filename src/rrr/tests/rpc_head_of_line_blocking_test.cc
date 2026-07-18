// Minimal reproduction: an rrr Server runs every request handler on its ONE
// poll thread (this is by design -- see rpc/SINGLE_THREAD_HEAD_OF_LINE.md).
// A handler that does not yield therefore blocks that thread, so any *other*
// request to the same server -- even a trivial one -- cannot be serviced until
// the slow handler returns. That is head-of-line (HOL) blocking.
//
// This is the suspected cause of Mako's live-migration big-table drain
// timeouts: the data plane serves the bulk row-copy (ScanRange) and the tiny
// control RPCs (DrainWrites, Freeze) on ONE server / ONE poll thread, so
// copying a big table can starve the drain check until it times out. The
// recommended fix is to serve the bulk path on its OWN server (own poll
// thread); the second test below demonstrates that fix in miniature.
//
// `benchmark::BenchmarkService::sleep(sec)` calls ::sleep on the poll thread --
// a non-yielding, thread-blocking handler, the same shape as a synchronous
// scan -- so it stands in for the bulk copy. `nop` stands in for the control
// RPC.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "../rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

import std;

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

// One BenchmarkService server on its OWN poll thread. Owns everything it
// creates; teardown joins the poll thread.
struct ServerNode {
    rusty::Option<rusty::Arc<PollThread>> poll;
    Server* server = nullptr;
    int port = 0;

    void start() {
        poll = rusty::Some(PollThread::create());
        for (int attempt = 0; attempt < 20; ++attempt) {
            port = test_ports::get_port();
            server = new Server(rusty::Some(poll.as_ref().unwrap().clone()));
            server->reg_service(rusty::make_box<BenchmarkService>());
            if (server->start(("0.0.0.0:" + std::to_string(port)).c_str()) == 0) return;
            delete server;
            server = nullptr;
        }
        ADD_FAILURE() << "could not bind a server port";
    }
    void stop() {
        delete server;
        server = nullptr;
        if (poll.is_some()) {
            poll.as_ref().unwrap()->shutdown();
            poll = rusty::None;
        }
    }
};

// A client on its OWN poll thread, connected to `port`.
struct ClientNode {
    rusty::Option<rusty::Arc<PollThread>> poll;
    rusty::Option<rusty::Arc<Client>> client;

    void connect(int port) {
        poll = rusty::Some(PollThread::create());
        client = rusty::Some(Client::create(poll.as_ref().unwrap()));
        ASSERT_EQ(client.as_ref().unwrap()->connect(
                      ("127.0.0.1:" + std::to_string(port)).c_str()), 0);
    }
    Client* raw() { return const_cast<Client*>(client.as_ref().unwrap().get()); }
    void stop() {
        if (client.is_some()) client.as_ref().unwrap()->close();
        client = rusty::None;
        if (poll.is_some()) {
            poll.as_ref().unwrap()->shutdown();
            poll = rusty::None;
        }
    }
};

// Fire a blocking sleep(sec) from its own client thread so it occupies the
// server's poll thread while the caller measures a concurrent nop.
static std::thread fire_sleep(ClientNode& cn, double sec) {
    return std::thread([&cn, sec] {
        BenchmarkProxy proxy(cn.raw());
        BenchmarkProxy::RpcSleepRequest req;
        req.sec = sec;
        proxy.sleep(req);  // blocks this thread AND the server's poll thread
    });
}

// Time a single nop() round-trip in milliseconds.
static long time_nop_ms(ClientNode& cn, const char* label) {
    BenchmarkProxy proxy(cn.raw());
    BenchmarkProxy::RpcNopRequest req;
    req.in_0 = "";
    auto t0 = steady_clock::now();
    auto r = proxy.nop(req);
    long ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    fprintf(stderr, "[HOL] %s: nop returned in %ld ms, ok=%d\n", label, ms, r.is_ok() ? 1 : 0);
    return ms;
}

// THE BUG: bulk (sleep) and control (nop) share one server/poll thread, so the
// control RPC waits out the full bulk handler.
TEST(RpcHeadOfLineBlocking, SingleServerBulkStarvesControl) {
    ServerNode srv;
    srv.start();
    ClientNode bulk;   bulk.connect(srv.port);
    ClientNode control; control.connect(srv.port);

    std::thread sleeper = fire_sleep(bulk, 2.0);
    std::this_thread::sleep_for(300ms);   // let the sleep handler occupy the poll thread

    long nop_ms = time_nop_ms(control, "single-server");
    sleeper.join();

    // The bulk handler holds the one poll thread, so the control nop cannot be
    // serviced and stalls to the rrr client request timeout (~1s) instead of
    // returning in well under a millisecond.
    EXPECT_GT(nop_ms, 700) << "nop should have been blocked behind the bulk handler";

    control.stop();
    bulk.stop();
    srv.stop();
}

// THE FIX in miniature: serve bulk on server A, control on server B (each its
// own poll thread). The control RPC is now unaffected by the bulk handler.
TEST(RpcHeadOfLineBlocking, SeparateServersControlNotStarved) {
    ServerNode bulk_srv;    bulk_srv.start();
    ServerNode control_srv; control_srv.start();
    ClientNode bulk;    bulk.connect(bulk_srv.port);
    ClientNode control; control.connect(control_srv.port);

    std::thread sleeper = fire_sleep(bulk, 2.0);
    std::this_thread::sleep_for(300ms);

    long nop_ms = time_nop_ms(control, "separate-servers");
    sleeper.join();

    // Control server's poll thread was free the whole time -> sub-millisecond.
    EXPECT_LT(nop_ms, 300) << "nop on a separate server should not be blocked by the bulk handler";

    control.stop();
    bulk.stop();
    control_srv.stop();
    bulk_srv.stop();
}

}  // namespace
