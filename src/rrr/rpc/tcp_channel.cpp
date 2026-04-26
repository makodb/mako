module;

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <rusty/arc.hpp>

module rrr:impl.rpc.tcp_channel;

import rrr;

namespace rrr {

namespace {

// One-shot recv buffer used by `handle_read` to copy bytes off the
// socket before handing them to `FrameStreamReader`. The reader uses
// its own contiguous buffer; this is just a syscall-side scratchpad.
constexpr std::size_t kRecvScratchBytes = 64 * 1024;

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TcpConnection::TcpConnection(int fd, std::string peer_address)
    : fd_(fd),
      peer_address_(std::move(peer_address)) {}

TcpConnection::~TcpConnection() {
    if (!closed_.get()) {
        // Best-effort cleanup. We can't fire callbacks here — the user
        // already dropped their handle, so there's nothing to deliver
        // to. Just close the fd.
        if (fd_ >= 0) {
            // @unsafe — system call
            ::close(fd_);
            fd_ = -1;
        }
        closed_.set(true);
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TcpConnection::set_outbound_high_water(std::size_t bytes) {
    outbound_high_water_ = bytes;
}

// ---------------------------------------------------------------------------
// Channel-facade methods
// ---------------------------------------------------------------------------

ChannelError TcpConnection::send_frame(const ChannelFrame& frame) {
    if (closed_.get()) {
        return ChannelError::ConnectionReset;
    }
    if (frame.size > 0 && frame.payload == nullptr) {
        return ChannelError::Internal;
    }
    if (frame.size > static_cast<std::size_t>(kMaxFramePayloadSize)) {
        return ChannelError::Internal;
    }

    // Determine whether the caller asked for the response extended-
    // header flag by sniffing the frame size's high bit. The channel
    // layer treats `payload` as opaque; the encoded byte stream that
    // crosses the wire carries the flag in the size header. To stay
    // wire-compatible with the existing inline send paths
    // (`ServerConnection::reply` writes the extended-flag bit into the
    // size prefix, NOT into `payload`), we expose the extended flag
    // through a separate convention: `frame.size` in the channel's
    // `ChannelFrame` is the *raw payload byte count* (no flag), and
    // the codec is responsible for setting the flag when the RPC
    // layer constructs a response. For sub-leaf 3a we always emit
    // request-style frames (flag clear). The server-side flag handling
    // lands when the RPC layer is refactored onto channel.
    constexpr bool extended_header_flag = false;

    auto guard = outbound_.lock().unwrap();
    auto& buf = *guard;

    // Reject when the queue is already past the high water — we never
    // append to a buffer that's already over budget so backpressure is
    // strictly bounded.
    if (buf.size() >= outbound_high_water_) {
        return ChannelError::WouldBlock;
    }

    if (!frame_codec_encode_into(buf,
                                 frame.payload,
                                 static_cast<std::int32_t>(frame.size),
                                 extended_header_flag)) {
        return ChannelError::Internal;
    }

    pending_write_update_.set(true);
    return ChannelError::None;
}

void TcpConnection::flush() {
    if (closed_.get()) return;

    auto guard = outbound_.lock().unwrap();
    if (guard->empty()) return;

    // Best-effort: try to drain immediately. Errors here are reported
    // via the callbacks set on this connection; the caller of `flush`
    // does not get a return code (matching the channel facade).
    ChannelError result = drain_outbound_locked(*guard);
    if (result != ChannelError::None && result != ChannelError::WouldBlock) {
        // Defer error/close delivery to the poll thread to keep the
        // callback contract (poll-thread-only) honest. We just mark the
        // closed flag; the next `handle_read` / `handle_write` will fire
        // the callbacks. If `flush` is being called from the poll
        // thread itself, fire immediately.
        // For sub-leaf 3a we don't have an independent poll-thread
        // hand-off; tests drive Pollable methods directly. Mark closed
        // and let the next poll cycle observe it.
        closed_.set(true);
    }
}

void TcpConnection::close() {
    // Latch on first call. Idempotent for concurrent callers because
    // `Cell<bool>::set(true)` is a release-store; the first to set
    // wins, the rest observe `closed_.get() == true` here.
    if (closed_.get()) {
        return;
    }
    closed_.set(true);

    // Shutdown the write side to flush kernel buffers and signal the
    // peer; then close the fd. `::shutdown` may fail if the socket is
    // already half-closed — we ignore that.
    if (fd_ >= 0) {
        // @unsafe — system call
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }

    // Deliver `on_closed(ChannelError::None)` exactly once.
    deliver_on_closed_locked(ChannelError::None);
}

bool TcpConnection::is_closed() const {
    return closed_.get();
}

std::string TcpConnection::peer_address() const {
    return peer_address_;
}

void TcpConnection::set_on_frame(OnFrameCallback cb) {
    auto guard = on_frame_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpConnection::set_on_closed(OnClosedCallback cb) {
    auto guard = on_closed_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpConnection::set_on_error(OnErrorCallback cb) {
    auto guard = on_error_.lock().unwrap();
    *guard = std::move(cb);
}

// ---------------------------------------------------------------------------
// Pollable methods
// ---------------------------------------------------------------------------

int TcpConnection::fd() const {
    return fd_;
}

int TcpConnection::poll_mode() const {
    int mode = PollMode::READ;
    auto guard = outbound_.lock().unwrap();
    if (!guard->empty()) {
        mode |= PollMode::WRITE;
    }
    return mode;
}

std::size_t TcpConnection::content_size() {
    auto guard = outbound_.lock().unwrap();
    return guard->size() + inbound_.buffered_bytes();
}

bool TcpConnection::handle_read() {
    if (closed_.get()) return false;

    std::uint8_t scratch[kRecvScratchBytes];
    bool any_progress = false;

    while (true) {
        // @unsafe — system call
        ssize_t n = ::recv(fd_, scratch, sizeof(scratch), 0);
        if (n > 0) {
            inbound_.append(scratch, static_cast<std::size_t>(n));
            any_progress = true;
            // Drain the syscall in a loop so edge-triggered epoll users
            // don't lose readiness; cap at one iteration when the
            // syscall returns less than the scratch (level-triggered
            // would be fine either way).
            if (static_cast<std::size_t>(n) < sizeof(scratch)) {
                break;
            }
            continue;
        }
        if (n == 0) {
            // Peer closed cleanly. Signal the listener; do not fire
            // on_error — this isn't a fault, it's a graceful close.
            closed_.set(true);
            // @unsafe — system call
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
            deliver_on_closed_locked(ChannelError::None);
            return false;
        }
        // n < 0
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            break;
        }
        if (err == EINTR) {
            continue;
        }
        // Hard transport error.
        const ChannelError ch = errno_to_channel_error(err);
        {
            auto guard = on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(ch, std::strerror(err));
            }
        }
        closed_.set(true);
        if (fd_ >= 0) {
            // @unsafe — system call
            ::close(fd_);
            fd_ = -1;
        }
        deliver_on_closed_locked(ch);
        return false;
    }

    // Now drain any complete frames out of the inbound buffer.
    while (true) {
        FrameView v{};
        const FrameDecodeStatus s = inbound_.next_frame(v);
        if (s == FrameDecodeStatus::Complete) {
            ChannelFrame cf{v.payload, v.payload_size};
            {
                auto guard = on_frame_.lock().unwrap();
                if (*guard) {
                    (*guard)(cf);
                }
            }
            inbound_.consume_frame();
            continue;
        }
        if (s == FrameDecodeStatus::NeedMoreBytes) {
            break;
        }
        // Malformed.
        {
            auto guard = on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(ChannelError::Internal,
                         "malformed frame on inbound stream");
            }
        }
        closed_.set(true);
        if (fd_ >= 0) {
            // @unsafe — system call
            ::close(fd_);
            fd_ = -1;
        }
        inbound_.reset();
        deliver_on_closed_locked(ChannelError::Internal);
        return false;
    }

    return any_progress;
}

int TcpConnection::handle_write() {
    if (closed_.get()) return PollMode::NO_CHANGE;

    auto guard = outbound_.lock().unwrap();
    auto& buf = *guard;
    if (buf.empty()) {
        return PollMode::READ;
    }

    const ChannelError result = drain_outbound_locked(buf);

    if (result == ChannelError::None) {
        if (buf.empty()) {
            return PollMode::READ;
        }
        return PollMode::NO_CHANGE;
    }
    if (result == ChannelError::WouldBlock) {
        return PollMode::NO_CHANGE;
    }
    // Hard transport error.
    {
        auto err_guard = on_error_.lock().unwrap();
        if (*err_guard) {
            (*err_guard)(result, "outbound write failed");
        }
    }
    closed_.set(true);
    if (fd_ >= 0) {
        // @unsafe — system call
        ::close(fd_);
        fd_ = -1;
    }
    deliver_on_closed_locked(result);
    return PollMode::READ;  // Stop watching writes; closed.
}

void TcpConnection::handle_error() {
    if (closed_.get()) return;
    {
        auto guard = on_error_.lock().unwrap();
        if (*guard) {
            (*guard)(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    close();  // Idempotent; fires on_closed.
}

bool TcpConnection::check_pending_write_update() const {
    if (!pending_write_update_.get()) return false;
    pending_write_update_.set(false);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ChannelError TcpConnection::drain_outbound_locked(
    std::vector<std::uint8_t>& buf) {

    std::size_t offset = 0;
    while (offset < buf.size()) {
        const std::size_t remaining = buf.size() - offset;
        // @unsafe — system call
        ssize_t n = ::send(fd_, buf.data() + offset, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            if (static_cast<std::size_t>(n) < remaining) {
                // Partial write — keep trying.
                continue;
            }
            continue;
        }
        if (n == 0) {
            // send returning 0 with non-zero `remaining` is treated as
            // a transport reset.
            return ChannelError::ConnectionReset;
        }
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            break;  // Caller will retry on next handle_write.
        }
        if (err == EINTR) {
            continue;
        }
        // Hard error. Drop the bytes we couldn't send (the connection
        // is dead anyway).
        if (offset > 0) {
            buf.erase(buf.begin(),
                      buf.begin() + static_cast<std::ptrdiff_t>(offset));
        } else {
            buf.clear();
        }
        return errno_to_channel_error(err);
    }

    if (offset > 0) {
        if (offset == buf.size()) {
            buf.clear();
        } else {
            buf.erase(buf.begin(),
                      buf.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    if (offset == 0 && !buf.empty()) {
        return ChannelError::WouldBlock;
    }
    return ChannelError::None;
}

void TcpConnection::deliver_on_closed_locked(ChannelError reason) {
    if (on_closed_fired_.get()) {
        return;
    }
    on_closed_fired_.set(true);
    auto guard = on_closed_.lock().unwrap();
    if (*guard) {
        (*guard)(reason);
    }
}

ChannelError TcpConnection::errno_to_channel_error(int err) {
    switch (err) {
        case ECONNREFUSED:                 return ChannelError::ConnectionRefused;
        case ECONNRESET:
        case EPIPE:
        case ENOTCONN:                     return ChannelError::ConnectionReset;
        case ETIMEDOUT:                    return ChannelError::Timeout;
        case EADDRINUSE:                   return ChannelError::AddressInUse;
        case EADDRNOTAVAIL:                return ChannelError::AddressInvalid;
        case EACCES:
        case EPERM:                        return ChannelError::PermissionDenied;
        case EMFILE:
        case ENFILE:                       return ChannelError::TooManyOpenFiles;
        default:                           return ChannelError::Internal;
    }
}

// ===========================================================================
// TcpListener
// ===========================================================================

namespace {

// Set the FD non-blocking. Returns 0 on success, errno on failure.
int set_nonblocking_fd(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return errno;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errno;
    return 0;
}

// Format an IPv4 sockaddr as "ip:port". Buffer is small; if the
// formatted string would overflow, returns a "?" placeholder.
std::string sockaddr_to_string(const sockaddr_in& sa) {
    char buf[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "?";
    }
    char out[INET_ADDRSTRLEN + 8] = {0};
    std::snprintf(out, sizeof(out), "%s:%u",
                  buf, static_cast<unsigned>(ntohs(sa.sin_port)));
    return std::string(out);
}

// Parse a "host:port" address into an IPv4 `sockaddr_in`. Accepts
// dotted-quad host literals (no DNS) and decimal port. Returns true
// on success.
bool parse_inet4_addr(std::string_view addr, sockaddr_in& out) {
    auto colon = addr.find_last_of(':');
    if (colon == std::string_view::npos) return false;
    std::string host(addr.substr(0, colon));
    std::string port_str(addr.substr(colon + 1));
    if (host.empty() || port_str.empty()) return false;

    long port = -1;
    try {
        port = std::stol(port_str);
    } catch (...) {
        return false;
    }
    if (port < 0 || port > 65535) return false;

    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) != 1) {
        return false;
    }
    return true;
}

}  // namespace

TcpListener::TcpListener() = default;

TcpListener::~TcpListener() {
    if (listen_fd_ >= 0) {
        // @unsafe — system call
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

ChannelError TcpListener::listen(std::string_view addr) {
    if (closed_.get()) {
        return ChannelError::AddressInUse;
    }
    if (listened_.get()) {
        return ChannelError::AddressInUse;
    }

    sockaddr_in sa;
    if (!parse_inet4_addr(addr, sa)) {
        return ChannelError::AddressInvalid;
    }

    // @unsafe — system call
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return listen_errno_to_channel_error(errno);
    }

    int reuse = 1;
    // @unsafe — system call
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        const int err = errno;
        ::close(fd);
        return listen_errno_to_channel_error(err);
    }

    if (set_nonblocking_fd(fd) != 0) {
        const int err = errno;
        ::close(fd);
        return listen_errno_to_channel_error(err);
    }

    // @unsafe — system call
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0) {
        const int err = errno;
        ::close(fd);
        return listen_errno_to_channel_error(err);
    }
    // Listen backlog: SOMAXCONN is the kernel's silent cap; the call
    // tolerates higher values without erroring.
    // @unsafe — system call
    if (::listen(fd, 128) < 0) {
        const int err = errno;
        ::close(fd);
        return listen_errno_to_channel_error(err);
    }

    // Discover actual bound address (port may have been 0).
    sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    std::memset(&bound, 0, sizeof(bound));
    // @unsafe — system call
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0
        && bound.sin_family == AF_INET) {
        bound_address_ = sockaddr_to_string(bound);
    } else {
        bound_address_ = std::string(addr);
    }

    listen_fd_ = fd;
    listened_.set(true);
    return ChannelError::None;
}

void TcpListener::close() {
    if (closed_.get()) return;
    closed_.set(true);

    if (listen_fd_ >= 0) {
        // @unsafe — system call
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool TcpListener::is_closed() const {
    return closed_.get();
}

std::string TcpListener::local_address() const {
    return bound_address_;
}

void TcpListener::set_on_accept(OnAcceptCallback cb) {
    auto guard = on_accept_.lock().unwrap();
    *guard = std::move(cb);
}

void TcpListener::set_on_error(OnErrorCallback cb) {
    auto guard = on_error_.lock().unwrap();
    *guard = std::move(cb);
}

int TcpListener::fd() const {
    return listen_fd_;
}

int TcpListener::poll_mode() const {
    return PollMode::READ;
}

std::size_t TcpListener::content_size() {
    return 0;
}

bool TcpListener::handle_read() {
    if (closed_.get()) return false;
    if (listen_fd_ < 0) return false;

    bool any_progress = false;
    while (true) {
        sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        std::memset(&peer, 0, sizeof(peer));

        // @unsafe — system call
        int conn_fd = ::accept(listen_fd_,
                               reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn_fd < 0) {
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                break;
            }
            if (err == EINTR) {
                continue;
            }
            // accept() can return EMFILE / ENFILE / ECONNABORTED / etc.
            // ECONNABORTED is retriable but rare; we treat it like
            // EAGAIN and break out so the caller doesn't spin.
            if (err == ECONNABORTED) {
                break;
            }

            // Non-recoverable failure.
            const ChannelError ch = listen_errno_to_channel_error(err);
            {
                auto guard = on_error_.lock().unwrap();
                if (*guard) {
                    (*guard)(ch, std::strerror(err));
                }
            }
            // For EMFILE/ENFILE we don't want to close — the listener
            // is still functional once a fd is freed up. The caller's
            // error callback decides whether to reduce load.
            if (err != EMFILE && err != ENFILE) {
                close();
            }
            return any_progress;
        }

        any_progress = true;

        // Non-blocking accepted socket; matches the rest of the
        // channel layer's expectations.
        if (set_nonblocking_fd(conn_fd) != 0) {
            const int err = errno;
            ::close(conn_fd);
            auto guard = on_error_.lock().unwrap();
            if (*guard) {
                (*guard)(listen_errno_to_channel_error(err),
                         "accept: failed to set non-blocking");
            }
            continue;
        }

        std::string peer_addr_str = sockaddr_to_string(peer);

        // Build a TcpConnection for the new fd and hand the channel
        // proxy to the accept callback.
        auto conn = rusty::Arc<TcpConnection>::make(
            conn_fd, std::move(peer_addr_str));
        ChannelConnectionProxy proxy =
            make_tcp_connection_channel_proxy(std::move(conn));

        auto guard = on_accept_.lock().unwrap();
        if (*guard) {
            (*guard)(std::move(proxy));
        }
        // If no on_accept callback is installed, the proxy drops here
        // and the connection is destroyed.
    }
    return any_progress;
}

int TcpListener::handle_write() {
    return PollMode::NO_CHANGE;
}

void TcpListener::handle_error() {
    if (closed_.get()) return;
    {
        auto guard = on_error_.lock().unwrap();
        if (*guard) {
            (*guard)(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    close();
}

bool TcpListener::check_pending_write_update() const {
    return false;
}

ChannelError TcpListener::listen_errno_to_channel_error(int err) {
    switch (err) {
        case EADDRINUSE:    return ChannelError::AddressInUse;
        case EADDRNOTAVAIL: return ChannelError::AddressInvalid;
        case EACCES:
        case EPERM:         return ChannelError::PermissionDenied;
        case EMFILE:
        case ENFILE:        return ChannelError::TooManyOpenFiles;
        case ECONNRESET:    return ChannelError::ConnectionReset;
        default:            return ChannelError::Internal;
    }
}

}  // namespace rrr
