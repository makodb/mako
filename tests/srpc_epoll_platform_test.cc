#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

import srpc.epoll_wrapper;

namespace {

class ScopedFd {
public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  ~ScopedFd()
  {
    if (fd_ >= 0)
      close(fd_);
  }

  int get() const { return fd_; }

private:
  int fd_;
};

void ExpectEventData(const epoll_event &event, int expected_fd)
{
  epoll_data_t expected{};
  expected.fd = expected_fd;

  uint8_t actual[sizeof(epoll_data_t)]{};
  memcpy(actual,
         reinterpret_cast<const uint8_t *>(&event) +
           offsetof(epoll_event, data),
         sizeof(actual));
  EXPECT_EQ(memcmp(actual, &expected, sizeof(expected)), 0);
}

TEST(EpollPlatformTest, AddAndUpdatePreserveReadyFileDescriptor)
{
  const ScopedFd poll_fd(srpc::epoll_open());
  int sockets[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  const ScopedFd observed(sockets[0]);
  const ScopedFd peer(sockets[1]);

  ASSERT_EQ(srpc::epoll_add_impl(
                poll_fd.get(), observed.get(), srpc::PollMode::READ),
            0);
  const char marker = 'x';
  ASSERT_EQ(write(peer.get(), &marker, sizeof(marker)),
            static_cast<ssize_t>(sizeof(marker)));

  epoll_event event{};
  ASSERT_EQ(epoll_wait(poll_fd.get(), &event, 1, 1000), 1);
  ExpectEventData(event, observed.get());
  EXPECT_NE(event.events & EPOLLIN, 0U);

  char received = 0;
  ASSERT_EQ(read(observed.get(), &received, sizeof(received)),
            static_cast<ssize_t>(sizeof(received)));
  EXPECT_EQ(received, marker);

  ASSERT_EQ(srpc::epoll_update_impl(poll_fd.get(), observed.get(),
                                    srpc::PollMode::WRITE,
                                    srpc::PollMode::READ),
            0);
  ASSERT_EQ(epoll_wait(poll_fd.get(), &event, 1, 1000), 1);
  ExpectEventData(event, observed.get());
  EXPECT_NE(event.events & EPOLLOUT, 0U);

  EXPECT_EQ(srpc::epoll_remove_impl(poll_fd.get(), observed.get()), 0);
}

}  // namespace
