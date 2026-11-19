// Unit tests for the TCP channel backend's connection-side data path.
//
// Two fixtures, because a connection has two lives:
//
// * TcpConnectionTest builds a `TcpConnection` over one end of a
//   `socketpair(2)` with no PollThread, and reads/writes the other end as the
//   "peer".  Without a PollThread there is no transport task and no
//   write-through, so a sent frame stays queued until `flush()`; that makes
//   the encoder, the coalescing of queued frames, close, backpressure and the
//   proxy forwarding deterministic to check.
// * AttachedTcpConnectionTest connects through the public `TcpFactory` to a
//   plain loopback socket, so the generated reader and writer tasks run on a
//   real PollThread (S5 of docs/dev/lion-runtime-plan.md).  Inbound framing
//   (whole, fragmented and coalesced frames), peer hang-up, close and foreign
//   sends are checked through them.
//
// Until S7b these tests drove the connection's `Pollable` methods
// (handle_read / handle_write / poll_mode / content_size and the pending-write
// latch) directly; that surface left with the retired epoll loop.  The
// contracts that only it had -- handle_read returning false and handle_write
// returning NO_CHANGE after close, poll_mode tracking queued bytes -- have no
// counterpart on Lion; task retirement after close is covered by
// tests/tcp_transport_rust.rs (a_close_on_the_poll_thread_retires_both_tasks,
// a_foreign_close_of_an_idle_connection_retires_both_tasks) and by
// CloseRetiresTheTransportAndThePeerSeesEof below.
//
// Notes on socketpair vs real TCP:
//   - SOCK_STREAM with AF_UNIX gives us byte-stream semantics matching
//     what TCP delivers, with no listen/accept/bind ceremony. The
//     wire-format guarantees we care about live in `frame_codec`, not
//     in any TCP-specific framing, so this is a faithful substrate.

#include <errno.h>

#include <cstddef>
#include <gtest/gtest.h>


#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rusty/arc.hpp>
#include <rusty/rusty.hpp>

#include "../srpc.hpp"

import std;

namespace srpc {
namespace {

// TcpConnection and TcpListener are consumed through Arc/proxy APIs, but the
// concrete module types predate the canonical-Rust migration and are part of
// the shipped C++ ABI.  Keep every historical boundary pinned: using fresh
// mutex wrappers for fd/listener/inbound state shifts the callback fields and
// breaks already-compiled consumers even when method symbols are unchanged.
// Re-pinned after fd_ became UnsafeCell<Option<Arc<LegacyOwnedFd>>> -- the
// descriptor lease that lets a close race an epoll operation safely.  A
// rusty::Option<Arc<..>> is 16 bytes where the bare OwnedFd was 4 (+4 pad), so
// every field after fd_ moves +8 and sizeof goes 344 -> 352.  These values are
// measured from the generated module (undefined-template probe), not derived.
// Re-pinned for the Lion transport (docs/dev/lion-runtime-plan.md S5): 352 ->
// 400.  21ce10a appended writer_ (UnsafeCell<Option<Waker>>, the writer task's
// waker, 32 bytes with rusty-cpp a130025e's heap-held Waker callable), and
// daf3d92/4c008ed appended send_error_ and last_send_us_ for write-through and
// the cork.  The fields before writer_ do not move.
// S7b removed pending_write_update_, the retired pending-write latch: a bool
// at 162, inside the padding before poll_thread_, so nothing else moves and
// sizeof stays 400 (measured, like the rest).
static_assert(sizeof(TcpConnection) == 400);
static_assert(alignof(TcpConnection) == 8);
static_assert(rusty::is_send<TcpConnection>::value);
static_assert(rusty::is_sync<TcpConnection>::value);
static_assert(offsetof(TcpConnection, fd_) == 0);
static_assert(offsetof(TcpConnection, peer_address_) == 16);
static_assert(offsetof(TcpConnection, outbound_high_water_) == 40);
static_assert(offsetof(TcpConnection, outbound_) == 48);
static_assert(offsetof(TcpConnection, inbound_) == 112);
static_assert(offsetof(TcpConnection, closed_) == 160);
static_assert(offsetof(TcpConnection, on_closed_fired_) == 161);
static_assert(offsetof(TcpConnection, poll_thread_) == 168);
static_assert(offsetof(TcpConnection, on_frame_) == 184);
static_assert(offsetof(TcpConnection, on_closed_) == 240);
static_assert(offsetof(TcpConnection, on_error_) == 296);
static_assert(offsetof(TcpConnection, writer_) == 352);
static_assert(offsetof(TcpConnection, send_error_) == 384);
static_assert(offsetof(TcpConnection, last_send_us_) == 392);

// Re-pinned with TcpConnection above: listener_ became
// RefCell<Option<Arc<LegacyTcpListener>>> (the listener's descriptor lease),
// which is a borrow flag plus a 16-byte Option<Arc<..>> where the old handle
// was 8 bytes total, so every later field moves +16 and sizeof goes 192 -> 208.
// Measured from the generated module, not derived.
static_assert(sizeof(TcpListener) == 208);
static_assert(alignof(TcpListener) == 8);
static_assert(rusty::is_send<TcpListener>::value);
static_assert(rusty::is_sync<TcpListener>::value);
static_assert(offsetof(TcpListener, listener_) == 0);
static_assert(offsetof(TcpListener, bound_address_) == 24);
static_assert(offsetof(TcpListener, closed_) == 56);
static_assert(offsetof(TcpListener, listened_) == 57);
static_assert(offsetof(TcpListener, accept_callback_thread_) == 60);
static_assert(offsetof(TcpListener, poll_thread_) == 64);
static_assert(offsetof(TcpListener, self_weak_) == 80);
static_assert(offsetof(TcpListener, on_accept_) == 96);
static_assert(offsetof(TcpListener, on_error_) == 152);

static_assert(sizeof(TcpFactory) == 16);
static_assert(alignof(TcpFactory) == 8);
static_assert(offsetof(TcpFactory, poll_thread_) == 0);
static_assert(offsetof(TcpFactory, connect_timeout_ms_) == 8);

int connect_to_listener(const rusty::String& listener_address) {
    // The listener reports its address as a rusty::String; parse it as std::string.
    const std::string address(listener_address.c_str());
    const auto colon = address.rfind(':');
    if (colon == std::string::npos) return -1;
    const int port = std::atoi(address.c_str() + colon + 1);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<std::uint16_t>(port));
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

class TcpConnectionTest : public ::testing::Test {
 protected:
    void SetUp() override {
        int sv[2];
        ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
        ASSERT_EQ(0, set_nonblocking(sv[0]));
        ASSERT_EQ(0, set_nonblocking(sv[1]));
        conn_fd_ = sv[0];
        peer_fd_ = sv[1];
        conn_ = rusty::Some(rusty::Arc<TcpConnection>::new_(TcpConnection::new_(conn_fd_, "test-peer")));
    }

    void TearDown() override {
        if (peer_fd_ >= 0) {
            ::close(peer_fd_);
            peer_fd_ = -1;
        }
        // The connection's destructor closes conn_fd_ if not already
        // closed. Dropping the optional drops the Arc.
        conn_ = rusty::None;
    }

    // `rusty::Arc<T>` exposes only `const T*` via `operator->`, but
    // the channel layer's mutator methods (`send_frame`, `close`, etc.)
    // are non-const. Mirror the `mut_conn()` idiom used by the proxy
    // adapters to get mutable access in test bodies.
    TcpConnection& mut_conn() {
        return const_cast<TcpConnection&>(*conn_.as_ref().unwrap().get());
    }
    const TcpConnection& conn() const {
        return *conn_.as_ref().unwrap().get();
    }

    static int set_nonblocking(int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return errno;
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errno;
        return 0;
    }

    // Write `bytes` to `peer_fd_`. Returns the number of bytes written
    // or -1 on error. May write less than requested if the pipe buffer
    // fills.
    ssize_t peer_write(const std::uint8_t* data, std::size_t size) {
        return ::write(peer_fd_, data, size);
    }

    // Read whatever is available on `peer_fd_` into `out`.
    ssize_t peer_read(std::vector<std::uint8_t>& out, std::size_t max = 4096) {
        out.resize(max);
        ssize_t n = ::read(peer_fd_, out.data(), max);
        if (n < 0) {
            out.clear();
            return n;
        }
        out.resize(static_cast<std::size_t>(n));
        return n;
    }

    int conn_fd_ = -1;
    int peer_fd_ = -1;
    rusty::Option<rusty::Arc<TcpConnection>> conn_;
};

// ---------------------------------------------------------------------------
// Channel-facade basic dispatch
// ---------------------------------------------------------------------------

TEST_F(TcpConnectionTest, PeerAddressIsPropagated) {
    EXPECT_EQ(conn().peer_address(), "test-peer");
}

TEST_F(TcpConnectionTest, IsClosedStartsFalse) {
    EXPECT_FALSE(conn().is_closed());
}

TEST_F(TcpConnectionTest, FdIsExposed) {
    EXPECT_EQ(conn().fd(), conn_fd_);
}

// ---------------------------------------------------------------------------
// Send path without a PollThread: bytes match frame_codec's wire format
// ---------------------------------------------------------------------------

// No PollThread means no writer task and no write-through: the frame stays
// queued (the peer sees nothing) until flush() drains it.
TEST_F(TcpConnectionTest, SendFrameIsQueuedUntilFlush) {
    const std::uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
    ChannelFrame f{payload, sizeof(payload)};

    EXPECT_EQ(mut_conn().send_frame(f), ChannelError::None);
    std::vector<std::uint8_t> got;
    EXPECT_EQ(peer_read(got), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    mut_conn().flush();

    // Peer should observe header (4 bytes) + payload (4 bytes).
    ssize_t n = peer_read(got);
    ASSERT_GT(n, 0);
    ASSERT_EQ(got.size(), 4u + sizeof(payload));

    FrameHeader hdr{};
    EXPECT_EQ(frame_codec_peek_header(got, hdr),
              FrameDecodeStatus::Complete);
    EXPECT_EQ(hdr.payload_size, static_cast<std::int32_t>(sizeof(payload)));
    EXPECT_FALSE(hdr.extended_header_flag);
    EXPECT_EQ(0, std::memcmp(got.data() + 4, payload, sizeof(payload)));
}

TEST_F(TcpConnectionTest, SendZeroLengthPayload) {
    ChannelFrame f{nullptr, 0};
    EXPECT_EQ(mut_conn().send_frame(f), ChannelError::None);
    mut_conn().flush();

    std::vector<std::uint8_t> got;
    ASSERT_EQ(peer_read(got), 4);  // Just the size header.
    FrameHeader hdr{};
    EXPECT_EQ(frame_codec_peek_header(std::span<const std::uint8_t>(got).first(4), hdr),
              FrameDecodeStatus::Complete);
    EXPECT_EQ(hdr.payload_size, 0);
}

TEST_F(TcpConnectionTest, MultipleSendFramesCoalesceIntoOneWrite) {
    const std::uint8_t a[] = {0x11, 0x22};
    const std::uint8_t b[] = {0x33, 0x44, 0x55};
    EXPECT_EQ(mut_conn().send_frame({a, sizeof(a)}), ChannelError::None);
    EXPECT_EQ(mut_conn().send_frame({b, sizeof(b)}), ChannelError::None);

    mut_conn().flush();

    std::vector<std::uint8_t> got;
    ssize_t n = peer_read(got);
    ASSERT_EQ(n, static_cast<ssize_t>(4 + sizeof(a) + 4 + sizeof(b)));

    // Decode both frames out of the coalesced buffer.
    auto reader = FrameStreamReader::new_();
    reader.append(got.data(), got.size());
    FrameView v{};
    ASSERT_EQ(reader.next_frame(v), FrameDecodeStatus::Complete);
    EXPECT_EQ(v.payload_size, sizeof(a));
    reader.consume_frame();
    ASSERT_EQ(reader.next_frame(v), FrameDecodeStatus::Complete);
    EXPECT_EQ(v.payload_size, sizeof(b));
    reader.consume_frame();
    EXPECT_EQ(reader.next_frame(v), FrameDecodeStatus::NeedMoreBytes);
}

// ---------------------------------------------------------------------------
// Close semantics
// ---------------------------------------------------------------------------

TEST_F(TcpConnectionTest, CloseIsIdempotent) {
    int closes_seen = 0;
    mut_conn().set_on_closed(OnClosedCallback::from_callable(
        [&](ChannelError) { ++closes_seen; }));

    EXPECT_FALSE(conn().is_closed());
    mut_conn().close();
    EXPECT_TRUE(conn().is_closed());
    mut_conn().close();
    mut_conn().close();
    EXPECT_EQ(closes_seen, 1);
}

TEST_F(TcpConnectionTest, FlushFailureStillDeliversOneReentrantCloseCallback) {
    int closes_seen = 0;
    mut_conn().set_on_closed(OnClosedCallback::from_callable([&](ChannelError reason) {
        EXPECT_EQ(reason, ChannelError::None);
        ++closes_seen;
        EXPECT_EQ(conn().fd(), -1);
        mut_conn().close();
        mut_conn().set_on_closed(OnClosedCallback{});
    }));
    const std::uint8_t bytes[] = {0x11, 0x22, 0x33};
    ASSERT_EQ(mut_conn().send_frame(ChannelFrame{bytes, sizeof(bytes)}), ChannelError::None);
    ASSERT_EQ(::close(peer_fd_), 0);
    peer_fd_ = -1;
    mut_conn().flush();
    ASSERT_TRUE(conn().is_closed());
    EXPECT_EQ(closes_seen, 0);
    mut_conn().close();
    mut_conn().close();
    EXPECT_EQ(conn().fd(), -1);
    EXPECT_EQ(closes_seen, 1);
}

TEST_F(TcpConnectionTest, SendAfterCloseReturnsConnectionReset) {
    mut_conn().close();
    EXPECT_TRUE(conn().is_closed());

    const std::uint8_t b[1] = {0xAA};
    EXPECT_EQ(mut_conn().send_frame({b, 1}), ChannelError::ConnectionReset);
}

// ---------------------------------------------------------------------------
// Backpressure
// ---------------------------------------------------------------------------

TEST_F(TcpConnectionTest, OutboundHighWaterReturnsWouldBlock) {
    mut_conn().set_outbound_high_water(64);

    // Drain the kernel side first so writes definitely buffer in our
    // outbound queue rather than passing straight through.
    // Use a 32-byte payload + 4-byte header = 36 bytes per frame.
    const std::uint8_t pad[32]{};

    // First send fills the queue past the high water but is still
    // accepted (we reject only if the queue is *already* over
    // budget at entry).
    EXPECT_EQ(mut_conn().send_frame({pad, sizeof(pad)}), ChannelError::None);
    EXPECT_EQ(mut_conn().send_frame({pad, sizeof(pad)}), ChannelError::None);

    // Now the queue is at 72 bytes which is past the 64-byte budget;
    // the next send should be rejected without buffering.
    EXPECT_EQ(mut_conn().send_frame({pad, sizeof(pad)}),
              ChannelError::WouldBlock);
}

// ---------------------------------------------------------------------------
// Channel-facade proxy dispatch
// ---------------------------------------------------------------------------

TEST_F(TcpConnectionTest, ChannelProxyForwardsAllOps) {
    auto proxy = make_tcp_connection_channel_proxy(conn_.as_ref().unwrap().clone());

    int frames_seen = 0;
    proxy->set_on_frame(OnFrameCallback::from_callable(
        [&](const ChannelFrame&) { ++frames_seen; }));

    EXPECT_EQ(proxy->peer_address(), "test-peer");
    EXPECT_FALSE(proxy->is_closed());

    const std::uint8_t b[2] = {0xA1, 0xA2};
    EXPECT_EQ(proxy->send_frame({b, 2}), ChannelError::None);
    proxy->flush();

    proxy->close();
    EXPECT_TRUE(proxy->is_closed());

    // Frame delivery exercised separately (proxy paths are pure
    // forwarding to the same Arc).
}

// ---------------------------------------------------------------------------
// Attached to a PollThread: the generated reader and writer tasks
// ---------------------------------------------------------------------------

// Callbacks run on the poll thread; the test thread reads what they record.
struct Observed {
    std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> frames;
    std::atomic<int> frame_count{0};
    std::atomic<int> errors{0};
    std::atomic<int> closes{0};
    std::atomic<int> last_close{static_cast<int>(ChannelError::Internal)};
};

class AttachedTcpConnectionTest : public ::testing::Test {
 protected:
    void SetUp() override {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(listen_fd_, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address),
                         sizeof(address)), 0);
        ASSERT_EQ(::listen(listen_fd_, 1), 0);
        socklen_t length = sizeof(address);
        ASSERT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                                &length), 0);
        const std::string endpoint =
            "127.0.0.1:" + std::to_string(ntohs(address.sin_port));

        poll_thread_ = rusty::Some(PollThread::create());
        auto factory = TcpFactory::new_(poll_thread_.as_ref().unwrap().clone());
        auto connected = factory.connect(endpoint);
        ASSERT_EQ(connected.error, ChannelError::None);
        ASSERT_TRUE(connected.connection.is_some());
        proxy_ = std::move(connected.connection);

        peer_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        ASSERT_GE(peer_fd_, 0);
        // One segment per peer write, so fragmented input stays fragmented.
        const int one = 1;
        ASSERT_EQ(::setsockopt(peer_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), 0);
        // Bounds every blocking peer read.
        const timeval limit{5, 0};
        ASSERT_EQ(::setsockopt(peer_fd_, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)), 0);
    }

    void TearDown() override {
        if (proxy_.is_some()) {
            proxy()->close();
        }
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
        }
        close_peer();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    // The proxy's setters take `&mut self`; mirror mut_conn() above.
    ChannelConnectionProxy& proxy() {
        return const_cast<ChannelConnectionProxy&>(proxy_.as_ref().unwrap());
    }

    // Callbacks capture the fixture's `seen_`, which outlives TearDown: its
    // close() delivers on_closed, after the test body's locals are gone.
    void observe() {
        Observed& seen = seen_;
        proxy()->set_on_frame(OnFrameCallback::from_callable([&seen](const ChannelFrame& f) {
            {
                std::lock_guard<std::mutex> guard(seen.mutex);
                seen.frames.emplace_back(f.payload, f.payload + f.size);
            }
            seen.frame_count.fetch_add(1, std::memory_order_release);
        }));
        proxy()->set_on_error(OnErrorCallback::from_callable(
            [&seen](ChannelError, std::string_view) {
                seen.errors.fetch_add(1, std::memory_order_release);
            }));
        proxy()->set_on_closed(OnClosedCallback::from_callable([&seen](ChannelError reason) {
            seen.last_close.store(static_cast<int>(reason), std::memory_order_relaxed);
            seen.closes.fetch_add(1, std::memory_order_release);
        }));
    }

    void peer_write_all(const std::uint8_t* data, std::size_t size) {
        while (size > 0) {
            const ssize_t n = ::write(peer_fd_, data, size);
            ASSERT_GT(n, 0);
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }

    // Read exactly `size` bytes; fewer means EOF, an error or the timeout.
    std::vector<std::uint8_t> peer_read_exactly(std::size_t size) {
        std::vector<std::uint8_t> out(size);
        std::size_t got = 0;
        while (got < size) {
            const ssize_t n = ::read(peer_fd_, out.data() + got, size - got);
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
        }
        out.resize(got);
        return out;
    }

    void close_peer() {
        if (peer_fd_ >= 0) {
            ::close(peer_fd_);
            peer_fd_ = -1;
        }
    }

    // Declared first, so destroyed last.
    Observed seen_;
    int listen_fd_ = -1;
    int peer_fd_ = -1;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    rusty::Option<ChannelConnectionProxy> proxy_;
};

constexpr auto kDeliveryLimit = std::chrono::seconds(5);

TEST_F(AttachedTcpConnectionTest, PeerFrameIsDeliveredByTheReaderTask) {
    Observed& seen = seen_;
    observe();

    std::vector<std::uint8_t> wire;
    const std::uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    ASSERT_TRUE(frame_codec_encode_into(wire, payload, sizeof(payload), false));
    peer_write_all(wire.data(), wire.size());

    ASSERT_TRUE(wait_until([&] { return seen.frame_count.load() == 1; }, kDeliveryLimit));
    std::lock_guard<std::mutex> guard(seen.mutex);
    ASSERT_EQ(seen.frames.size(), 1u);
    ASSERT_EQ(seen.frames[0].size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(seen.frames[0].data(), payload, sizeof(payload)));
}

TEST_F(AttachedTcpConnectionTest, FragmentedInboundReassembled) {
    Observed& seen = seen_;
    observe();

    std::vector<std::uint8_t> wire;
    const std::uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(frame_codec_encode_into(wire, payload, sizeof(payload), false));

    // Feed bytes one at a time, each its own segment and read edge; on_frame
    // must not fire until the last byte arrives.
    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        peer_write_all(wire.data() + i, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_EQ(seen.frame_count.load(), 0)
            << "frame fired prematurely at byte " << (i + 1);
    }
    peer_write_all(wire.data() + wire.size() - 1, 1);
    ASSERT_TRUE(wait_until([&] { return seen.frame_count.load() == 1; }, kDeliveryLimit));
    std::lock_guard<std::mutex> guard(seen.mutex);
    ASSERT_EQ(seen.frames.size(), 1u);
    EXPECT_EQ(0, std::memcmp(seen.frames[0].data(), payload, sizeof(payload)));
}

TEST_F(AttachedTcpConnectionTest, MultiFrameCoalescedReadDeliversAll) {
    Observed& seen = seen_;
    observe();

    std::vector<std::uint8_t> wire;
    const std::uint8_t a[] = {0xAA};
    const std::uint8_t b[] = {0xBB, 0xCC};
    const std::uint8_t c[] = {0xDD, 0xEE, 0xFF};
    ASSERT_TRUE(frame_codec_encode_into(wire, a, 1, false));
    ASSERT_TRUE(frame_codec_encode_into(wire, b, 2, false));
    ASSERT_TRUE(frame_codec_encode_into(wire, c, 3, false));
    peer_write_all(wire.data(), wire.size());

    ASSERT_TRUE(wait_until([&] { return seen.frame_count.load() == 3; }, kDeliveryLimit));
    std::lock_guard<std::mutex> guard(seen.mutex);
    ASSERT_EQ(seen.frames.size(), 3u);
    EXPECT_EQ(seen.frames[0].size(), 1u);
    EXPECT_EQ(seen.frames[1].size(), 2u);
    EXPECT_EQ(seen.frames[2].size(), 3u);
    EXPECT_EQ(seen.frames[2][2], 0xFF);
}

// A peer hang-up after a partial frame is a clean close, not an error.  (A
// malformed header cannot be produced on this wire -- the 31-bit size field
// is unsigned -- and the codec's Malformed branch is covered by
// rpc_frame_codec_test.cc.)
TEST_F(AttachedTcpConnectionTest, PartialFrameThenPeerHangupIsACleanClose) {
    Observed& seen = seen_;
    observe();

    const std::uint8_t partial_header[2] = {0x10, 0x00};
    peer_write_all(partial_header, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(seen.errors.load(), 0);
    EXPECT_EQ(seen.closes.load(), 0);

    ::shutdown(peer_fd_, SHUT_WR);
    close_peer();

    ASSERT_TRUE(wait_until([&] { return seen.closes.load() == 1; }, kDeliveryLimit));
    EXPECT_EQ(seen.errors.load(), 0);
    EXPECT_EQ(seen.last_close.load(), static_cast<int>(ChannelError::None));
    EXPECT_EQ(seen.frame_count.load(), 0);
    EXPECT_TRUE(proxy()->is_closed());
}

TEST_F(AttachedTcpConnectionTest, PeerHangupFiresOnClosedExactlyOnce) {
    Observed& seen = seen_;
    observe();

    ::shutdown(peer_fd_, SHUT_WR);
    close_peer();

    ASSERT_TRUE(wait_until([&] { return seen.closes.load() >= 1; }, kDeliveryLimit));
    // Both tasks see the hang-up; the connection reports it once.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(seen.closes.load(), 1);
    EXPECT_TRUE(proxy()->is_closed());
}

// Sends from the test thread are foreign to the PollThread: they write
// through or hand the bytes to the writer task.  Either way the peer sees
// every frame, in order, in frame_codec's format.
TEST_F(AttachedTcpConnectionTest, ForeignSendsReachThePeerInWireOrder) {
    const std::uint8_t a[] = {0xCA, 0xFE};
    const std::uint8_t b[] = {0xBA, 0xBE, 0x01};
    EXPECT_EQ(proxy()->send_frame({a, sizeof(a)}), ChannelError::None);
    EXPECT_EQ(proxy()->send_frame({nullptr, 0}), ChannelError::None);
    EXPECT_EQ(proxy()->send_frame({b, sizeof(b)}), ChannelError::None);

    const std::size_t total = 4 + sizeof(a) + 4 + 4 + sizeof(b);
    const auto got = peer_read_exactly(total);
    ASSERT_EQ(got.size(), total);

    auto reader = FrameStreamReader::new_();
    reader.append(got.data(), got.size());
    FrameView v{};
    ASSERT_EQ(reader.next_frame(v), FrameDecodeStatus::Complete);
    ASSERT_EQ(v.payload_size, sizeof(a));
    EXPECT_EQ(0, std::memcmp(v.payload, a, sizeof(a)));
    reader.consume_frame();
    ASSERT_EQ(reader.next_frame(v), FrameDecodeStatus::Complete);
    EXPECT_EQ(v.payload_size, 0u);
    reader.consume_frame();
    ASSERT_EQ(reader.next_frame(v), FrameDecodeStatus::Complete);
    ASSERT_EQ(v.payload_size, sizeof(b));
    EXPECT_EQ(0, std::memcmp(v.payload, b, sizeof(b)));
    reader.consume_frame();
    EXPECT_EQ(reader.next_frame(v), FrameDecodeStatus::NeedMoreBytes);
}

// close() from a foreign thread shuts the socket down, the transport tasks
// retire and close the connection (on_closed fires once), and the peer reads
// EOF.
TEST_F(AttachedTcpConnectionTest, CloseRetiresTheTransportAndThePeerSeesEof) {
    Observed& seen = seen_;
    observe();

    proxy()->close();
    EXPECT_TRUE(proxy()->is_closed());

    std::uint8_t byte = 0;
    EXPECT_EQ(::read(peer_fd_, &byte, 1), 0);
    ASSERT_TRUE(wait_until([&] { return seen.closes.load() >= 1; }, kDeliveryLimit));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(seen.closes.load(), 1);
    EXPECT_EQ(seen.errors.load(), 0);

    const std::uint8_t b[1] = {0xAA};
    EXPECT_EQ(proxy()->send_frame({b, 1}), ChannelError::ConnectionReset);
}

// ---------------------------------------------------------------------------
// Listener close and the accept driver
// ---------------------------------------------------------------------------

// The accept task runs the accept driver on the PollThread; a close from any
// other thread waits until the whole driver (not only the callback) is done.
// Ported from the two-reader version that drove TcpListener::handle_read on
// two threads; on Lion there is one accept task per listener, and the
// Rust lane's twin is tests/tcp_channel_rust.rs
// close_waits_for_the_whole_accept_driver.
TEST(TcpListenerConcurrencyTest, CloseWaitsForTheWholeAcceptDriver) {
    auto poll_thread = PollThread::create();
    auto raw = TcpListener::new_();
    raw.set_poll_thread(poll_thread.clone());
    auto listener = rusty::Arc<TcpListener>::new_(std::move(raw));

    std::atomic<unsigned> callbacks_entered{0};
    std::atomic<bool> release_first{false};
    listener->set_on_accept(OnAcceptCallback::from_callable(
        [&](ChannelConnectionProxy) {
            const unsigned index =
                callbacks_entered.fetch_add(1, std::memory_order_seq_cst);
            if (index == 0) {
                while (!release_first.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        }));
    auto proxy = make_tcp_listener_channel_proxy(listener.clone());
    ASSERT_EQ(proxy->listen("127.0.0.1:0"), ChannelError::None);

    const int client1 = connect_to_listener(listener->local_address());
    ASSERT_GE(client1, 0);
    const bool first_entered = wait_until(
        [&] { return callbacks_entered.load(std::memory_order_acquire) >= 1; },
        std::chrono::seconds(5));
    if (!first_entered) {
        ::close(client1);
        poll_thread->shutdown();
        FAIL() << "first accept callback did not start";
    }
    // A second connection queues behind the blocked driver.
    const int client2 = connect_to_listener(listener->local_address());

    std::atomic<unsigned> closes_done{0};
    std::thread closer1([&] {
        listener->close();
        closes_done.fetch_add(1, std::memory_order_release);
    });
    const bool close_started = wait_until([&] { return listener->is_closed(); },
                                          std::chrono::seconds(5));
    // A second close exercises the already-closed path; it must wait too.
    std::thread closer2([&] {
        listener->close();
        closes_done.fetch_add(1, std::memory_order_release);
    });
    const bool a_close_returned_while_first_callback_was_live = wait_until(
        [&] { return closes_done.load(std::memory_order_acquire) != 0; },
        std::chrono::milliseconds(200));

    release_first.store(true, std::memory_order_release);
    closer1.join();
    closer2.join();
    // The driver saw close and accepted nothing more.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ::close(client1);
    if (client2 >= 0) ::close(client2);

    EXPECT_TRUE(close_started);
    EXPECT_GE(client2, 0);
    EXPECT_EQ(callbacks_entered.load(std::memory_order_seq_cst), 1u);
    EXPECT_FALSE(a_close_returned_while_first_callback_was_live);
    EXPECT_EQ(closes_done.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(listener->fd(), -1);
    poll_thread->shutdown();
}

}  // namespace
}  // namespace srpc
