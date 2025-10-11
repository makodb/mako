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
    
    int __reg_to__(Server* svr) {
        int ret = 0;
        ret = svr->reg(FAST_ECHO, this, &TestFutureService::fast_echo_wrapper);
        ret = svr->reg(SLOW_ECHO, this, &TestFutureService::slow_echo_wrapper);
        ret = svr->reg(GET_VALUE, this, &TestFutureService::get_value_wrapper);
        ret = svr->reg(ERROR_METHOD, this, &TestFutureService::error_method_wrapper);
        return ret;
    }
    
private:
    void fast_echo_wrapper(rusty::Box<Request> req, std::weak_ptr<ServerConnection> weak_sconn) {
        call_count++;
        std::string input;
        req->m >> input;

        auto sconn = weak_sconn.lock();
        if (sconn) {
            sconn->begin_reply(req.get());
        *sconn << input;
        sconn->end_reply();
        }
        // req (Box) automatically deleted when going out of scope
    }

    void slow_echo_wrapper(rusty::Box<Request> req, std::weak_ptr<ServerConnection> weak_sconn) {
        call_count++;
        std::string input;
        req->m >> input;

        if (should_delay) {
            std::this_thread::sleep_for(milliseconds(delay_ms));
        }

        auto sconn = weak_sconn.lock();
        if (sconn) {
            sconn->begin_reply(req.get());
            *sconn << input;
            sconn->end_reply();
        }
        // req (Box) automatically deleted when going out of scope
    }

    void get_value_wrapper(rusty::Box<Request> req, std::weak_ptr<ServerConnection> weak_sconn) {
        call_count++;
        i32 input;
        req->m >> input;

        i32 result = input * 2;

        auto sconn = weak_sconn.lock();
        if (sconn) {
            sconn->begin_reply(req.get());
        *sconn << result;
        sconn->end_reply();
        }
        // req (Box) automatically deleted when going out of scope
    }

    void error_method_wrapper(rusty::Box<Request> req, std::weak_ptr<ServerConnection> weak_sconn) {
        call_count++;
        // Don't reply - simulate an error
        // req (Box) automatically deleted when going out of scope
        // sconn automatically released by shared_ptr
    }
};

class FutureTest : public ::testing::Test {
protected:
    rusty::Arc<PollThreadWorker> poll_thread_worker_;
    Server* server;
    TestFutureService* service;
    std::shared_ptr<Client> client;
    static constexpr int test_port = 8849;  // Different port from RPC test

    void SetUp() override {
        // Create PollThreadWorker Arc<Mutex<>>
        poll_thread_worker_ = PollThreadWorker::create();

        // Server now takes Arc<Mutex<>>
        server = new Server(poll_thread_worker_);
        service = new TestFutureService();

        server->reg(service);

        ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port)).c_str()), 0);

        // Client takes Arc<Mutex<>>
        client = std::make_shared<Client>(poll_thread_worker_);
        ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port)).c_str()), 0);

        std::this_thread::sleep_for(milliseconds(50));
    }

    void TearDown() override {
        client->close();

        delete service;
        delete server;  // Server destructor waits for connections to close

        // Shutdown PollThreadWorker with proper locking
        {
            poll_thread_worker_->shutdown();
        }
    }
};

TEST_F(FutureTest, BasicFutureCreation) {
    // Create a future through an RPC call
    Future* fu = client->begin_request(TestFutureService::FAST_ECHO);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    // Wait for completion
    fu->wait();
    EXPECT_TRUE(fu->ready());
    
    EXPECT_EQ(fu->get_error_code(), 0);
    
    std::string output;
    fu->get_reply() >> output;
    EXPECT_EQ(input, output);
    
    fu->release();
}

TEST_F(FutureTest, FutureReadyCheck) {
    service->should_delay = true;
    service->delay_ms = 100;
    
    Future* fu = client->begin_request(TestFutureService::SLOW_ECHO);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    // Should not be ready immediately (probably)
    // This is a bit racy but usually works
    
    // Wait and check again
    fu->wait();
    EXPECT_TRUE(fu->ready());
    
    fu->release();
    
    service->should_delay = false;
}

TEST_F(FutureTest, FutureWait) {
    Future* fu = client->begin_request(TestFutureService::FAST_ECHO);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    // wait() should block until ready
    fu->wait();
    
    EXPECT_TRUE(fu->ready());
    EXPECT_EQ(fu->get_error_code(), 0);
    
    fu->release();
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
    
    FutureAttr attr([&](Future* f) {
        callback_called = true;
        callback_error_code = f->get_error_code();
    });
    
    Future* fu = client->begin_request(TestFutureService::FAST_ECHO, attr);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    fu->wait();
    
    // Give callback time to execute
    std::this_thread::sleep_for(milliseconds(50));
    
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_error_code, 0);
    
    fu->release();
}

TEST_F(FutureTest, FutureGetReply) {
    i32 n = 17;
    Future* fu = client->begin_request(TestFutureService::GET_VALUE);
    *client << n;
    client->end_request();
    
    // get_reply() should wait internally
    Marshal& reply = fu->get_reply();
    
    i32 result;
    reply >> result;
    
    EXPECT_EQ(result, 34);  // 17 * 2
    
    fu->release();
}

TEST_F(FutureTest, FutureErrorCode) {
    // Test with invalid RPC ID
    Future* fu = client->begin_request(99999);
    client->end_request();
    
    fu->wait();
    
    // Should have an error
    EXPECT_NE(fu->get_error_code(), 0);
    
    fu->release();
}

TEST_F(FutureTest, MultipleFuturesConcurrent) {
    const int num_futures = 10;
    std::vector<Future*> futures;
    
    // Create multiple futures
    for (int i = 0; i < num_futures; i++) {
        Future* fu = client->begin_request(TestFutureService::FAST_ECHO);
        std::string input = "test_" + std::to_string(i);
        *client << input;
        client->end_request();
        futures.push_back(fu);
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
    
    // Release all
    for (auto fu : futures) {
        fu->release();
    }
}

TEST_F(FutureTest, FutureReleaseWithoutWait) {
    // Create a future but don't wait for it
    Future* fu = client->begin_request(TestFutureService::FAST_ECHO);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    // Release without waiting - should be safe
    fu->release();
    
    // Give time for the response to arrive
    std::this_thread::sleep_for(milliseconds(100));
}

TEST_F(FutureTest, StressTestManyFutures) {
    const int num_futures = 100;
    std::vector<Future*> futures;
    
    // Create many futures rapidly
    for (int i = 0; i < num_futures; i++) {
        i32 n = i;
        Future* fu = client->begin_request(TestFutureService::GET_VALUE);
        *client << n;
        client->end_request();
        futures.push_back(fu);
    }
    
    // Check results
    for (int i = 0; i < num_futures; i++) {
        futures[i]->wait();
        EXPECT_EQ(futures[i]->get_error_code(), 0);
        
        i32 result;
        futures[i]->get_reply() >> result;
        EXPECT_EQ(result, i * 2);
        
        futures[i]->release();
    }
}

// This test should now pass with pthread_cond_broadcast fix
TEST_F(FutureTest, ConcurrentWaitersOnSameFuture) {
    service->should_delay = true;
    service->delay_ms = 200;
    
    Future* fu = client->begin_request(TestFutureService::SLOW_ECHO);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    std::atomic<int> wait_count{0};
    const int num_threads = 5;
    std::vector<std::thread> threads;
    
    // Multiple threads waiting on the same future
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
    
    fu->release();
    
    service->should_delay = false;
}

TEST_F(FutureTest, TimedWaitWithQuickResponse) {
    // Test timed_wait when response comes quickly
    Future* fu = client->begin_request(TestFutureService::FAST_ECHO);
    std::string input = "test";
    *client << input;
    client->end_request();
    
    // Wait for up to 5 seconds (but should complete much faster)
    fu->timed_wait(5.0);
    
    EXPECT_TRUE(fu->ready());
    EXPECT_EQ(fu->get_error_code(), 0);
    
    fu->release();
}

TEST_F(FutureTest, MixedSyncAsync) {
    // Create some futures
    Future* fu1 = client->begin_request(TestFutureService::FAST_ECHO);
    std::string input1 = "first";
    *client << input1;
    client->end_request();
    
    Future* fu2 = client->begin_request(TestFutureService::GET_VALUE);
    i32 val = 50;
    *client << val;
    client->end_request();
    
    Future* fu3 = client->begin_request(TestFutureService::FAST_ECHO);
    std::string input3 = "third";
    *client << input3;
    client->end_request();
    
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
    
    fu1->release();
    fu2->release();
    fu3->release();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}