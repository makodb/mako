#ifndef _LIB_CLIENT_TCP_SERVER_H_
#define _LIB_CLIENT_TCP_SERVER_H_

/**
 * ClientTcpServer - TCP Server for Remote Client Connections
 *
 * This component provides a TCP listener that accepts connections from
 * RemoteDB clients and routes their requests to the ShardReceiver handlers.
 *
 * This is separate from the main transport layer (eRPC/rrr) which is used
 * for inter-shard communication. The ClientTcpServer provides a simple
 * TCP-based interface for external client applications.
 *
 * Usage:
 *   ClientTcpServer server(31000);  // Port for client connections
 *   server.SetReceiver(&shard_receiver);
 *   server.Start();  // Starts listener thread
 *   ...
 *   server.Stop();
 */

#include "lib/server.h"
#include "lib/common.h"
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

// @unsafe { POSIX socket API }
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

namespace mako {

/**
 * ClientTcpServer - Accepts TCP connections from RemoteDB clients
 *
 * Runs a listener thread that accepts client connections and spawns
 * handler threads for each connection. Each handler thread reads
 * requests, calls ShardReceiver handlers, and sends responses.
 */
class ClientTcpServer {
public:
    // @unsafe - Constructor may allocate resources
    explicit ClientTcpServer(int port) : port_(port) {}

    // @unsafe - Destructor closes socket and joins threads
    ~ClientTcpServer() { Stop(); }

    // Set the ShardReceiver that will handle client requests
    // @unsafe - Stores raw pointer (borrowing)
    void SetReceiver(ShardReceiver* receiver) { receiver_ = receiver; }

    // Start the listener thread
    // @unsafe - Creates threads, opens sockets
    bool Start();

    // Stop the listener and all handler threads
    // @unsafe - Closes sockets, joins threads
    void Stop();

    // Check if server is running
    // @safe - Atomic read
    bool IsRunning() const { return running_.load(); }

    // Get the port number
    // @safe - Read-only accessor
    int GetPort() const { return port_; }

private:
    int port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    ShardReceiver* receiver_ = nullptr;

    std::thread listener_thread_;
    std::vector<std::thread> handler_threads_;
    std::mutex threads_mutex_;

    // Listener thread main loop
    // @unsafe - Socket operations
    void ListenerLoop();

    // Handler thread for a single client connection
    // @unsafe - Socket operations
    void HandleClient(int client_fd);

    // Non-copyable
    ClientTcpServer(const ClientTcpServer&) = delete;
    ClientTcpServer& operator=(const ClientTcpServer&) = delete;
};

// ============================================================================
// Inline Implementation
// ============================================================================

// @unsafe - Creates socket, binds, listens
inline bool ClientTcpServer::Start() {
    if (running_.load() || !receiver_) {
        return false;
    }

    // Create listener socket
    // @unsafe { POSIX socket API }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }

    // Allow socket reuse
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));  // @unsafe

    // Bind to port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));  // @unsafe
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);  // @unsafe
        listen_fd_ = -1;
        return false;
    }

    // Start listening
    if (::listen(listen_fd_, 10) < 0) {
        ::close(listen_fd_);  // @unsafe
        listen_fd_ = -1;
        return false;
    }

    // Start listener thread
    running_.store(true);
    stop_requested_.store(false);
    listener_thread_ = std::thread(&ClientTcpServer::ListenerLoop, this);

    return true;
}

// @unsafe - Closes sockets, joins threads
inline void ClientTcpServer::Stop() {
    if (!running_.load()) {
        return;
    }

    stop_requested_.store(true);

    // Close listener socket to unblock accept()
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);  // @unsafe
        listen_fd_ = -1;
    }

    // Join listener thread
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    // Join all handler threads
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& t : handler_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        handler_threads_.clear();
    }

    running_.store(false);
}

// @unsafe - Socket accept loop
inline void ClientTcpServer::ListenerLoop() {
    while (!stop_requested_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Accept client connection
        // @unsafe { POSIX accept }
        int client_fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr),
                                  &client_len);
        if (client_fd < 0) {
            if (stop_requested_.load()) {
                break;
            }
            continue;
        }

        // Spawn handler thread for this client
        {
            std::lock_guard<std::mutex> lock(threads_mutex_);
            handler_threads_.emplace_back(&ClientTcpServer::HandleClient, this, client_fd);
        }
    }
}

// @unsafe - Socket read/write operations
inline void ClientTcpServer::HandleClient(int client_fd) {
    // Buffer for request/response
    static constexpr size_t kMaxBufSize = sizeof(client_kv_request_t) + 1024;
    char req_buf[kMaxBufSize];
    char resp_buf[kMaxBufSize];

    while (!stop_requested_.load()) {
        // Read message type (1 byte)
        uint8_t msg_type;
        ssize_t n = ::read(client_fd, &msg_type, sizeof(msg_type));  // @unsafe
        if (n <= 0) {
            break;  // Connection closed or error
        }

        // Read data length (4 bytes)
        uint32_t data_len;
        n = ::read(client_fd, &data_len, sizeof(data_len));  // @unsafe
        if (n <= 0 || data_len > kMaxBufSize) {
            break;
        }

        // Read request data
        size_t remaining = data_len;
        char* ptr = req_buf;
        while (remaining > 0) {
            n = ::read(client_fd, ptr, remaining);  // @unsafe
            if (n <= 0) {
                goto cleanup;
            }
            ptr += n;
            remaining -= static_cast<size_t>(n);
        }

        // Process request through ShardReceiver
        size_t resp_len = 0;
        if (receiver_) {
            resp_len = receiver_->ReceiveRequest(msg_type, req_buf, resp_buf);
        }

        // Send response
        if (resp_len > 0) {
            remaining = resp_len;
            ptr = resp_buf;
            while (remaining > 0) {
                n = ::write(client_fd, ptr, remaining);  // @unsafe
                if (n <= 0) {
                    goto cleanup;
                }
                ptr += n;
                remaining -= static_cast<size_t>(n);
            }
        }
    }

cleanup:
    ::close(client_fd);  // @unsafe
}

}  // namespace mako

#endif  // _LIB_CLIENT_TCP_SERVER_H_
