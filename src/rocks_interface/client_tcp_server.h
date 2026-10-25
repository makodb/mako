#ifndef _LIB_CLIENT_TCP_SERVER_H_
#define _LIB_CLIENT_TCP_SERVER_H_

/**
 * ClientTcpServer - TCP Server for Remote Client Connections
 *
 * This component provides a TCP listener that accepts connections from
 * RemoteDB clients and routes their requests to the ShardReceiver handlers.
 *
 * Supports multiple concurrent clients with proper transaction isolation:
 * - Uses a worker pool with fixed number of slots (= nthreads)
 * - Each worker slot binds to a unique transaction context
 * - When all workers are busy, new clients receive a rejection message
 *
 * This is separate from the main transport layer (srpc/rpc) which is used
 * for inter-shard communication. The ClientTcpServer provides a simple
 * TCP-based interface for external client applications.
 *
 * Usage:
 *   ClientTcpServer server(31000);
 *   server.SetReceiver(&shard_receiver);
 *   server.SetMaxClients(nthreads);  // Configure worker pool size
 *   server.Start();
 *   ...
 *   server.Stop();
 */

#include "mako/lib/server.h"
#include "mako/lib/common.h"
#include "mako/benchmarks/bench.h"  // scoped_db_thread_ctx (worker Silo binding)
#include "mako/sto/Transaction.hh"
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>
#include <condition_variable>

// @unsafe { POSIX socket API }
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

// Forward declaration for database context
class abstract_db;

namespace mako {

/**
 * WorkerSlot - Manages a single worker slot for client handling
 *
 * Each slot represents one concurrent client capacity.
 * The slot tracks whether it's in use and provides atomic acquire/release.
 *
 * Note: This struct is not copyable/movable due to atomics.
 * Use std::unique_ptr<WorkerSlot> in containers.
 */
struct WorkerSlot {
    std::atomic<bool> in_use{false};      // Is slot currently occupied?
    std::atomic<int> client_fd{-1};       // Socket fd of connected client
    std::thread worker_thread;             // Worker thread for this slot
    int worker_id{0};                      // Unique worker ID for TThread context

    // Default constructor
    WorkerSlot() = default;

    // Non-copyable and non-movable (due to atomics)
    WorkerSlot(const WorkerSlot&) = delete;
    WorkerSlot& operator=(const WorkerSlot&) = delete;
    WorkerSlot(WorkerSlot&&) = delete;
    WorkerSlot& operator=(WorkerSlot&&) = delete;

    // @safe - Atomically try to acquire this slot
    // Returns true if successfully acquired, false if already in use
    bool TryAcquire() {
        bool expected = false;
        return in_use.compare_exchange_strong(expected, true,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed);
    }

    // @safe - Release this slot
    void Release() {
        client_fd.store(-1, std::memory_order_relaxed);
        in_use.store(false, std::memory_order_release);
    }

    // @safe - Check if slot is in use
    bool IsInUse() const {
        return in_use.load(std::memory_order_acquire);
    }
};

/**
 * ClientTcpServer - Accepts TCP connections from RemoteDB clients
 *
 * Uses a worker pool to support multiple concurrent clients with
 * proper transaction isolation. Each client is assigned to a worker
 * slot that has its own transaction context.
 */
class ClientTcpServer {
public:
    // @unsafe - Constructor allocates worker slots
    explicit ClientTcpServer(int port, size_t max_clients = 4)
        : port_(port), max_clients_(max_clients) {
        AllocateSlots(max_clients_);
    }

    // @unsafe - Destructor closes socket and joins threads
    ~ClientTcpServer() { Stop(); }

    // Set the ShardReceiver that will handle client requests
    // @unsafe - Stores raw pointer (borrowing)
    void SetReceiver(ShardReceiver* receiver) { receiver_ = receiver; }

    // Set maximum number of concurrent clients (worker pool size)
    // Must be called before Start()
    // @safe - Simple setter
    void SetMaxClients(size_t max_clients) {
        if (!running_.load()) {
            max_clients_ = max_clients;
            AllocateSlots(max_clients_);
        }
    }

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

    // Get maximum concurrent clients
    // @safe - Read-only accessor
    size_t GetMaxClients() const { return max_clients_; }

    // Get current number of active clients
    // @safe - Counts active slots
    size_t GetActiveClients() const {
        size_t count = 0;
        for (const auto& slot : worker_slots_) {
            if (slot && slot->IsInUse()) count++;
        }
        return count;
    }

private:
    int port_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    ShardReceiver* receiver_ = nullptr;

    // Worker pool - using std::unique_ptr because:
    // 1. WorkerSlot contains std::thread and atomics which are not copyable
    // 2. rusty::Box::make() requires the type to be move-constructible
    // 3. std::make_unique can construct the object in-place without moves
    // @note: This is an acceptable use of std::unique_ptr per CLAUDE.md guidelines
    size_t max_clients_;
    std::vector<std::unique_ptr<WorkerSlot>> worker_slots_;
    std::mutex slots_mutex_;  // Protects thread join operations

    std::thread listener_thread_;

    // @safe - Allocate worker slots
    void AllocateSlots(size_t count) {
        worker_slots_.clear();
        worker_slots_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto slot = std::make_unique<WorkerSlot>();
            slot->worker_id = static_cast<int>(i);
            worker_slots_.push_back(std::move(slot));
        }
    }

    // @safe - Try to acquire an available worker slot
    // Returns slot index if successful, -1 if all slots busy
    int TryAcquireSlot() {
        for (size_t i = 0; i < max_clients_; ++i) {
            if (worker_slots_[i] && worker_slots_[i]->TryAcquire()) {
                return static_cast<int>(i);
            }
        }
        return -1;  // All slots busy
    }

    // @safe - Release a worker slot
    void ReleaseSlot(int slot_id) {
        if (slot_id >= 0 && slot_id < static_cast<int>(max_clients_) && worker_slots_[slot_id]) {
            worker_slots_[slot_id]->Release();
        }
    }

    // Listener thread main loop
    // @unsafe - Socket operations
    void ListenerLoop();

    // Worker thread for a single client connection (with transaction context)
    // @unsafe - Socket operations, TThread calls
    void WorkerThread(int slot_id, int client_fd);

    // Send rejection response to client
    // @unsafe - Socket write
    void SendRejectionResponse(int client_fd, const char* message);

    // Handle client requests (called from WorkerThread with context bound)
    // @unsafe - Socket operations
    void HandleClientRequests(int client_fd);

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

    // Start listening with backlog = max_clients (reasonable queue size)
    if (::listen(listen_fd_, static_cast<int>(max_clients_)) < 0) {
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

    // Wake the listener: on Linux, close() alone does NOT unblock a
    // thread parked in accept() — shutdown() does (accept returns
    // with an error, and the loop sees stop_requested_).
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);  // @unsafe
        ::close(listen_fd_);  // @unsafe
        listen_fd_ = -1;
    }

    // Close all active client connections to unblock their threads
    for (auto& slot : worker_slots_) {
        if (slot) {
            int fd = slot->client_fd.load();
            if (fd >= 0) {
                ::shutdown(fd, SHUT_RDWR);  // @unsafe - Signal threads to exit
            }
        }
    }

    // Join listener thread
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    // Join all worker threads
    {
        std::lock_guard<std::mutex> lock(slots_mutex_);
        for (auto& slot : worker_slots_) {
            if (slot && slot->worker_thread.joinable()) {
                slot->worker_thread.join();
            }
        }
    }

    running_.store(false);
}

// @unsafe - Socket accept loop with worker pool management
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

        // Try to acquire a worker slot
        int slot_id = TryAcquireSlot();
        if (slot_id < 0) {
            // All workers busy - send rejection and close connection
            SendRejectionResponse(client_fd, "All servers occupied, please try later");
            ::close(client_fd);  // @unsafe
            continue;
        }

        // Store client fd in slot for potential shutdown
        worker_slots_[slot_id]->client_fd.store(client_fd);

        // Join any previous thread for this slot (should already be done, but be safe)
        {
            std::lock_guard<std::mutex> lock(slots_mutex_);
            if (worker_slots_[slot_id]->worker_thread.joinable()) {
                worker_slots_[slot_id]->worker_thread.join();
            }
            // Spawn worker thread with transaction context binding
            worker_slots_[slot_id]->worker_thread = std::thread(
                &ClientTcpServer::WorkerThread, this, slot_id, client_fd);
        }
    }
}

// @unsafe - Send rejection response when server is busy
inline void ClientTcpServer::SendRejectionResponse(int client_fd, const char* message) {
    // Send message type first
    uint8_t msg_type = clientServerBusyType;
    ::write(client_fd, &msg_type, sizeof(msg_type));  // @unsafe

    // Send response length
    client_server_busy_response_t resp;
    resp.status = ErrorCode::SERVER_BUSY;
    memset(resp.message, 0, sizeof(resp.message));  // @unsafe
    strncpy(resp.message, message, sizeof(resp.message) - 1);  // @unsafe

    uint32_t resp_len = sizeof(resp);
    ::write(client_fd, &resp_len, sizeof(resp_len));  // @unsafe
    ::write(client_fd, &resp, resp_len);  // @unsafe
}

// @unsafe - Worker thread with Silo/Masstree thread binding
inline void ClientTcpServer::WorkerThread(int slot_id, int client_fd) {
    // Register once for the connection lifetime. The explicit receiver owner
    // selects allocator/RCU state before thread_init in multi-shard processes.
    // loader=true skips the benchmark worker's fixed-port RPC client; forwarding
    // lazily owns a separate ephemeral ShardClient and preserves the caller ID.
    abstract_db* db = receiver_ ? receiver_->GetDb() : nullptr;
    if (db) {
        const int owner = receiver_->GetOwner();
        auto& config = BenchmarkConfig::getInstance();
        BenchmarkConfig::setThreadLocalShardIndex(owner);
        if (config.getConfig() && config.getConfig()->multi_shard_mode) {
            auto* shard = config.getShardContext(owner);
            if (!shard || !shard->runtime.get()) {
                ::close(client_fd);
                ReleaseSlot(slot_id);
                return;
            }
            const_cast<SiloRuntime*>(shard->runtime.get())->BindToCurrentThread();
        } else {
            SiloRuntime::Current()->BindToCurrentThread();
        }
        scoped_db_thread_ctx ctx(db, /*loader=*/true);
        // Gateway I/O is never bulk loading: unactivated native runtime fails
        // closed instead of taking the loader's pre-bootstrap lease bypass.
        TThread::in_loading_phase = false;
        TThread::set_shard_index(owner);
        TThread::set_nshards(config.getNshards());
        TThread::set_pid(0);
        HandleClientRequests(client_fd);
    }

    // Close connection
    ::close(client_fd);  // @unsafe

    // Release slot when done
    ReleaseSlot(slot_id);
}

// @unsafe - Socket read/write operations (separated from thread setup)
inline void ClientTcpServer::HandleClientRequests(int client_fd) {
    // Gateway-only framing. Internal shard request types 1-18 are not an
    // alternate unauthenticated/non-ID client entry point.
    MakoGatewayRequest request{};
    MakoGatewayResponse response{};
    char* req_buf = reinterpret_cast<char*>(&request);
    char* resp_buf = reinterpret_cast<char*>(&response);
    static constexpr size_t kMaxBufSize = sizeof(request);

    while (!stop_requested_.load()) {
        // Read message type (1 byte)
        uint8_t msg_type;
        ssize_t n = ::read(client_fd, &msg_type, sizeof(msg_type));  // @unsafe
        if (n <= 0) {
            break;  // Connection closed or error
        }

        // Read data length (4 bytes)
        uint32_t data_len = 0;
        size_t header_read = 0;
        while (header_read < sizeof(data_len)) {
            n = ::read(client_fd, reinterpret_cast<char*>(&data_len) + header_read,
                       sizeof(data_len) - header_read);
            if (n <= 0) return;
            header_read += static_cast<size_t>(n);
        }
        if (data_len < offsetof(MakoGatewayRequest, value) || data_len > kMaxBufSize)
            return;
        request = {};
        response = {};

        // Read request data
        size_t remaining = data_len;
        char* ptr = req_buf;
        while (remaining > 0) {
            n = ::read(client_fd, ptr, remaining);  // @unsafe
            if (n <= 0) {
                return;  // Connection error
            }
            ptr += n;
            remaining -= static_cast<size_t>(n);
        }

        if (request.version != MAKO_GATEWAY_VERSION || request.kind != msg_type
            || request.key_length > sizeof(request.key)
            || request.coordinate_length > sizeof(request.coordinate)
            || request.name_length > sizeof(request.name)
            || request.value_length > sizeof(request.value)
            || data_len != offsetof(MakoGatewayRequest, value) + request.value_length)
            return;
        switch (msg_type) {
        case MAKO_GATEWAY_BEGIN: case MAKO_GATEWAY_COMMIT: case MAKO_GATEWAY_ROLLBACK:
        case MAKO_GATEWAY_PUT: case MAKO_GATEWAY_GET: case MAKO_GATEWAY_DELETE:
        case MAKO_GATEWAY_ROUTE: case MAKO_GATEWAY_INSERT:
            break;
        default:
            return;
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
                    return;  // Connection error
                }
                ptr += n;
                remaining -= static_cast<size_t>(n);
            }
        }
    }
}

}  // namespace mako

#endif  // _LIB_CLIENT_TCP_SERVER_H_
