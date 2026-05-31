#include <stdlib.h>

#include <gtest/gtest.h>
#include "../rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

import std;

using namespace rrr;
using namespace benchmark;
using namespace std::chrono_literals;

class DeferTestService : public BenchmarkService {
public:
    std::atomic<int> deferred_call_count{0};
    std::atomic<bool> should_drop_reply{false};
    std::atomic<bool> should_delay_reply{false};
    std::atomic<int> delay_ms{0};

    void deferred_echo(
        const RpcDeferredEchoRequest& req,
        RpcDeferredEchoResponse& resp,
        DeferredReply defer) override {
        deferred_call_count++;
        resp.result = req.val * 2;

        if (should_drop_reply.load()) {
            // Deliberately do NOT call defer.reply().
            // DeferredReply destructor should handle cleanup without crash.
            return;
        }

        if (should_delay_reply.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(delay_ms.load()));
        }

        defer.reply();
    }
};

class DeferredHandlerTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_;
    Server* server_ = nullptr;
    DeferTestService* service_ = nullptr;
    rusty::Option<rusty::Arc<Client>> client_;
    int port_;

    void SetUp() override {
        auto poll_arc = PollThread::create();
        poll_ = rusty::Some(std::move(poll_arc));

        bool started = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            port_ = test_ports::get_port();
            auto poll_clone = poll_.as_ref().unwrap().clone();
            server_ = new Server(rusty::Some(std::move(poll_clone)));
            auto svc = rusty::make_box<DeferTestService>();
            service_ = svc.get();
            server_->reg_service_typed(std::move(svc));
            if (server_->start(("0.0.0.0:" + std::to_string(port_)).c_str()) == 0) {
                started = true;
                break;
            }
            delete server_;
            server_ = nullptr;
            service_ = nullptr;
        }
        ASSERT_TRUE(started);

        client_ = rusty::Some(Client::create(poll_.as_ref().unwrap()));
        ASSERT_EQ(client_.as_ref().unwrap()->connect(
            reinterpret_cast<const int8_t*>(("127.0.0.1:" + std::to_string(port_)).c_str()), true), 0);
        std::this_thread::sleep_for(50ms);
    }

    void TearDown() override {
        client_.as_ref().unwrap()->close();
        delete server_;
        poll_.as_ref().unwrap()->shutdown();
    }
};

TEST_F(DeferredHandlerTest, NormalDeferredReplyReturnsCorrectValue) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    BenchmarkProxy::RpcDeferredEchoRequest req;
    req.val = 21;
    auto result = proxy.deferred_echo(req);
    ASSERT_TRUE(result.is_ok()) << "deferred_echo sync call failed";
    EXPECT_EQ(result.unwrap().result, 42);
    EXPECT_EQ(service_->deferred_call_count.load(), 1);
}

TEST_F(DeferredHandlerTest, AsyncDeferredReplyResolves) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    BenchmarkProxy::RpcDeferredEchoRequest req;
    req.val = 7;
    auto fu_result = proxy.async_deferred_echo(req);
    ASSERT_TRUE(fu_result.is_ok());
    auto resolved = fu_result.unwrap().resolve();
    ASSERT_TRUE(resolved.is_ok());
    EXPECT_EQ(resolved.unwrap().result, 14);
}

TEST_F(DeferredHandlerTest, MultipleDeferredCallsNoLeak) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    const int N = 100;
    for (int i = 0; i < N; i++) {
        BenchmarkProxy::RpcDeferredEchoRequest req;
        req.val = i;
        auto result = proxy.deferred_echo(req);
        ASSERT_TRUE(result.is_ok()) << "deferred_echo failed at i=" << i;
        EXPECT_EQ(result.unwrap().result, i * 2);
    }
    EXPECT_EQ(service_->deferred_call_count.load(), N);
}

TEST_F(DeferredHandlerTest, DroppedDeferredReplyNoCrash) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    service_->should_drop_reply = true;

    BenchmarkProxy::RpcDeferredEchoRequest req;
    req.val = 99;
    auto fu_result = proxy.async_deferred_echo(req);
    ASSERT_TRUE(fu_result.is_ok());

    // The server handler drops the DeferredReply without calling reply().
    // The future will never get a response — wait briefly then check it's not ready
    // or has an error. Either way, no crash/leak is the assertion.
    auto fu = fu_result.unwrap();
    std::this_thread::sleep_for(200ms);

    // The request was dispatched but reply was dropped — the future may
    // remain pending or time out. Either way, no crash = success.
    EXPECT_EQ(service_->deferred_call_count.load(), 1);
    // Verify the test didn't crash — if we got here, no double-free occurred.
}

TEST_F(DeferredHandlerTest, ConcurrentDeferredCallsNoLeak) {
    const int N_THREADS = 4;
    const int CALLS_PER_THREAD = 25;
    std::atomic<int> success{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < N_THREADS; t++) {
        auto poll_clone = poll_.as_ref().unwrap().clone();
        threads.emplace_back([&, poll_clone = std::move(poll_clone)]() {
            auto thread_client = Client::create(poll_clone);
            if (thread_client->connect(
                    reinterpret_cast<const int8_t*>(("127.0.0.1:" + std::to_string(port_)).c_str()), true) != 0) {
                return;
            }
            std::this_thread::sleep_for(20ms);
            BenchmarkProxy proxy(const_cast<Client*>(thread_client.get()));

            for (int i = 0; i < CALLS_PER_THREAD; i++) {
                BenchmarkProxy::RpcDeferredEchoRequest req;
                req.val = t * 100 + i;
                auto result = proxy.deferred_echo(req);
                if (result.is_ok() && result.unwrap().result == req.val * 2) {
                    success++;
                }
            }
            thread_client->close();
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(success.load(), N_THREADS * CALLS_PER_THREAD);
    EXPECT_EQ(service_->deferred_call_count.load(), N_THREADS * CALLS_PER_THREAD);
}
