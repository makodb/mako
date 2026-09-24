#include "std_compat.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

import srpc.channel;
import srpc.client;
import srpc.logging;
import srpc.reactor;
import srpc.serializable;
import srpc.server;
import srpc.tcp_channel;
import srpc.utils;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { require(fd >= 0, "native descriptor creation failed"); }
    ~Fd() { ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
std::atomic<unsigned> signals{0};
void count_signal(int) { signals.fetch_add(1, std::memory_order_relaxed); }
struct SignalGuard {
    struct sigaction previous{};
    SignalGuard() {
        struct sigaction action{};
        action.sa_handler = count_signal;
        ::sigemptyset(&action.sa_mask);
        require(::sigaction(SIGUSR1, &action, &previous) == 0, "install signal handler");
    }
    ~SignalGuard() { ::sigaction(SIGUSR1, &previous, nullptr); }
};
void wait_for_io(pid_t tid, int fd, long syscall) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream state("/proc/self/task/" + std::to_string(tid) + "/syscall");
        std::string line;
        std::getline(state, line);
        long actual_call = -1;
        unsigned long actual_fd = 0;
        if (std::sscanf(line.c_str(), "%ld %lx", &actual_call, &actual_fd) == 2 &&
            actual_call == syscall && actual_fd == static_cast<unsigned long>(fd)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("worker did not enter native I/O syscall");
}
void interrupt(pthread_t thread) {
    signals.store(0, std::memory_order_relaxed);
    require(::pthread_kill(thread, SIGUSR1) == 0, "deliver interruption");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (signals.load(std::memory_order_relaxed) == 0) {
        require(std::chrono::steady_clock::now() < deadline, "signal handler did not execute");
        std::this_thread::yield();
    }
}
void fd_checks() {
    SignalGuard signal_guard;
    int descriptors[2];
    require(::pipe(descriptors) == 0, "create read pipe");
    Fd reader(descriptors[0]);
    Fd writer(descriptors[1]);
    std::promise<pid_t> started;
    auto tid_future = started.get_future();
    std::array<unsigned char, 7> output{};
    std::size_t count = 0;
    std::thread read_worker([&] {
        started.set_value(static_cast<pid_t>(::syscall(SYS_gettid)));
        auto source = srpc::FdSource::new_(reader.value);
        count = source.read_bytes(output.data(), output.size());
    });
    const pid_t tid = tid_future.get();
    wait_for_io(tid, reader.value, SYS_read);
    interrupt(read_worker.native_handle());
    wait_for_io(tid, reader.value, SYS_read);
    require(::write(writer.value, "abc", 3) == 3, "write first source fragment");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    wait_for_io(tid, reader.value, SYS_read);
    require(::write(writer.value, "defg", 4) == 4, "write second source fragment");
    read_worker.join();
    require(count == 7 && std::memcmp(output.data(), "abcdefg", 7) == 0,
            "canonical source lost partial or interrupted read");

    require(::pipe(descriptors) == 0, "create write pipe");
    Fd write_reader(descriptors[0]);
    const int write_fd = descriptors[1];
    std::vector<unsigned char> payload(1024 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = i % 251;
    std::promise<pid_t> write_started;
    auto write_tid = write_started.get_future();
    std::thread write_worker([&] {
        Fd write_owner(write_fd);
        write_started.set_value(static_cast<pid_t>(::syscall(SYS_gettid)));
        auto sink = srpc::FdSink::new_(write_fd);
        sink.write_bytes(payload.data(), payload.size());
    });
    wait_for_io(write_tid.get(), write_fd, SYS_write);
    interrupt(write_worker.native_handle());
    std::vector<unsigned char> received;
    std::array<unsigned char, 4096> chunk{};
    for (;;) {
        const auto amount = ::read(write_reader.value, chunk.data(), chunk.size());
        if (amount == 0) break;
        require(amount > 0, "read write-test payload");
        received.insert(received.end(), chunk.begin(), chunk.begin() + amount);
    }
    write_worker.join();
    require(received == payload, "canonical sink lost interrupted partial write");
}
std::string listener_address(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "get listener address");
    return "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}
void tcp_checks() {
    Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "bind listener");
    require(::listen(listener.value, 0) == 0, "listen with bounded accept queue");
    const auto endpoint = listener_address(listener.value);
    auto poll_thread = srpc::PollThread::create();
    auto factory = srpc::TcpFactory::new_(poll_thread.clone());
    factory.set_connect_timeout_ms(30);
    auto connected = factory.connect(endpoint);
    require(connected.error == srpc::ChannelError::None && connected.connection.is_some(),
            "canonical factory failed real connection");
    auto timed_out = factory.connect(endpoint);
    require(timed_out.error == srpc::ChannelError::Timeout && timed_out.connection.is_none(),
            "canonical factory ignored full accept queue timeout");
    Fd accepted(::accept(listener.value, nullptr, nullptr));
    connected.connection.as_ref().unwrap()->close();
    poll_thread->shutdown();

    Fd closed_listener(::socket(AF_INET, SOCK_STREAM, 0));
    require(::bind(closed_listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "bind refused endpoint");
    const auto refused_endpoint = listener_address(closed_listener.value);
    auto refused = factory.connect(refused_endpoint);
    require(refused.error == srpc::ChannelError::ConnectionRefused && refused.connection.is_none(),
            "canonical factory lost SO_ERROR refusal");
}
int socket_option(int fd, int level, int option) {
    int value = -1;
    socklen_t size = sizeof(value);
    require(::getsockopt(fd, level, option, &value, &size) == 0,
            "read real TCP socket option");
    return value;
}
void keepalive_checks() {
    Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "bind keepalive listener");
    require(::listen(listener.value, 1) == 0, "listen for keepalive connection");
    socklen_t size = sizeof(address);
    require(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0,
            "get keepalive listener address");
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "create keepalive socket");
    require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "connect keepalive socket");
    Fd accepted(::accept(listener.value, nullptr, nullptr));
    auto connection = srpc::TcpConnection::new_(fd, "keepalive-peer");
    require(connection.set_keepalive(true, 17, 4, 6), "apply TCP keepalive");
    require(socket_option(fd, SOL_SOCKET, SO_KEEPALIVE) == 1, "SO_KEEPALIVE enabled");
    require(socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 17, "TCP_KEEPIDLE value");
    require(socket_option(fd, IPPROTO_TCP, TCP_KEEPINTVL) == 4, "TCP_KEEPINTVL value");
    require(socket_option(fd, IPPROTO_TCP, TCP_KEEPCNT) == 6, "TCP_KEEPCNT value");
    require(connection.set_keepalive(false, 0, 0, 0), "disable TCP keepalive");
    require(socket_option(fd, SOL_SOCKET, SO_KEEPALIVE) == 0, "SO_KEEPALIVE disabled");
    require(socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 17, "disable keeps tuning");
    require(!connection.set_keepalive(true, 0, 7, 8), "invalid option must be reported");
    require(socket_option(fd, IPPROTO_TCP, TCP_KEEPINTVL) == 7, "continue after rejected idle");
    require(socket_option(fd, IPPROTO_TCP, TCP_KEEPCNT) == 8, "continue to count");
    connection.close();
    require(!connection.set_keepalive(true, 17, 4, 6), "closed descriptor rejects options");
}
void poll_count_checks() {
    int pair[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "create removal socket pair");
    Fd socket(pair[0]);
    Fd peer(pair[1]);
    auto worker = srpc::PollThread::create();
    require(worker->get_remove_count() == 0, "fresh removal count");
    std::vector<std::thread> senders;
    for (int thread = 0; thread < 4; ++thread) {
        senders.emplace_back([worker = worker.clone(), fd = socket.value] {
            for (int request = 0; request < 8; ++request) worker->remove_fd(fd);
        });
    }
    for (auto& sender : senders) sender.join();
    require(worker->get_remove_count() == 32, "accepted concurrent removals counted");
    worker->shutdown();
    worker->remove_fd(socket.value);
    require(worker->get_remove_count() == 32, "shutdown rejects later removal");
}
void logging_sink_checks() {
    struct Sink : std::stringbuf {
        unsigned flushes = 0;
        int sync() override {
            ++flushes;
            return std::stringbuf::sync();
        }
    } sink;
    auto* original = std::cout.rdbuf(&sink);
    srpc::log_sink_write(std::string_view("a\0b\n", 4));
    srpc::log_sink_write("");
    std::cout.rdbuf(original);
    require(sink.str() == std::string("a\0b\n\n\n", 6),
            "logging sink changed bytes or newline count");
    require(sink.flushes == 2, "logging sink did not flush each line");
}
void value_checks() {
    for (const auto& [text, expected] : std::vector<std::pair<std::string, std::int32_t>>{
             {"0", 0}, {"  +123port", 123}, {"-42tail", -42},
             {"2147483647", INT32_MAX}, {"-2147483648", INT32_MIN}}) {
        auto value = srpc::server_parse_port(text);
        require(value.is_some() && value.unwrap() == expected, "canonical decimal prefix parse");
    }
    for (const std::string text : {"", "+", "- 1", "2147483648", "-2147483649"}) {
        require(srpc::server_parse_port(text).is_none(), "canonical parser accepted invalid value");
    }
    const int first = srpc::find_open_port();
    require(first >= 1024 && first < 65000, "canonical port scan failed");
    {
        Fd occupied(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = static_cast<std::uint16_t>(first);
        require(::bind(occupied.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "occupy selected port");
        require(srpc::find_open_port() > first, "scan reused occupied port");
    }
    require(srpc::find_open_port() == first, "scan did not release probe descriptor");
    std::array<char, 256> hostname{};
    require(::gethostname(hostname.data(), hostname.size() - 1) == 0, "native hostname");
    require(srpc::get_host_name() == hostname.data(), "canonical hostname used a facade");
    const auto timestamp = srpc::log_time_now();
    const std::string_view timestamp_view = timestamp.as_str();
    require(timestamp_view.size() == 23 && timestamp_view[4] == '-' && timestamp_view[19] == '.',
            "canonical timestamp shape");
}
}  // namespace

int main() {
    std::set<int> draws;
    for (int i = 0; i < 256; ++i) {
        const auto value = srpc::client_rand(-5, 5);
        require(value >= -5 && value <= 5, "client random range violated");
        draws.insert(value);
    }
    require(draws.size() > 1, "client random draws were constant");

    try {
        value_checks();
        logging_sink_checks();
        fd_checks();
        tcp_checks();
        keepalive_checks();
        poll_count_checks();
        std::cout << "native policy checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
