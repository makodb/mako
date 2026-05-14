#include <gtest/gtest.h>
#include <rusty/arc.hpp>

#include "../rrr.hpp"

namespace rrr {
namespace {

class CountingPollable : public Pollable {
 public:
  explicit CountingPollable(int fd) : fd_(fd) {}

  int fd() const override { return fd_; }
  int poll_mode() const override { return poll_mode_; }
  size_t content_size() override {
    ++content_size_calls_;
    return 64;
  }
  bool handle_read() override {
    ++read_calls_;
    return true;
  }
  int handle_write() override {
    ++write_calls_;
    return PollMode::NO_CHANGE;
  }
  void handle_error() override { ++error_calls_; }
  void close() override {
    ++close_calls_;
    closed_ = true;
  }
  bool check_pending_write_update() const override {
    return pending_write_update_;
  }
  bool is_closed() const override { return closed_; }

  void set_pending_write_update(bool enabled) const { pending_write_update_ = enabled; }

  int content_size_calls() const { return content_size_calls_; }
  int read_calls() const { return read_calls_; }
  int write_calls() const { return write_calls_; }
  int error_calls() const { return error_calls_; }
  int close_calls() const { return close_calls_; }

 private:
  int fd_;
  int poll_mode_{PollMode::READ | PollMode::WRITE};
  mutable bool pending_write_update_{false};
  bool closed_{false};
  int content_size_calls_{0};
  int read_calls_{0};
  int write_calls_{0};
  int error_calls_{0};
  int close_calls_{0};
};

TEST(RpcPollableProxyFacadeTest, AdapterForwardsAllPollableMethods) {
  auto pollable = rusty::Arc<CountingPollable>::make(42);
  auto proxy = make_pollable_proxy_from_typed_arc(pollable);

  EXPECT_EQ(proxy->fd(), 42);
  EXPECT_EQ(proxy->poll_mode(), PollMode::READ | PollMode::WRITE);

  EXPECT_EQ(proxy->content_size(), 64u);
  EXPECT_TRUE(proxy->handle_read());
  EXPECT_EQ(proxy->handle_write(), PollMode::NO_CHANGE);
  proxy->handle_error();

  EXPECT_EQ(pollable->content_size_calls(), 1);
  EXPECT_EQ(pollable->read_calls(), 1);
  EXPECT_EQ(pollable->write_calls(), 1);
  EXPECT_EQ(pollable->error_calls(), 1);
}

TEST(RpcPollableProxyFacadeTest, AdapterPropagatesCloseAndPendingState) {
  auto pollable = rusty::Arc<CountingPollable>::make(77);
  auto proxy = make_pollable_proxy_from_typed_arc(pollable);

  EXPECT_FALSE(proxy->check_pending_write_update());
  pollable->set_pending_write_update(true);
  EXPECT_TRUE(proxy->check_pending_write_update());

  EXPECT_FALSE(proxy->is_closed());
  proxy->close();
  EXPECT_TRUE(proxy->is_closed());
  EXPECT_EQ(pollable->close_calls(), 1);
}

}  // namespace
}  // namespace rrr
