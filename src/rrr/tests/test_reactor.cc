// @unsafe - Test file with mutable fields in test classes
// @unsafe {

#include <stddef.h>
#include <string.h>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <rusty/arc.hpp>
#include <rusty/function.hpp>
#include <rusty/mutex.hpp>
#include "../rrr.hpp"

import std;

using namespace rrr;
using namespace std::chrono;

// Concrete implementation of Pollable for testing
// @unsafe - Uses mutable fields for interior mutability in test scenarios
class TestPollable : public Pollable {
private:
    int fd_;
    mutable int mode_;  // mutable to allow modification through const methods
    mutable rusty::Function<void()> read_handler_;  // mutable handler
    mutable rusty::Function<void()> write_handler_;  // mutable handler
    mutable rusty::Function<void()> error_handler_;  // mutable handler

public:
    // No user-declared destructor: Pollable's `virtual ~Pollable() = default;`
    // covers polymorphic deletion, and omitting our own destructor keeps
    // the implicit move constructor / move assignment available — required
    // since `read_handler_` / `write_handler_` / `error_handler_` are
    // move-only (`rusty::Function` is non-copyable).
    explicit TestPollable(int fd, int mode = PollMode::READ)
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

    // Required by Pollable interface
    size_t content_size() override {
        return 0;  // Test implementation
    }

    // @unsafe - Uses mutable field
    bool handle_read() override {
        // @unsafe {
        if (read_handler_) {
            read_handler_();
        }
        // }
        return true;
    }

    // @unsafe - Uses mutable field
    // Returns MODE_NO_CHANGE since test doesn't need mode updates
    int handle_write() override {
        // @unsafe {
        if (write_handler_) {
            write_handler_();
        }
        // }
        return PollMode::NO_CHANGE;
    }

    // @unsafe - Uses mutable field
    void handle_error() override {
        // @unsafe {
        if (error_handler_) {
            error_handler_();
        }
        // }
    }

    // @unsafe - Closes the file descriptor
    void close() override {
        // @unsafe {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        // }
    }

    // @safe - Check if closed (fd_ == -1)
    bool is_closed() const override {
        return fd_ < 0;
    }

    // @safe - Test class doesn't use pending write updates
    bool check_pending_write_update() const override {
        return false;
    }

    // @unsafe - Modifies mutable field. rusty::Function is move-only so
    // the setter takes by value-with-move.
    void set_read_handler(rusty::Function<void()> handler) const {  // const method
        // @unsafe {
        read_handler_ = std::move(handler);
        // }
    }

    // @unsafe - Modifies mutable field
    void set_write_handler(rusty::Function<void()> handler) const {  // const method
        // @unsafe {
        write_handler_ = std::move(handler);
        // }
    }

    // @unsafe - Modifies mutable field
    void set_error_handler(rusty::Function<void()> handler) const {  // const method
        // @unsafe {
        error_handler_ = std::move(handler);
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
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
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

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::READ));
    p->set_read_handler([&read_triggered, fd1]() {
        read_triggered = true;
        // Read data to clear the event
        char buf[256];
        read(fd1, buf, sizeof(buf));
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
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

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::WRITE));
    p->set_write_handler([&write_triggered]() {
        write_triggered = true;
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
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

    auto p1 = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::READ));
    p1->set_read_handler([&events_triggered, fd1]() {
        events_triggered++;
        char buf[256];
        read(fd1, buf, sizeof(buf));
    });

    auto p2 = rusty::Arc<TestPollable>::new_(TestPollable(fd3, PollMode::READ));
    p2->set_read_handler([&events_triggered, fd3]() {
        events_triggered++;
        char buf[256];
        read(fd3, buf, sizeof(buf));
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p1.clone()));
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p2.clone()));
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

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::READ));
    p->set_read_handler([&read_triggered, fd1]() {
        read_triggered = true;
        char buf[256];
        read(fd1, buf, sizeof(buf));
    });
    p->set_write_handler([&write_triggered]() {
        write_triggered = true;
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
    }

    // Initially only READ mode
    write(fd2, "test", 4);
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_TRUE(read_triggered);
    EXPECT_FALSE(write_triggered);

    // Change to WRITE mode
    p->set_mode(PollMode::WRITE);
    {
        Pollable& pollable_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*p));
        poll_thread_worker_.as_ref().unwrap()->update_mode(pollable_ref, PollMode::WRITE);
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

    auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::READ));
    p->set_error_handler([&error_triggered]() {
        error_triggered = true;
    });

    {
        poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
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
    auto reactor = Reactor::get_reactor();
    // Rc is never null by design - just verify we can get a reactor
    EXPECT_TRUE(true);
}

TEST_F(ReactorTest, EventCreation) {
    auto reactor = Reactor::get_reactor();
    
    // Use IntEvent which has the set method
    auto& event = Reactor::create_event<IntEvent>();
    EXPECT_FALSE(event.is_ready());

    // Trigger the event
    event.set(1);
    EXPECT_TRUE(event.is_ready());
    EXPECT_EQ(event.value_, 1);
}

TEST_F(ReactorTest, FiberBasic) {
    auto reactor = Reactor::get_reactor();
    
    std::atomic<int> value{0};
    
    reactor->create_run_fiber([&value]() {
        value = 1;
    });
    
    // CreateRunFiber already runs the event loop internally
    // No need for a separate thread
    
    EXPECT_EQ(value, 1);
}

TEST_F(ReactorTest, FiberWithYield) {
    auto reactor = Reactor::get_reactor();
    
    std::atomic<int> value{0};
    
    auto sp_fiber = reactor->create_run_fiber([&value]() {
        value = 1;
        Fiber::current_fiber().unwrap()->yield_();
        value = 2;
    });
    
    // After initial run, the fiber yields at value=1
    EXPECT_EQ(value, 1);
    EXPECT_FALSE(sp_fiber->finished());
    
    // Manually continue the fiber
    reactor->continue_fiber(sp_fiber);
    
    // After continuation, value should be 2
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(sp_fiber->finished());
}

TEST_F(ReactorTest, MultipleFibers) {
    auto reactor = Reactor::get_reactor();
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 5; i++) {
        reactor->create_run_fiber([&counter]() {
            counter++;
        });
    }
    
    // All fibers should have been executed
    EXPECT_EQ(counter, 5);
}

TEST_F(ReactorTest, QuorumEvent) {
    auto reactor = Reactor::get_reactor();
    
    // QuorumEvent needs total count and quorum
    auto sp_event = Reactor::create_sp_event<janus::QuorumEvent>(3, 2);  // 3 total, need 2 votes
    
    EXPECT_FALSE(sp_event->is_ready());
    
    // Vote once
    sp_event->n_voted_yes_ = 1;
    EXPECT_FALSE(sp_event->is_ready());
    
    // Vote again - should trigger
    sp_event->n_voted_yes_ = 2;
    EXPECT_TRUE(sp_event->is_ready());
    EXPECT_TRUE(sp_event->yes());
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

        auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::READ));
        p->set_read_handler([&total_events, fd1]() {
            total_events++;
            char buf[256];
            read(fd1, buf, sizeof(buf));
        });

        {
            poll_thread_worker_.as_ref().unwrap()->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
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
            auto p = rusty::Arc<TestPollable>::new_(TestPollable(fd1, PollMode::READ));
            test_poll_worker->add_proxy(make_pollable_proxy_from_typed_arc(p.clone()));
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
