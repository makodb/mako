#include <stddef.h>

#include <gtest/gtest.h>


#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/function.hpp>

#include "../srpc.hpp"

// PollMode et al. live in srpc.epoll_wrapper (trimmed from the consumer
// umbrella in 08b68144) — import directly.
import srpc.epoll_wrapper;

import std;

namespace srpc {
namespace {

using namespace std::chrono;

// rusty::Function takes the predicate by value-with-move; the
// non-const operator() is called in the loop body.  Each call site
// passes a fresh lambda which auto-converts via Function(Callable&&).
bool wait_until(rusty::Function<bool()> pred, int timeout_ms) {
  const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
  while (steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  return pred();
}

// Plain struct (no PollableBase inheritance): consumed only via the
// typed-arc PollableArcShim<T>, whose hooks are const.
class CountingPollable {
 public:
  CountingPollable(
      int fd,
      int mode,
      std::atomic<int>* read_count,
      std::atomic<int>* write_count,
      std::atomic<int>* close_count)
      : fd_(fd),
        mode_(mode),
        read_count_(read_count),
        write_count_(write_count),
        close_count_(close_count) {}

  // Movable so it can be constructed into an Arc.
  CountingPollable(CountingPollable&& o) noexcept
      : fd_(o.fd_), mode_(o.mode_),
        read_count_(o.read_count_), write_count_(o.write_count_),
        close_count_(o.close_count_) {}

  int fd() const { return fd_; }
  int poll_mode() const { return mode_; }
  size_t content_size() const { return 0; }
  bool handle_read() const {
    char buf[32];
    (void)::read(fd_, buf, sizeof(buf));
    if (read_count_ != nullptr) {
      read_count_->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }
  int handle_write() const {
    if (write_count_ != nullptr) {
      write_count_->fetch_add(1, std::memory_order_relaxed);
    }
    return PollMode::NO_CHANGE;
  }
  void handle_error() const {}
  void close() const {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (close_count_ != nullptr) {
      close_count_->fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool check_pending_write_update() const { return false; }
  bool is_closed() const { return fd_ < 0; }

  void set_mode(int mode) const { mode_ = mode; }

 private:
  mutable int fd_;
  mutable int mode_;
  std::atomic<int>* read_count_;
  std::atomic<int>* write_count_;
  std::atomic<int>* close_count_;
};

class PlainPollable {
 public:
  PlainPollable(
      int fd,
      int mode,
      std::atomic<int>* read_count,
      std::atomic<int>* write_count,
      std::atomic<int>* close_count)
      : fd_(fd),
        mode_(mode),
        read_count_(read_count),
        write_count_(write_count),
        close_count_(close_count) {}

  int fd() const { return fd_; }
  int poll_mode() const { return mode_; }
  size_t content_size() const { return 0; }
  bool handle_read() const {
    char buf[32];
    (void)::read(fd_, buf, sizeof(buf));
    if (read_count_ != nullptr) {
      read_count_->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }
  int handle_write() const {
    if (write_count_ != nullptr) {
      write_count_->fetch_add(1, std::memory_order_relaxed);
    }
    return PollMode::NO_CHANGE;
  }
  void handle_error() const {}
  void close() const {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (close_count_ != nullptr) {
      close_count_->fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool check_pending_write_update() const { return false; }
  bool is_closed() const { return fd_ < 0; }

  void set_mode(int mode) const { mode_ = mode; }

 private:
  mutable int fd_;
  mutable int mode_;
  std::atomic<int>* read_count_;
  std::atomic<int>* write_count_;
  std::atomic<int>* close_count_;
};

TEST(RpcPollThreadProxyStorageTest, RequestCloseInvokesCloseAfterCallerArcReleased) {
  auto poll_thread = PollThread::create();

  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  ASSERT_EQ(::fcntl(sv[0], F_SETFL, O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(sv[1], F_SETFL, O_NONBLOCK), 0);

  std::atomic<int> close_count{0};
  int tracked_fd = -1;
  {
    auto pollable = rusty::Arc<CountingPollable>::new_(
        CountingPollable(sv[0], PollMode::READ, nullptr, nullptr, &close_count));
    tracked_fd = pollable->fd();
    poll_thread->add_proxy(make_pollable_proxy_from_typed_arc(pollable.clone()));
  }

  std::this_thread::sleep_for(milliseconds(60));
  poll_thread->request_close(tracked_fd);

  ASSERT_TRUE(wait_until([&] { return close_count.load(std::memory_order_relaxed) >= 1; }, 1000));
  EXPECT_EQ(close_count.load(std::memory_order_relaxed), 1);

  poll_thread->shutdown();
  ::close(sv[1]);
}

TEST(RpcPollThreadProxyStorageTest, UpdateModeAndRemoveCommandsOperateThroughProxyStorage) {
  auto poll_thread = PollThread::create();

  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  ASSERT_EQ(::fcntl(sv[0], F_SETFL, O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(sv[1], F_SETFL, O_NONBLOCK), 0);

  std::atomic<int> write_count{0};
  auto pollable = rusty::Arc<CountingPollable>::new_(
      CountingPollable(sv[0], PollMode::READ, nullptr, &write_count, nullptr));

  poll_thread->add_proxy(make_pollable_proxy_from_typed_arc(pollable.clone()));
  std::this_thread::sleep_for(milliseconds(60));

  pollable->set_mode(PollMode::WRITE);
  poll_thread->update_mode(pollable->fd(), PollMode::WRITE);

  ASSERT_TRUE(wait_until([&] { return write_count.load(std::memory_order_relaxed) > 0; }, 1000));

  poll_thread->remove_fd(pollable->fd());
  std::this_thread::sleep_for(milliseconds(120));
  const int stable_count = write_count.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(milliseconds(150));
  EXPECT_EQ(write_count.load(std::memory_order_relaxed), stable_count);

  pollable->close();
  poll_thread->shutdown();
  ::close(sv[1]);
}

TEST(RpcPollThreadProxyStorageTest, FdReuseDispatchesToCurrentProxyInstance) {
  auto poll_thread = PollThread::create();

  int first_sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, first_sv), 0);
  ASSERT_EQ(::fcntl(first_sv[0], F_SETFL, O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(first_sv[1], F_SETFL, O_NONBLOCK), 0);

  std::atomic<int> first_read_count{0};
  std::atomic<int> first_close_count{0};
  auto first_pollable = rusty::Arc<CountingPollable>::new_(
      CountingPollable(first_sv[0], PollMode::READ, &first_read_count, nullptr, &first_close_count));
  const int reused_fd = first_pollable->fd();
  poll_thread->add_proxy(make_pollable_proxy_from_typed_arc(first_pollable.clone()));

  ASSERT_GT(::write(first_sv[1], "a", 1), 0);
  ASSERT_TRUE(wait_until([&] { return first_read_count.load(std::memory_order_relaxed) >= 1; }, 1000));

  poll_thread->request_close(reused_fd);
  ASSERT_TRUE(wait_until([&] { return first_close_count.load(std::memory_order_relaxed) >= 1; }, 1000));
  ::close(first_sv[1]);

  int second_sv[2] = {-1, -1};
  bool got_reused_fd = false;
  for (int i = 0; i < 128; ++i) {
    int tmp[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, tmp), 0);
    ASSERT_EQ(::fcntl(tmp[0], F_SETFL, O_NONBLOCK), 0);
    ASSERT_EQ(::fcntl(tmp[1], F_SETFL, O_NONBLOCK), 0);
    if (tmp[0] == reused_fd) {
      second_sv[0] = tmp[0];
      second_sv[1] = tmp[1];
      got_reused_fd = true;
      break;
    }
    ::close(tmp[0]);
    ::close(tmp[1]);
  }
  ASSERT_TRUE(got_reused_fd);

  const int first_reads_after_close = first_read_count.load(std::memory_order_relaxed);

  std::atomic<int> second_read_count{0};
  std::atomic<int> second_close_count{0};
  auto second_pollable = rusty::Arc<CountingPollable>::new_(
      CountingPollable(second_sv[0], PollMode::READ, &second_read_count, nullptr, &second_close_count));
  poll_thread->add_proxy(make_pollable_proxy_from_typed_arc(second_pollable.clone()));

  ASSERT_GT(::write(second_sv[1], "b", 1), 0);
  ASSERT_TRUE(wait_until([&] { return second_read_count.load(std::memory_order_relaxed) >= 1; }, 1000));
  EXPECT_EQ(first_read_count.load(std::memory_order_relaxed), first_reads_after_close);

  poll_thread->request_close(reused_fd);
  ASSERT_TRUE(wait_until([&] { return second_close_count.load(std::memory_order_relaxed) >= 1; }, 1000));

  poll_thread->shutdown();
  ::close(second_sv[1]);
}

TEST(RpcPollThreadProxyStorageTest, DirectTypedProxySupportsNonPollableClass) {
  auto poll_thread = PollThread::create();

  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  ASSERT_EQ(::fcntl(sv[0], F_SETFL, O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(sv[1], F_SETFL, O_NONBLOCK), 0);

  std::atomic<int> read_count{0};
  std::atomic<int> write_count{0};
  std::atomic<int> close_count{0};
  auto plain = rusty::Arc<PlainPollable>::new_(
      PlainPollable(sv[0], PollMode::READ, &read_count, &write_count, &close_count));
  const int fd = plain->fd();

  auto proxy = make_pollable_proxy_from_typed_arc(plain.clone());
  poll_thread->add_proxy(std::move(proxy));

  ASSERT_GT(::write(sv[1], "x", 1), 0);
  ASSERT_TRUE(wait_until([&] { return read_count.load(std::memory_order_relaxed) >= 1; }, 1000));

  plain->set_mode(PollMode::WRITE);
  poll_thread->update_mode(fd, PollMode::WRITE);
  ASSERT_TRUE(wait_until([&] { return write_count.load(std::memory_order_relaxed) >= 1; }, 1000));

  poll_thread->request_close(fd);
  ASSERT_TRUE(wait_until([&] { return close_count.load(std::memory_order_relaxed) >= 1; }, 1000));

  poll_thread->shutdown();
  ::close(sv[1]);
}

}  // namespace
}  // namespace srpc
