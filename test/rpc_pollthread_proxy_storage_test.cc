#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rusty/arc.hpp>

#include "reactor/reactor.h"

namespace rrr {
namespace {

using namespace std::chrono;

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
  while (steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  return pred();
}

class CountingPollable : public Pollable {
 public:
  CountingPollable(int fd, int mode, std::atomic<int>* write_count, std::atomic<int>* close_count)
      : fd_(fd), mode_(mode), write_count_(write_count), close_count_(close_count) {}

  int fd() const override { return fd_; }
  int poll_mode() const override { return mode_; }
  size_t content_size() override { return 0; }
  bool handle_read() override {
    char buf[32];
    (void)::read(fd_, buf, sizeof(buf));
    return true;
  }
  int handle_write() override {
    if (write_count_ != nullptr) {
      write_count_->fetch_add(1, std::memory_order_relaxed);
    }
    return PollMode::NO_CHANGE;
  }
  void handle_error() override {}
  void close() override {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (close_count_ != nullptr) {
      close_count_->fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool check_pending_write_update() const override { return false; }
  bool is_closed() const override { return fd_ < 0; }

  void set_mode(int mode) const { mode_ = mode; }

 private:
  int fd_;
  mutable int mode_;
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
        CountingPollable(sv[0], PollMode::READ, nullptr, &close_count));
    tracked_fd = pollable->fd();
    poll_thread->add(pollable.clone());
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
      CountingPollable(sv[0], PollMode::READ, &write_count, nullptr));
  Pollable& poll_ref = const_cast<Pollable&>(static_cast<const Pollable&>(*pollable));

  poll_thread->add(pollable.clone());
  std::this_thread::sleep_for(milliseconds(60));

  pollable->set_mode(PollMode::WRITE);
  poll_thread->update_mode(poll_ref, PollMode::WRITE);

  ASSERT_TRUE(wait_until([&] { return write_count.load(std::memory_order_relaxed) > 0; }, 1000));

  poll_thread->remove(poll_ref);
  std::this_thread::sleep_for(milliseconds(120));
  const int stable_count = write_count.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(milliseconds(150));
  EXPECT_EQ(write_count.load(std::memory_order_relaxed), stable_count);

  poll_ref.close();
  poll_thread->shutdown();
  ::close(sv[1]);
}

}  // namespace
}  // namespace rrr
