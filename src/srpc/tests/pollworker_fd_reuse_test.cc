module;
#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/sync/mpsc.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <vector>
#include <utility>

// This test implementation unit can call the canonical worker's private
// functions without exporting test hooks from the production module.
module srpc.reactor;
import srpc.tcp_channel;
import srpc.channel;
import srpc.epoll_wrapper;
import srpc.pollable_proxy;
import std;
using namespace srpc;

namespace {

class FdReuseOwner {
public:
    explicit FdReuseOwner(int fd) : fd_(fd) {}
    ~FdReuseOwner() { reset(); }
    FdReuseOwner(const FdReuseOwner&) = delete;
    FdReuseOwner& operator=(const FdReuseOwner&) = delete;
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void reset() {
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
    }
private:
    int fd_;
};

// Mirrors tests/helpers/pollworker_fd_reuse.rs through the generated worker
// functions and the actual TcpConnection proxy. No poll thread or sleeps are
// needed: each socket's registration is observed directly through epoll.
void replacement_delivers_a_real_frame(bool deferred_removal) {
    auto commands = rusty::sync::mpsc::channel<PollCommand>();
    auto worker = pollworker_make(std::move(commands.second));
    int old_pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, old_pair), 0);
    FdReuseOwner old_socket(old_pair[0]);
    FdReuseOwner old_peer(old_pair[1]);
    ASSERT_EQ(::fcntl(old_socket.get(), F_SETFL, O_NONBLOCK), 0);

    // F_DUPFD_CLOEXEC chooses an unused high descriptor without closing any
    // unrelated descriptor, even if another process thread allocates one.
    const int old_fd = ::fcntl(old_socket.get(), F_DUPFD_CLOEXEC, 256);
    ASSERT_GE(old_fd, 256);
    old_socket.reset();
    auto old = rusty::Arc<TcpConnection>::new_(TcpConnection::new_(old_fd, "old"));
    pollworker_do_add_pollable(worker, make_tcp_connection_pollable_proxy(old.clone()));

    const std::uint32_t empty_size = 0;
    ASSERT_EQ(::write(old_peer.get(), &empty_size, sizeof(empty_size)), sizeof(empty_size));
    bool initial_ready = false;
    worker.poll_.Wait([&](int fd, int events) {
        initial_ready |= fd == old_fd && (events & PollReady::READABLE) != 0;
    });
    ASSERT_TRUE(initial_ready) << "the original socket must first be registered";

    // Retiring a duplicate Add must leave the live connection registered.
    pollworker_do_add_pollable(worker, make_tcp_connection_pollable_proxy(old.clone()));
    ASSERT_FALSE(old->is_closed());
    ASSERT_TRUE(worker.fd_to_pollable_.get_mut(old_fd).unwrap()->handle_read());

    int replacement_pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, replacement_pair), 0);
    FdReuseOwner replacement_socket(replacement_pair[0]);
    FdReuseOwner replacement_peer(replacement_pair[1]);
    ASSERT_EQ(::fcntl(replacement_socket.get(), F_SETFL, O_NONBLOCK), 0);
    if (deferred_removal) {
        pollworker_do_remove_pollable(worker, old_fd);
        ASSERT_TRUE(worker.pending_remove_.contains(old_fd));
    }
    old->close();
    ASSERT_EQ(old->fd(), -1);
    ASSERT_TRUE(worker.fd_to_pollable_.get(old_fd).unwrap()->is_closed());

    // The registration owns a descriptor lease until explicit retirement.
    ASSERT_GE(::fcntl(old_fd, F_GETFD), 0);
    pollworker_do_close_pollable(worker, old_fd);
    ASSERT_EQ(::fcntl(old_fd, F_GETFD), -1);
    ASSERT_EQ(errno, EBADF);
    ASSERT_FALSE(worker.pending_remove_.contains(old_fd));

    const int replacement_fd =
        ::fcntl(replacement_socket.get(), F_DUPFD_CLOEXEC, old_fd);
    FdReuseOwner replacement_duplicate(replacement_fd);
    ASSERT_EQ(replacement_fd, old_fd) << "the fixture must reuse the exact descriptor";
    replacement_socket.reset();
    auto replacement = rusty::Arc<TcpConnection>::new_(
        TcpConnection::new_(replacement_duplicate.release(), "replacement"));
    std::vector<std::uint8_t> received;
    replacement->set_on_frame(OnFrameCallback::from_callable([&](const ChannelFrame& frame) {
        received.insert(received.end(), frame.payload, frame.payload + frame.size);
    }));
    pollworker_do_add_pollable(worker, make_tcp_connection_pollable_proxy(replacement.clone()));
    pollworker_process_pending_removals(worker);

    const std::uint32_t payload_size = 3;
    ASSERT_EQ(::write(replacement_peer.get(), &payload_size, sizeof(payload_size)),
              sizeof(payload_size));
    ASSERT_EQ(::write(replacement_peer.get(), "new", 3), 3);
    std::vector<int> readable;
    worker.poll_.Wait([&](int fd, int events) {
        if ((events & PollReady::READABLE) != 0) readable.push_back(fd);
    });
    ASSERT_EQ(readable, std::vector<int>{replacement_fd}) << "the replacement must reach epoll";
    for (int fd : readable) {
        ASSERT_TRUE(worker.fd_to_pollable_.get_mut(fd).unwrap()->handle_read());
    }
    EXPECT_EQ(received, (std::vector<std::uint8_t>{'n', 'e', 'w'}));
    EXPECT_FALSE(replacement->is_closed());
}

}  // namespace

TEST(PollWorkerFdReuse, RetiredRegistrationReleasesFdBeforeReplacementIsAdmitted) {
    replacement_delivers_a_real_frame(false);
}

TEST(PollWorkerFdReuse, ReusedFdCancelsRetiredRegistrationsPendingRemoval) {
    replacement_delivers_a_real_frame(true);
}

TEST(PollWorkerFdReuse, ClosedQueuedAddKeepsDescriptorUntilCommandRetirement) {
    auto commands = rusty::sync::mpsc::channel<PollCommand>();
    auto worker = pollworker_make(std::move(commands.second));
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    FdReuseOwner socket(pair[0]);
    FdReuseOwner peer(pair[1]);
    ASSERT_EQ(::fcntl(socket.get(), F_SETFL, O_NONBLOCK), 0);
    ASSERT_EQ(::fcntl(peer.get(), F_SETFL, O_NONBLOCK), 0);
    const int fd = ::fcntl(socket.get(), F_DUPFD_CLOEXEC, 256);
    ASSERT_GE(fd, 256);
    socket.reset();
    auto connection = rusty::Arc<TcpConnection>::new_(TcpConnection::new_(fd, "leased"));
    ASSERT_TRUE(commands.first.send(PollCommand_AddPollable{
        make_tcp_connection_pollable_proxy(connection.clone())}).is_ok());

    connection->close();
    ASSERT_TRUE(connection->is_closed());
    ASSERT_EQ(connection->fd(), -1);
    ASSERT_GE(::fcntl(fd, F_GETFD), 0);
    unsigned char byte = 0;
    ASSERT_EQ(::read(peer.get(), &byte, 1), 0) << "logical close must shut down the socket";
    pollworker_process_commands(worker);
    EXPECT_FALSE(worker.fd_to_pollable_.contains_key(fd));
    EXPECT_FALSE(worker.mode_.contains_key(fd));
    ASSERT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(PollWorkerFdReuse, ClosedActiveRegistrationKeepsDescriptorUntilDeferredRemoval) {
    auto commands = rusty::sync::mpsc::channel<PollCommand>();
    auto worker = pollworker_make(std::move(commands.second));
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    FdReuseOwner socket(pair[0]);
    FdReuseOwner peer(pair[1]);
    ASSERT_EQ(::fcntl(socket.get(), F_SETFL, O_NONBLOCK), 0);
    const int fd = ::fcntl(socket.get(), F_DUPFD_CLOEXEC, 256);
    ASSERT_GE(fd, 256);
    socket.reset();
    auto connection = rusty::Arc<TcpConnection>::new_(TcpConnection::new_(fd, "leased"));
    pollworker_do_add_pollable(worker, make_tcp_connection_pollable_proxy(connection.clone()));
    ASSERT_TRUE(worker.mode_.contains_key(fd));

    int other_pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, other_pair), 0);
    FdReuseOwner other_socket(other_pair[0]);
    FdReuseOwner other_peer(other_pair[1]);
    ASSERT_EQ(::fcntl(other_socket.get(), F_SETFL, O_NONBLOCK), 0);
    const int other_fd = ::fcntl(other_socket.get(), F_DUPFD_CLOEXEC, 256);
    ASSERT_GE(other_fd, 256);
    other_socket.reset();
    auto unrelated = rusty::Arc<TcpConnection>::new_(TcpConnection::new_(other_fd, "unrelated"));
    pollworker_do_add_pollable(worker, make_tcp_connection_pollable_proxy(unrelated.clone()));

    connection->close();
    ASSERT_TRUE(connection->is_closed());
    ASSERT_EQ(connection->fd(), -1);
    ASSERT_GE(::fcntl(fd, F_GETFD), 0);
    ASSERT_NE(fd, other_fd);
    pollworker_do_remove_pollable(worker, fd);
    ASSERT_GE(::fcntl(fd, F_GETFD), 0);
    pollworker_process_pending_removals(worker);
    ASSERT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_FALSE(worker.fd_to_pollable_.contains_key(fd));
    EXPECT_FALSE(worker.mode_.contains_key(fd));

    ASSERT_GE(::fcntl(other_fd, F_GETFD), 0);
    const std::uint32_t empty_size = 0;
    ASSERT_EQ(::write(other_peer.get(), &empty_size, sizeof(empty_size)), sizeof(empty_size));
    std::vector<int> ready;
    worker.poll_.Wait([&](int ready_fd, int events) {
        if ((events & PollReady::READABLE) != 0) ready.push_back(ready_fd);
    });
    ASSERT_EQ(ready, std::vector<int>{other_fd});
    EXPECT_TRUE(worker.fd_to_pollable_.get_mut(other_fd).unwrap()->handle_read());
    EXPECT_FALSE(unrelated->is_closed());
}

TEST(PollWorkerFdReuse, ClosedQueuedListenerKeepsDescriptorUntilCommandRetirement) {
    auto commands = rusty::sync::mpsc::channel<PollCommand>();
    auto worker = pollworker_make(std::move(commands.second));
    auto listener = rusty::Arc<TcpListener>::make_with([] { return TcpListener::new_(); });
    ASSERT_EQ(listener->listen("127.0.0.1:0"), ChannelError::None);
    const int fd = listener->fd();
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(commands.first.send(PollCommand_AddPollable{
        make_tcp_listener_pollable_proxy(listener.clone())}).is_ok());
    listener->close();
    ASSERT_TRUE(listener->is_closed());
    ASSERT_EQ(listener->fd(), -1);
    ASSERT_GE(::fcntl(fd, F_GETFD), 0);
    pollworker_process_commands(worker);
    EXPECT_FALSE(worker.fd_to_pollable_.contains_key(fd));
    EXPECT_FALSE(worker.mode_.contains_key(fd));
    ASSERT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
