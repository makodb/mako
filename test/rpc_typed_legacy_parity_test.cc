#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono_literals;

class TypedLegacyParityTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_;
    Server* server_ = nullptr;
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
            server_->reg_service(rusty::make_box<BenchmarkService>());
            if (server_->start(("0.0.0.0:" + std::to_string(port_)).c_str()) == 0) {
                started = true;
                break;
            }
            delete server_;
            server_ = nullptr;
        }
        ASSERT_TRUE(started);

        client_ = rusty::Some(Client::create(poll_.as_ref().unwrap()));
        ASSERT_EQ(client_.as_ref().unwrap()->connect(
            ("127.0.0.1:" + std::to_string(port_)).c_str()), 0);
        std::this_thread::sleep_for(50ms);
    }

    void TearDown() override {
        client_.as_ref().unwrap()->close();
        delete server_;
        poll_.as_ref().unwrap()->shutdown();
    }
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

TEST_F(TypedLegacyParityTest, FastPrimeSyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    for (i32 n : {0, 1, 2, 3, 4, 7, 10, 13, 97, 100}) {
        // Typed sync call
        BenchmarkProxy::RpcFastPrimeRequest typed_req;
        typed_req.n = n;
        auto typed_result = proxy.fast_prime(typed_req);
        ASSERT_TRUE(typed_result.is_ok()) << "typed fast_prime failed for n=" << n;
        auto typed_resp = typed_result.unwrap();

        // Legacy sync call
        i8 legacy_flag = -99;
        i32 legacy_err = proxy.fast_prime(n, &legacy_flag);

        EXPECT_EQ(legacy_err, 0) << "legacy fast_prime error for n=" << n;
        EXPECT_EQ(typed_resp.flag, legacy_flag)
            << "parity mismatch for fast_prime(n=" << n << "): "
            << "typed=" << (int)typed_resp.flag << " legacy=" << (int)legacy_flag;
    }
}

TEST_F(TypedLegacyParityTest, FastPrimeAsyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    i32 n = 17;

    // Typed async call
    BenchmarkProxy::RpcFastPrimeRequest typed_req;
    typed_req.n = n;
    auto typed_fu_result = proxy.async_fast_prime(typed_req);
    ASSERT_TRUE(typed_fu_result.is_ok());
    auto typed_resolved = typed_fu_result.unwrap().resolve();
    ASSERT_TRUE(typed_resolved.is_ok());
    i8 typed_flag = typed_resolved.unwrap().flag;

    // Legacy async call
    auto legacy_fu_result = proxy.async_fast_prime(n);
    ASSERT_TRUE(legacy_fu_result.is_ok());
    auto legacy_fu = legacy_fu_result.unwrap();
    legacy_fu->wait();
    ASSERT_EQ(legacy_fu->get_error_code(), 0);
    i8 legacy_flag = -99;
    legacy_fu->get_reply() >> legacy_flag;

    EXPECT_EQ(typed_flag, legacy_flag)
        << "async parity mismatch for fast_prime(17)";
    EXPECT_EQ(typed_flag, 1);  // 17 is prime
}

TEST_F(TypedLegacyParityTest, FastAddSyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    v32 a, b;
    a.set(42);
    b.set(58);

    // Typed sync call
    BenchmarkProxy::RpcFastAddRequest typed_req;
    typed_req.a = a;
    typed_req.b = b;
    auto typed_result = proxy.fast_add(typed_req);
    ASSERT_TRUE(typed_result.is_ok());
    auto typed_sum = typed_result.unwrap().a_add_b;

    // Legacy sync call
    v32 legacy_sum;
    i32 legacy_err = proxy.fast_add(a, b, &legacy_sum);

    EXPECT_EQ(legacy_err, 0);
    EXPECT_EQ(typed_sum.get(), legacy_sum.get());
    EXPECT_EQ(typed_sum.get(), 100);
}

TEST_F(TypedLegacyParityTest, FastDotProdSyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    point3 p1{1.0, 2.0, 3.0};
    point3 p2{4.0, 5.0, 6.0};

    // Typed sync call
    BenchmarkProxy::RpcFastDotProdRequest typed_req;
    typed_req.p1 = p1;
    typed_req.p2 = p2;
    auto typed_result = proxy.fast_dot_prod(typed_req);
    ASSERT_TRUE(typed_result.is_ok());
    double typed_v = typed_result.unwrap().v;

    // Legacy sync call
    double legacy_v = -999.0;
    i32 legacy_err = proxy.fast_dot_prod(p1, p2, &legacy_v);

    EXPECT_EQ(legacy_err, 0);
    EXPECT_DOUBLE_EQ(typed_v, legacy_v);
    EXPECT_DOUBLE_EQ(typed_v, 32.0);  // 1*4 + 2*5 + 3*6
}

TEST_F(TypedLegacyParityTest, FastNopSyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    std::string input = "parity_test";

    // Typed sync call
    BenchmarkProxy::RpcFastNopRequest typed_req;
    typed_req.in_0 = input;
    auto typed_result = proxy.fast_nop(typed_req);
    ASSERT_TRUE(typed_result.is_ok());

    // Legacy sync call (no output param)
    i32 legacy_err = proxy.fast_nop(input);
    EXPECT_EQ(legacy_err, 0);
}

TEST_F(TypedLegacyParityTest, FastVecAsyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    i32 n = 5;

    // Typed async call
    BenchmarkProxy::RpcFastVecRequest typed_req;
    typed_req.n = n;
    auto typed_fu_result = proxy.async_fast_vec(typed_req);
    ASSERT_TRUE(typed_fu_result.is_ok());
    auto typed_resolved = typed_fu_result.unwrap().resolve();
    ASSERT_TRUE(typed_resolved.is_ok());
    auto typed_vec = typed_resolved.unwrap().v;

    // Legacy async call
    auto legacy_fu_result = proxy.async_fast_vec(n);
    ASSERT_TRUE(legacy_fu_result.is_ok());
    auto legacy_fu = legacy_fu_result.unwrap();
    legacy_fu->wait();
    ASSERT_EQ(legacy_fu->get_error_code(), 0);
    std::vector<i64> legacy_vec;
    legacy_fu->get_reply() >> legacy_vec;

    ASSERT_EQ(typed_vec.size(), legacy_vec.size());
    for (size_t i = 0; i < typed_vec.size(); i++) {
        EXPECT_EQ(typed_vec[i], legacy_vec[i]) << "vec mismatch at index " << i;
    }
    EXPECT_EQ(typed_vec.size(), 5u);
}

TEST_F(TypedLegacyParityTest, SlowMethodSyncParity) {
    BenchmarkProxy proxy(const_cast<Client*>(client_.as_ref().unwrap().get()));

    // Typed sync call (non-fast method)
    BenchmarkProxy::RpcPrimeRequest typed_req;
    typed_req.n = 7;
    auto typed_result = proxy.prime(typed_req);
    ASSERT_TRUE(typed_result.is_ok());
    i8 typed_flag = typed_result.unwrap().flag;

    // Legacy sync call
    i8 legacy_flag = -99;
    i32 legacy_err = proxy.prime(static_cast<i32>(7), &legacy_flag);

    EXPECT_EQ(legacy_err, 0);
    EXPECT_EQ(typed_flag, legacy_flag);
    EXPECT_EQ(typed_flag, 1);  // 7 is prime
}

#pragma GCC diagnostic pop
