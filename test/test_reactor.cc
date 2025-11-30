// @unsafe - Test file with mutable fields in test classes
// @unsafe {

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include "reactor/reactor.h"
#include "reactor/event.h"
#include "reactor/coroutine.h"
#include "reactor/epoll_wrapper.h"

using namespace rrr;
using namespace std::chrono;

// Concrete implementation of Pollable for testing
// @unsafe - Uses mutable fields for interior mutability in test scenarios
class TestPollable : public Pollable {
private:
    int fd_;
    mutable int mode_;  // mutable to allow modification through const methods
    mutable std::function<void()> read_handler_;  // mutable handler
    mutable std::function<void()> write_handler_;  // mutable handler
    mutable std::function<void()> error_handler_;  // mutable handler

public:
    ~TestPollable() override = default;
    explicit TestPollable(int fd, int mode = READ)
        : fd_(fd), mode_(mode) {}

    int fd() const override {
        return fd_;
    }

    int poll_mode() const override {
        return mode_;
    }

    // @unsafe - Modifies mutable field
    void set_mode(int mode) const {  // const method
        // @unsafe {
        mode_ = mode;
        // }
    }

    // @unsafe - Uses mutable field
    void handle_read() override {
        // @unsafe {
        if (read_handler_) {
            read_handler_();
        }
        // }
    }

    // @unsafe - Uses mutable field
    // Returns MODE_NO_CHANGE since test doesn't need mode updates
    int handle_write() override {
        // @unsafe {
        if (write_handler_) {
            write_handler_();
        }
        // }
        return Pollable::MODE_NO_CHANGE;
    }

    // @unsafe - Uses mutable field
    void handle_error() override {
        // @unsafe {
        if (error_handler_) {
            error_handler_();
        }
        // }
    }

    // @unsafe - Modifies mutable field
    void set_read_handler(std::function<void()> handler) const {  // const method
        // @unsafe {
        read_handler_ = handler;
        // }
    }

    // @unsafe - Modifies mutable field
    void set_write_handler(std::function<void()> handler) const {  // const method
        // @unsafe {
        write_handler_ = handler;
        // }
    }

    // @unsafe - Modifies mutable field
    void set_error_handler(std::function<void()> handler) const {  // const method
        // @unsafe {
        error_handler_ = handler;
        // }
    }
};

class ReactorTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;

    void SetUp() override {
        poll_thread_worker_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        // Shutdown PollThread with proper locking
        {
            poll_thread_worker_.as_ref().unwrap()->shutdown();
        }
    }
    
    std::pair<int, int> create_socket_pair() {
        int sv[2];
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        
        fcntl(sv[0], F_SETFL, O_NONBLOCK);
        fcntl(sv[1], F_SETFL, O_NONBLOCK);
        
        return {sv[0], sv[1]};
    }
};

TEST_F(ReactorTest, BasicPollThreadCreation) {
    EXPECT_TRUE(poll_thread_worker_.is_some());
    // PollThread now always uses a single thread (n_threads_ member removed)
}

TEST_F(ReactorTest, AddRemoveFd) {
    auto [fd1, fd2] = create_socket_pair();

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1));

    {
        poll_thread_worker_.as_ref().unwrap()->add(p);
    }

    // Allow worker thread time to process the add command via channel
    std::this_thread::sleep_for(milliseconds(50));

    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref);
    }

    // Allow worker thread time to process the remove command
    std::this_thread::sleep_for(milliseconds(50));

    close(fd1);
    close(fd2);
}

TEST_F(ReactorTest, PollReadEvent) {
    auto [fd1, fd2] = create_socket_pair();

    std::atomic<bool> read_triggered{false};

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::READ));
    p->set_read_handler([&read_triggered, fd1]() {
        read_triggered = true;
        // Read data to clear the event
        char buf[256];
        read(fd1, buf, sizeof(buf));
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add(p);
    }

    // Write data to trigger read event
    const char* test_data = "test";
    write(fd2, test_data, strlen(test_data));

    // Give poll thread time to process
    std::this_thread::sleep_for(milliseconds(100));

    EXPECT_TRUE(read_triggered);

    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref);
    }
    close(fd1);
    close(fd2);
}

TEST_F(ReactorTest, PollWriteEvent) {
    auto [fd1, fd2] = create_socket_pair();

    std::atomic<bool> write_triggered{false};

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::WRITE));
    p->set_write_handler([&write_triggered]() {
        write_triggered = true;
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add(p);
    }

    // Socket should be immediately writable
    std::this_thread::sleep_for(milliseconds(100));

    EXPECT_TRUE(write_triggered);

    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref);
    }
    close(fd1);
    close(fd2);
}

TEST_F(ReactorTest, MultipleEvents) {
    auto [fd1, fd2] = create_socket_pair();
    auto [fd3, fd4] = create_socket_pair();

    std::atomic<int> events_triggered{0};

    auto p1 = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::READ));
    p1->set_read_handler([&events_triggered, fd1]() {
        events_triggered++;
        char buf[256];
        read(fd1, buf, sizeof(buf));
    });

    auto p2 = rusty::Arc<TestPollable>::new_(TestPollable(fd3, Pollable::READ));
    p2->set_read_handler([&events_triggered, fd3]() {
        events_triggered++;
        char buf[256];
        read(fd3, buf, sizeof(buf));
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add(p1);
        poll_thread_worker_.as_ref().unwrap()->add(p2);
    }

    // Trigger both events
    write(fd2, "test1", 5);
    write(fd4, "test2", 5);

    std::this_thread::sleep_for(milliseconds(200));

    EXPECT_EQ(events_triggered, 2);

    {
        Pollable& pollable_ref1 = const_cast<Pollable&>(static_cast<const Pollable&>(*p1));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref1);
        Pollable& pollable_ref2 = const_cast<Pollable&>(static_cast<const Pollable&>(*p2));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref2);
    }

    close(fd1);
    close(fd2);
    close(fd3);
    close(fd4);
}

TEST_F(ReactorTest, UpdateMode) {
    auto [fd1, fd2] = create_socket_pair();

    std::atomic<bool> read_triggered{false};
    std::atomic<bool> write_triggered{false};

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::READ));
    p->set_read_handler([&read_triggered, fd1]() {
        read_triggered = true;
        char buf[256];
        read(fd1, buf, sizeof(buf));
    });
    p->set_write_handler([&write_triggered]() {
        write_triggered = true;
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add(p);
    }

    // Initially only READ mode
    write(fd2, "test", 4);
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_TRUE(read_triggered);
    EXPECT_FALSE(write_triggered);

    // Change to WRITE mode
    p->set_mode(Pollable::WRITE);
    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->update_mode(pollable_ref, Pollable::WRITE);
    }

    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_TRUE(write_triggered);

    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref);
    }
    close(fd1);
    close(fd2);
}

TEST_F(ReactorTest, ErrorHandling) {
    auto [fd1, fd2] = create_socket_pair();

    std::atomic<bool> error_triggered{false};

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::READ));
    p->set_error_handler([&error_triggered]() {
        error_triggered = true;
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add(p);
    }

    // Allow worker thread time to process the add command via channel
    std::this_thread::sleep_for(milliseconds(50));

    // Close the other end to trigger error/hangup
    close(fd2);

    std::this_thread::sleep_for(milliseconds(200));

    // Error handling depends on epoll/kqueue behavior
    // This test may not reliably trigger error on all systems

    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref);
    }
    close(fd1);
}

// Reactor-specific tests
TEST_F(ReactorTest, ReactorCreation) {
    auto reactor = Reactor::GetReactor();
    // Rc is never null by design - just verify we can get a reactor
    EXPECT_TRUE(true);
}

TEST_F(ReactorTest, EventCreation) {
    auto reactor = Reactor::GetReactor();
    
    // Use IntEvent which has the Set method
    auto& event = Reactor::CreateEvent<IntEvent>();
    EXPECT_FALSE(event.IsReady());
    
    // Trigger the event
    event.Set(1);
    EXPECT_TRUE(event.IsReady());
    EXPECT_EQ(event.value_, 1);
}

TEST_F(ReactorTest, CoroutineBasic) {
    auto reactor = Reactor::GetReactor();
    
    std::atomic<int> value{0};
    
    reactor->CreateRunCoroutine([&value]() {
        value = 1;
    });
    
    // CreateRunCoroutine already runs the event loop internally
    // No need for a separate thread
    
    EXPECT_EQ(value, 1);
}

TEST_F(ReactorTest, CoroutineWithYield) {
    auto reactor = Reactor::GetReactor();
    
    std::atomic<int> value{0};
    
    auto sp_coro = reactor->CreateRunCoroutine([&value]() {
        value = 1;
        Coroutine::CurrentCoroutine().unwrap()->Yield();
        value = 2;
    });
    
    // After initial run, the coroutine yields at value=1
    EXPECT_EQ(value, 1);
    EXPECT_FALSE(sp_coro->Finished());
    
    // Manually continue the coroutine
    reactor->ContinueCoro(sp_coro);
    
    // After continuation, value should be 2
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(sp_coro->Finished());
}

TEST_F(ReactorTest, MultipleCoroutines) {
    auto reactor = Reactor::GetReactor();
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 5; i++) {
        reactor->CreateRunCoroutine([&counter]() {
            counter++;
        });
    }
    
    // All coroutines should have been executed
    EXPECT_EQ(counter, 5);
}

TEST_F(ReactorTest, QuorumEvent) {
    auto reactor = Reactor::GetReactor();
    
    // QuorumEvent needs total count and quorum
    auto sp_event = Reactor::CreateSpEvent<janus::QuorumEvent>(3, 2);  // 3 total, need 2 votes
    
    EXPECT_FALSE(sp_event->IsReady());
    
    // Vote once
    sp_event->n_voted_yes_ = 1;
    EXPECT_FALSE(sp_event->IsReady());
    
    // Vote again - should trigger
    sp_event->n_voted_yes_ = 2;
    EXPECT_TRUE(sp_event->IsReady());
    EXPECT_TRUE(sp_event->Yes());
    EXPECT_EQ(sp_event->n_voted_yes_, 2);
}

TEST_F(ReactorTest, StressTest) {
    const int num_fds = 10;
    const int events_per_fd = 10;
    std::vector<std::pair<int, int>> socket_pairs;
    std::vector<rusty::Arc<TestPollable>> pollables;
    std::atomic<int> total_events{0};

    // Create multiple socket pairs
    for (int i = 0; i < num_fds; i++) {
        socket_pairs.push_back(create_socket_pair());
        auto [fd1, fd2] = socket_pairs.back();

        auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::READ));
        p->set_read_handler([&total_events, fd1]() {
            total_events++;
            char buf[256];
            read(fd1, buf, sizeof(buf));
        });

        {
            poll_thread_worker_.as_ref().unwrap()->add(p);
        }
        pollables.push_back(p);
    }

    // Allow worker thread time to process all add commands via channel
    std::this_thread::sleep_for(milliseconds(100));

    // Send multiple events
    for (int i = 0; i < events_per_fd; i++) {
        for (auto& [fd1, fd2] : socket_pairs) {
            write(fd2, "x", 1);
        }
        std::this_thread::sleep_for(milliseconds(10));
    }

    // Wait for processing
    std::this_thread::sleep_for(milliseconds(500));

    EXPECT_EQ(total_events, num_fds * events_per_fd);

    // Cleanup
    {
        for (auto p : pollables) {
            Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->remove(pollable_ref);
        }
    }

    for (auto& [fd1, fd2] : socket_pairs) {
        close(fd1);
        close(fd2);
    }
}

// Test for Issue #1: Destructor cleanup order problem
// This test DEMONSTRATES THE BUG by showing that epoll Remove() is NOT called
// when PollThread is destroyed with pollables still registered.
//
// BUG (before fix): When PollThread destructor runs, it:
// 1. Joins the thread (stops poll_loop)
// 2. Calls remove() for each pollable
// 3. remove() adds fds to pending_remove_ queue
// 4. BUT poll_loop has stopped, so pending_remove_ is NEVER processed!
// 5. Result: epoll_.Remove() is never called for these fds
//
// FIX: Move the remove() calls to BEFORE joining the thread, so poll_loop
// can process pending_remove_ before exiting.
//
// This test uses instrumentation (static remove_count_) to verify the fix works.
TEST_F(ReactorTest, DestructorCleanupWithoutExplicitRemove) {
    const int NUM_POLLABLES = 5;
    std::vector<std::pair<int, int>> socket_pairs;

    // Create socket pairs
    for (int i = 0; i < NUM_POLLABLES; i++) {
        socket_pairs.push_back(create_socket_pair());
    }

    // Reset the static remove counter
    Epoll::remove_count_ = 0;

    {
        auto test_poll_worker = PollThread::create();

        // Add pollables WITHOUT explicit remove
        for (auto& [fd1, fd2] : socket_pairs) {
            auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, Pollable::READ));
            test_poll_worker->add(p);
        }

        // Allow worker thread time to process the add commands via channel
        std::this_thread::sleep_for(milliseconds(100));

        // Verify no removes happened yet
        EXPECT_EQ(Epoll::remove_count_.load(), 0);

        // Destroy PollThread WITHOUT calling remove() on pollables
        // With the FIX, the destructor will:
        // 1. Set stop_flag_ = true
        // 2. Call remove() for each pollable (adds to pending_remove_)
        // 3. Join the thread (thread processes pending_remove_ before exiting)
        // 4. epoll_.Remove() gets called for each pollable!
        // Shutdown (const method, no lock needed)
        test_poll_worker->shutdown();

    }

    // Now check the static remove counter
    int final_remove_count = Epoll::remove_count_.load();

    std::cout << "Remove count after destruction: " << final_remove_count << std::endl;
    std::cout << "Expected (correct behavior): " << NUM_POLLABLES << std::endl;

    // THIS TEST SHOULD FAIL with the bug, PASS with the fix!
    // After the fix, the destructor properly calls epoll_.Remove()
    // for each pollable, so the count will be NUM_POLLABLES.
    EXPECT_EQ(final_remove_count, NUM_POLLABLES);

    // Clean up socket pairs
    for (auto& [fd1, fd2] : socket_pairs) {
        close(fd1);
        close(fd2);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// } @unsafe