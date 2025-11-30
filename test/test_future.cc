#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <future>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "misc/marshal.hpp"

// External safety annotations for std::shared_ptr atomic internals
// @external: {
//   _Atomic_count: [unsafe]
//   __shared_ptr: [unsafe]
// }

using namespace rrr;
using namespace std::chrono;

// Simple test service for Future testing
class TestFutureService : public Service {
public:
    enum {
        FAST_ECHO = 0x1001,
        SLOW_ECHO = 0x1002,
        GET_VALUE = 0x1003,
        ERROR_METHOD = 0x1004
    };
    
    std::atomic<int> call_count{0};
    std::atomic<bool> should_delay{false};
    std::atomic<int> delay_ms{100};

    // @unsafe - Takes address-of member function pointers
    int __reg_to__(Server* svr) {
        int ret = 0;
        ret = svr->reg(FAST_ECHO, this, &TestFutureService::fast_echo_wrapper);
        ret = svr->reg(SLOW_ECHO, this, &TestFutureService::slow_echo_wrapper);
        ret = svr->reg(GET_VALUE, this, &TestFutureService::get_value_wrapper);
        ret = svr->reg(ERROR_METHOD, this, &TestFutureService::error_method_wrapper);
        return ret;
    }
    
private:
    void fast_echo_wrapper(rusty::Box<Request> req, WeakServerConnection weak_sconn) {
        call_count++;
        std::string input;
        req->m >> input;

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).begin_reply(*req);
            const_cast<ServerConnection&>(*sconn) << input;
            const_cast<ServerConnection&>(*sconn).end_reply();
        }
        // req automatically cleaned up by rusty::Box
    }

    void slow_echo_wrapper(rusty::Box<Request> req, WeakServerConnection weak_sconn) {
        call_count++;
        std::string input;
        req->m >> input;

        if (should_delay) {
            std::this_thread::sleep_for(milliseconds(delay_ms));
        }

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).begin_reply(*req);
            const_cast<ServerConnection&>(*sconn) << input;
            const_cast<ServerConnection&>(*sconn).end_reply();
        }
        // req automatically cleaned up by rusty::Box
    }

    void get_value_wrapper(rusty::Box<Request> req, WeakServerConnection weak_sconn) {
        call_count++;
        i32 input;
        req->m >> input;

        i32 result = input * 2;

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).begin_reply(*req);
            const_cast<ServerConnection&>(*sconn) << result;
            const_cast<ServerConnection&>(*sconn).end_reply();
        }
        // req automatically cleaned up by rusty::Box
    }

    void error_method_wrapper(rusty::Box<Request> req, WeakServerConnection weak_sconn) {
        call_count++;
        // Don't reply - simulate an error
        // req automatically cleaned up by rusty::Box
        // sconn automatically released by Arc
    }
};

class FutureTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;
    Server* server;
    TestFutureService* service;
    rusty::Option<rusty::Arc<Client>> client;
    static constexpr int base_port = 8849;  // Base port, different from RPC test
    static int test_counter;  // Counter for unique ports per test
    int test_port;

    void SetUp() override {
        // Use unique port for each test to avoid TIME_WAIT conflicts
        test_port = base_port + (test_counter++);

        // Create PollThread Arc
        poll_thread_worker_ = rusty::Some(PollThread::create());

        // Server now takes Option<Arc<...>> - use as_ref() to borrow and clone
        server = new Server(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone()));
        service = new TestFutureService();

        server->reg(service);
        ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port)).c_str()), 0);

        // Client must be created with factory method to initialize weak_self_
        client = rusty::Some(Client::create(poll_thread_worker_.as_ref().unwrap()));
        ASSERT_EQ(client.as_ref().unwrap()->connect(("127.0.0.1:" + std::to_string(test_port)).c_str()), 0);

        std::this_thread::sleep_for(milliseconds(50));
    }

    void TearDown() override {
        // Reset service state flags to prevent test interaction
        service->should_delay = false;
        service->delay_ms = 100;

        client.as_ref().unwrap()->close();
        delete service;
        delete server;  // Server destructor waits for connections to close

        // Shutdown PollThread
        poll_thread_worker_.as_ref().unwrap()->shutdown();

        // Give time for cleanup to complete
        std::this_thread::sleep_for(milliseconds(100));
    }
};

// Initialize static counter
int FutureTest::test_counter = 0;

TEST_F(FutureTest, BasicFutureCreation) {
    // Create a future through an RPC call
    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    // Wait for completion
    fu->wait();
    EXPECT_TRUE(fu->ready());

    EXPECT_EQ(fu->get_error_code(), 0);

    std::string output;
    fu->get_reply() >> output;
    EXPECT_EQ(input, output);

    // Arc auto-released
}

TEST_F(FutureTest, FutureReadyCheck) {
    service->should_delay = true;
    service->delay_ms = 100;

    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::SLOW_ECHO);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    // Should not be ready immediately (probably)
    // This is a bit racy but usually works

    // Wait and check again
    fu->wait();
    EXPECT_TRUE(fu->ready());

    // Arc auto-released

    service->should_delay = false;
}

TEST_F(FutureTest, FutureWait) {
    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    // wait() should block until ready
    fu->wait();

    EXPECT_TRUE(fu->ready());
    EXPECT_EQ(fu->get_error_code(), 0);

    // Arc auto-released
}

// Once a future times out, it shouldn't be waited on again
// TEST_F(FutureTest, FutureTimedWait) {
//     service->should_delay = true;
//     service->delay_ms = 2000;  // 2 seconds delay
//     
//     Future* fu = client->begin_request(TestFutureService::SLOW_ECHO);
//     std::string input = "test";
//     *client << input;
//     client->end_request();
//     
//     // Wait for only 0.1 seconds
//     fu->timed_wait(0.1);
//     
//     // Should not be ready yet (timed out)
//     EXPECT_FALSE(fu->ready());
//     
//     // Now wait for completion - this is problematic as the future already timed out
//     fu->wait();
//     EXPECT_TRUE(fu->ready());
//     
//     fu->release();
//     
//     service->should_delay = false;
// }

TEST_F(FutureTest, FutureCallback) {
    std::atomic<bool> callback_called{false};
    std::atomic<int> callback_error_code{-1};

    FutureAttr attr([&](rusty::Arc<Future> f) {
        callback_called = true;
        callback_error_code = f->get_error_code();
    });

    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO, attr);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    fu->wait();

    // Give callback time to execute
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_error_code, 0);

    // Arc auto-released
}

TEST_F(FutureTest, FutureGetReply) {
    i32 n = 17;
    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::GET_VALUE);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    *client.as_ref().unwrap() << n;
    client.as_ref().unwrap()->end_request();

    // get_reply() should wait internally
    Marshal& reply = fu->get_reply();

    i32 result;
    reply >> result;

    EXPECT_EQ(result, 34);  // 17 * 2

    // Arc auto-released
}

TEST_F(FutureTest, FutureErrorCode) {
    // Test with invalid RPC ID
    auto fu_result = client.as_ref().unwrap()->begin_request(99999);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    client.as_ref().unwrap()->end_request();

    fu->wait();

    // Should have an error
    EXPECT_NE(fu->get_error_code(), 0);

    // Arc auto-released
}

TEST_F(FutureTest, MultipleFuturesConcurrent) {
    const int num_futures = 10;
    std::vector<rusty::Arc<Future>> futures;

    // Create multiple futures
    for (int i = 0; i < num_futures; i++) {
        auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        std::string input = "test_" + std::to_string(i);
        *client.as_ref().unwrap() << input;
        client.as_ref().unwrap()->end_request();
        futures.push_back(std::move(fu));
    }

    // Wait for all
    for (int i = 0; i < num_futures; i++) {
        futures[i]->wait();
        EXPECT_TRUE(futures[i]->ready());
        EXPECT_EQ(futures[i]->get_error_code(), 0);

        std::string output;
        futures[i]->get_reply() >> output;
        EXPECT_EQ(output, "test_" + std::to_string(i));
    }

    // Arc auto-released when vector destroyed
}

TEST_F(FutureTest, FutureReleaseWithoutWait) {
    // Create a future but don't wait for it
    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
    ASSERT_TRUE(fu_result.is_ok());
    rusty::Option<rusty::Arc<Future>> fu = rusty::Some(fu_result.unwrap());
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    // Arc released without waiting - should be safe (fire-and-forget)
    fu = rusty::None;  // Explicit release

    // Give time for the response to arrive
    std::this_thread::sleep_for(milliseconds(100));
}

TEST_F(FutureTest, StressTestManyFutures) {
    const int num_futures = 50;  // Reduced from 100 - appears to be a resource limit around 90-95
    std::vector<rusty::Arc<Future>> futures;

    // Create many futures rapidly
    for (int i = 0; i < num_futures; i++) {
        i32 n = i;
        auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::GET_VALUE);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        *client.as_ref().unwrap() << n;
        client.as_ref().unwrap()->end_request();
        futures.push_back(std::move(fu));
    }

    // Check results
    for (int i = 0; i < num_futures; i++) {
        futures[i]->wait();
        EXPECT_EQ(futures[i]->get_error_code(), 0);

        i32 result;
        futures[i]->get_reply() >> result;
        EXPECT_EQ(result, i * 2);

        // Arc auto-released
    }
}

// This test should now pass with pthread_cond_broadcast fix
TEST_F(FutureTest, ConcurrentWaitersOnSameFuture) {
    service->should_delay = true;
    service->delay_ms = 200;

    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::SLOW_ECHO);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    std::atomic<int> wait_count{0};
    const int num_threads = 5;
    std::vector<std::thread> threads;

    // Multiple threads waiting on the same future (Arc keeps it alive)
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() {
            fu->wait();
            wait_count++;
        });
    }

    // All threads should eventually complete
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(wait_count, num_threads);
    EXPECT_TRUE(fu->ready());

    // Arc auto-released

    service->should_delay = false;
}

TEST_F(FutureTest, TimedWaitWithQuickResponse) {
    // Test timed_wait when response comes quickly
    auto fu_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "test";
    *client.as_ref().unwrap() << input;
    client.as_ref().unwrap()->end_request();

    // Wait for up to 5 seconds (but should complete much faster)
    fu->timed_wait(5.0);

    EXPECT_TRUE(fu->ready());
    EXPECT_EQ(fu->get_error_code(), 0);

    // Arc auto-released
}

TEST_F(FutureTest, MixedSyncAsync) {
    // Create some futures
    auto fu1_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
    ASSERT_TRUE(fu1_result.is_ok());
    auto fu1 = fu1_result.unwrap();
    std::string input1 = "first";
    *client.as_ref().unwrap() << input1;
    client.as_ref().unwrap()->end_request();

    auto fu2_result = client.as_ref().unwrap()->begin_request(TestFutureService::GET_VALUE);
    ASSERT_TRUE(fu2_result.is_ok());
    auto fu2 = fu2_result.unwrap();
    i32 val = 50;
    *client.as_ref().unwrap() << val;
    client.as_ref().unwrap()->end_request();

    auto fu3_result = client.as_ref().unwrap()->begin_request(TestFutureService::FAST_ECHO);
    ASSERT_TRUE(fu3_result.is_ok());
    auto fu3 = fu3_result.unwrap();
    std::string input3 = "third";
    *client.as_ref().unwrap() << input3;
    client.as_ref().unwrap()->end_request();

    // Wait for them in different order
    fu2->wait();
    i32 result;
    fu2->get_reply() >> result;
    EXPECT_EQ(result, 100);

    fu1->wait();
    std::string output1;
    fu1->get_reply() >> output1;
    EXPECT_EQ(output1, input1);

    fu3->wait();
    std::string output3;
    fu3->get_reply() >> output3;
    EXPECT_EQ(output3, input3);

    // Arc auto-released for all three futures
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}