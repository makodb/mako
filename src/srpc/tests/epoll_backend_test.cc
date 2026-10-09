// The Lion OS backend (reactor/epoll_wrapper.rs, SrpcEpollBackend) through
// the generated C++ provider: registration, token round trip, edge-triggered
// waits, the error-only pipe event, the reserved interrupt token, the
// cross-thread interrupt, and the timeout rule. The Rust suite
// tests/epoll_backend_rust.rs holds the full mio-parity tables; this suite
// proves the generated module behaves the same on the same kernel.
#include <chrono>
#include <cstdint>
#include <thread>
#include <tuple>
#include <type_traits>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include "../srpc.hpp"

import std;
import rusty;
import srpc.epoll_wrapper;

namespace {

using srpc::SrpcEpollBackend;
using srpc::SrpcInterest;
using srpc::SrpcOsEvent;

// rusty::time::Duration is emitted into each generated module rather than a
// shared header, so name it through the signature of an exported function.
template <class Return, class Argument>
Argument first_argument(Return (*)(Argument));
using TimeoutOption = decltype(first_argument(&srpc::epoll_timeout_ms));
using Duration = typename TimeoutOption::value_type;
using Events = rusty::Vec<SrpcOsEvent>;

constexpr SrpcInterest kRead{true, false};
constexpr SrpcInterest kWrite{false, true};
constexpr SrpcInterest kReadWrite{true, true};

struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() {
        if (value >= 0) ::close(value);
    }
    void reset() {
        if (value >= 0) ::close(value);
        value = -1;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

struct SocketPair {
    Fd a{-1};
    Fd b{-1};
    SocketPair() {
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
        a.value = fds[0];
        b.value = fds[1];
    }
};

SrpcEpollBackend make_backend() {
    auto created = SrpcEpollBackend::new_();
    if (!created.is_ok()) throw std::runtime_error("SrpcEpollBackend::new_ failed");
    return created.unwrap();
}

Events wait_ms(SrpcEpollBackend& backend, int32_t timeout_ms) {
    Events events;
    auto result = backend.wait_timeout_ms(events, timeout_ms);
    EXPECT_TRUE(result.is_ok());
    return events;
}

TEST(EpollBackend, TokenRoundTripsAndWaitsAreEdgeTriggered) {
    auto backend = make_backend();
    SocketPair pair;
    constexpr std::size_t kToken = 0x1234'5678'9abcULL;
    ASSERT_TRUE(backend.register_(pair.a.value, kToken, kRead).is_ok());
    EXPECT_EQ(wait_ms(backend, 0).len(), 0u);

    ASSERT_EQ(::write(pair.b.value, "x", 1), 1);
    auto events = wait_ms(backend, 5000);
    ASSERT_EQ(events.len(), 1u);
    EXPECT_EQ(events[0].token, kToken);
    EXPECT_TRUE(events[0].readable);
    EXPECT_FALSE(events[0].writable);
    EXPECT_FALSE(events[0].error);
    EXPECT_FALSE(events[0].read_closed);
    EXPECT_FALSE(events[0].write_closed);

    // The byte stays unread; an edge-triggered backend reports nothing new.
    EXPECT_EQ(wait_ms(backend, 50).len(), 0u);
    ASSERT_EQ(::write(pair.b.value, "y", 1), 1);
    EXPECT_EQ(wait_ms(backend, 5000).len(), 1u);

    // Peer half-close: IN|RDHUP, read_closed without write_closed.
    ASSERT_EQ(::shutdown(pair.b.value, SHUT_WR), 0);
    events = wait_ms(backend, 5000);
    ASSERT_EQ(events.len(), 1u);
    EXPECT_TRUE(events[0].readable);
    EXPECT_TRUE(events[0].read_closed);
    EXPECT_FALSE(events[0].write_closed);
}

TEST(EpollBackend, DurationWaitAndReregister) {
    auto backend = make_backend();
    SocketPair pair;
    ASSERT_TRUE(backend.register_(pair.a.value, 21, kRead).is_ok());
    ASSERT_TRUE(backend.reregister(pair.a.value, 22, kReadWrite).is_ok());
    Events events;
    ASSERT_TRUE(backend.wait(events, TimeoutOption(Duration::from_millis(5000))).is_ok());
    ASSERT_EQ(events.len(), 1u);
    EXPECT_EQ(events[0].token, 22u);
    EXPECT_TRUE(events[0].writable);
}

TEST(EpollBackend, PipeReaderGoneWhileFullIsErrorOnly) {
    auto backend = make_backend();
    int fds[2];
    ASSERT_EQ(::pipe2(fds, O_NONBLOCK | O_CLOEXEC), 0);
    Fd reader(fds[0]);
    Fd writer(fds[1]);
    char chunk[65536] = {};
    while (::write(writer.value, chunk, sizeof(chunk)) > 0) {
    }
    ASSERT_EQ(errno, EAGAIN);
    ASSERT_TRUE(backend.register_(writer.value, 12, kWrite).is_ok());
    EXPECT_EQ(wait_ms(backend, 0).len(), 0u);
    reader.reset();
    auto events = wait_ms(backend, 5000);
    ASSERT_EQ(events.len(), 1u);
    EXPECT_EQ(events[0].token, 12u);
    EXPECT_FALSE(events[0].readable);
    EXPECT_FALSE(events[0].writable);
    EXPECT_TRUE(events[0].error);
    EXPECT_FALSE(events[0].read_closed);
    EXPECT_TRUE(events[0].write_closed);
}

TEST(EpollBackend, TokenZeroIsReservedAndErrorsCarryErrno) {
    auto backend = make_backend();
    SocketPair pair;
    auto reserved = backend.register_(pair.a.value, 0, kRead);
    ASSERT_TRUE(reserved.is_err());
    EXPECT_EQ(reserved.unwrap_err().raw_os_error().unwrap(), 22);
    ASSERT_TRUE(backend.register_(pair.a.value, 5, kRead).is_ok());
    auto twice = backend.register_(pair.a.value, 6, kRead);
    ASSERT_TRUE(twice.is_err());
    EXPECT_EQ(twice.unwrap_err().raw_os_error().unwrap(), 17);
    ASSERT_TRUE(backend.deregister(pair.a.value).is_ok());
    ASSERT_EQ(::write(pair.b.value, "x", 1), 1);
    EXPECT_EQ(wait_ms(backend, 50).len(), 0u);
    auto again = backend.deregister(pair.a.value);
    ASSERT_TRUE(again.is_err());
    EXPECT_EQ(again.unwrap_err().raw_os_error().unwrap(), 2);
}

TEST(EpollBackend, CrossThreadInterruptWakesAndIsNotReported) {
    auto backend = make_backend();
    auto interrupt = backend.interrupt();

    // A signal made before the wait ends it; it is consumed, not reported.
    ASSERT_TRUE(interrupt->signal().is_ok());
    ASSERT_TRUE(interrupt->signal().is_ok());
    Events events;
    ASSERT_TRUE(backend.wait(events, TimeoutOption()).is_ok());
    EXPECT_EQ(events.len(), 0u);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(wait_ms(backend, 100).len(), 0u);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(80));

    // A signal from another thread ends a blocked unbounded wait.
    std::thread signaller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_TRUE(interrupt->signal().is_ok());
    });
    start = std::chrono::steady_clock::now();
    Events woken;
    ASSERT_TRUE(backend.wait(woken, TimeoutOption()).is_ok());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    signaller.join();
    EXPECT_EQ(woken.len(), 0u);
    EXPECT_LT(elapsed, std::chrono::seconds(4));
}

TEST(EpollBackend, PureMappingsMatchMio) {
    EXPECT_EQ(srpc::epoll_timeout_ms(TimeoutOption()), -1);
    EXPECT_EQ(srpc::epoll_timeout_ms(TimeoutOption(Duration::from_millis(0))), 0);
    EXPECT_EQ(srpc::epoll_timeout_ms(TimeoutOption(Duration::from_micros(500))), 1);
    EXPECT_EQ(srpc::epoll_timeout_ms(TimeoutOption(Duration::from_micros(1500))), 2);
    EXPECT_EQ(srpc::epoll_timeout_ms(TimeoutOption(Duration::from_secs(4'000'000))), INT32_MAX);

    EXPECT_EQ(srpc::epoll_interest_flags(kRead), 0x8000'2001u);
    EXPECT_EQ(srpc::epoll_interest_flags(kReadWrite), 0x8000'2005u);
    EXPECT_EQ(srpc::epoll_interest_flags(SrpcInterest{false, false}), 0x8000'2001u);

    const SrpcOsEvent error_only = srpc::epoll_os_event(9, 0x008);
    EXPECT_TRUE(error_only.error && error_only.write_closed);
    EXPECT_FALSE(error_only.readable || error_only.writable || error_only.read_closed);
    const SrpcOsEvent out_error = srpc::epoll_os_event(9, 0x00c);
    EXPECT_TRUE(out_error.writable && out_error.error && out_error.write_closed);
    const SrpcOsEvent rdhup_alone = srpc::epoll_os_event(9, 0x2000);
    EXPECT_FALSE(rdhup_alone.readable || rdhup_alone.read_closed || rdhup_alone.error);
    const SrpcOsEvent priority = srpc::epoll_os_event(9, 0x002);
    EXPECT_TRUE(priority.readable);
}

}  // namespace
