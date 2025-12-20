#pragma once
#include <rusty/rusty.hpp>
#include <rusty/result.hpp>
#include <rusty/cell.hpp>

#include <unordered_map>
#include <chrono>
#include <mutex>

#include "misc/marshal.hpp"
#include "reactor/epoll_wrapper.h"
#include "reactor/reactor.h"

// External safety annotations for system functions used in this module
// @external: {
//   socket: [unsafe, (int, int, int) -> int]
//   connect: [unsafe, (int, const struct sockaddr*, socklen_t) -> int]
//   close: [unsafe, (int) -> int]
//   setsockopt: [unsafe, (int, int, int, const void*, socklen_t) -> int]
//   getaddrinfo: [unsafe, (const char*, const char*, const struct addrinfo*, struct addrinfo**) -> int]
//   freeaddrinfo: [unsafe, (struct addrinfo*) -> void]
//   gai_strerror: [unsafe, (int) -> const char*]
//   memset: [unsafe, (void*, int, size_t) -> void*]
//   strcpy: [unsafe, (char*, const char*) -> char*]
//   std::lock_guard: [safe, (std::mutex&) -> void]
//   std::unique_lock: [safe, (std::mutex&) -> void]
//   std::chrono::duration: [safe, (double) -> void]
//   std::function: [safe, (auto) -> void]
//   std::vector::push_back: [safe, (auto) -> void]
//   rrr::Log::error: [unsafe]
//   Log_error: [unsafe]
// }

namespace rrr {

class Future;
// @unsafe - Forward declarations
class Client;
class ClientConnection;

// Type alias for Future result (replaces nullable Future* returns)
// Ok(Arc<Future>) on success, Err(error_code) on failure
using FutureResult = rusty::Result<rusty::Arc<Future>, i32>;

// @safe - Simple attribute struct for Future callbacks
struct FutureAttr {
    FutureAttr(const std::function<void(rusty::Arc<Future>)>& cb = std::function<void(rusty::Arc<Future>)>()) : callback(cb) { }

    // callback should be fast, otherwise it hurts rpc performance
    // Receives Arc<Future> for lifetime safety (callback keeps Future alive)
    std::function<void(rusty::Arc<Future>)> callback;
};

// Thread-safe future for async RPC results using low-level synchronization
// Uses mutable fields and condition variables which require unsafe operations
// MIGRATED: Now uses rusty::Arc<Future> instead of RefCounted for memory safety
class Future { // @unsafe
    friend class rusty::Arc<Future>;  // Allow Arc to construct/destroy
    friend class Client;              // Client needs to call private constructor and set error
    friend class ClientConnection;    // ClientConnection needs access to set error and notify

    struct State {
        bool ready = false;
        bool timed_out = false;
    };

    i64 xid_;
    rusty::Cell<i32> error_code_;  // Cell for interior mutability of Copy type

    FutureAttr attr_;
    rusty::UnsafeCell<Marshal> reply_;  // UnsafeCell for interior mutability in unsafe class

    uint64_t timeout_{1000000}; // default timeout 1s (jetpack)
    rusty::Mutex<State> state_;  // Mutex protects State (ready/timed_out flags)
    rusty::UnsafeCell<rusty::Condvar> ready_cond_;  // UnsafeCell for interior mutability

    // @safe - Uses rusty::Mutex and rusty::Condvar together (Rust-like pattern)
    // Takes Arc<Future> self parameter for callback safety
    void notify_ready(rusty::Arc<Future> self) const;

    // Private destructor - only Arc can delete
    // @safe - RAII destructors handle cleanup automatically
    ~Future() = default;

    // Private constructor - only Arc factory can create
    // @safe - Default initialization with RAII primitives
    Future(i64 xid, const FutureAttr& attr = FutureAttr())
            : xid_(xid), error_code_(0), attr_(attr), reply_(), state_(State{}),
              ready_cond_() {
    }

public:

    // Factory method for Arc creation
    // @safe - Creates Future wrapped in Arc for memory safety
    static rusty::Arc<Future> create(i64 xid, const FutureAttr& attr = FutureAttr()) {
        return rusty::Arc<Future>::make(xid, attr);
    }

    // @safe - Uses rusty::Mutex
    bool ready() const {
        auto guard = state_.lock().unwrap();
        return (*guard).ready;
    }

    // @safe - Uses rusty::Mutex and rusty::Condvar together
    void wait() const;

    // @safe - Uses rusty::Mutex and rusty::Condvar together
    void timed_wait(double sec) const;

    // @safe - Uses rusty::Mutex
    bool timed_out() const {
        auto guard = state_.lock().unwrap();
        return (*guard).timed_out;
    }

    // Returns reference to reply with lifetime tied to Future
    // @lifetime: (&'a) -> &'a
    // @unsafe - Returns reference through UnsafeCell (caller must ensure lifetime safety)
    Marshal& get_reply() const {
        wait();
        return *reply_.get();
    }

    // @safe - Calls safe wait()/timed_wait() methods
    i32 get_error_code() const {
        if (timeout_ > 0) {
            double x = timeout_;
            x = x / 1000000;
            timed_wait(x);
        } else {
            wait();
        }
        return error_code_.get();
    }

    i64 get_xid() const {
        return xid_;
    }

    // =========================================================================
    // Compatibility shim for legacy code that calls Future::safe_release()
    // =========================================================================
    // With rusty::Arc, manual release is no longer needed - Arc automatically
    // cleans up when the last reference goes out of scope. These are NO-OP
    // functions that exist solely for backward compatibility with existing
    // call sites (raft/macros.h, fpga_raft/commo.cc, paxos/commo.cc, etc.)
    //
    // Old pattern (raw pointer):
    //   Future* fu = proxy->async_Something(fuattr);
    //   Future::safe_release(fu);  // Manual cleanup required
    //
    // New pattern (Arc):
    //   auto fu = proxy->async_Something(fuattr);
    //   // No cleanup needed - Arc handles it automatically when fu goes out of scope
    //   Future::safe_release(fu);  // NO-OP, just for compatibility
    // =========================================================================

    // NO-OP: Arc automatically releases when dropped
    static inline void safe_release(rusty::Arc<Future> fu) {
        (void)fu;  // Intentionally empty - Arc handles cleanup
    }

    // NO-OP: Legacy overload for any remaining raw pointer usage in old code paths
    static inline void safe_release(Future* fu) {
        (void)fu;  // Intentionally empty - should not be called in new code
    }

    // NO-OP: Overload for FutureResult (Result<Arc<Future>, i32>) - jetpack compatibility
    static inline void safe_release(FutureResult fu_result) {
        (void)fu_result;  // Intentionally empty - Arc handles cleanup
    }
};

// @safe - RAII container for managing multiple futures
// MIGRATED: Now uses Arc<Future> for automatic memory management
class FutureGroup {
private:
    std::vector<rusty::Arc<Future>> futures_;

public:
    // @safe - Adds future to group (has internal @unsafe block for Log_error)
    void add(rusty::Arc<Future> f) {
        if (!f) {  // Check Arc validity (empty Arc check)
            // @unsafe
            { Log_error("Invalid Future object passed to FutureGroup!"); }
            return;
        }
        futures_.push_back(std::move(f));
    }

    void wait_all() {
        for (auto& f : futures_) {
            f->wait();
        }
    }

    ~FutureGroup() {
        wait_all();
        // Arc auto-released when vector destroyed - no manual release needed
    }
};

// Type alias for Arc weak reference to ClientConnection
using WeakClientConnection = rusty::sync::Weak<ClientConnection>;

// @unsafe - Handles individual client connections to servers
// Similar to ServerConnection but for client-side connections
// SAFETY: Thread-safe with spinlocks, proper Arc lifetime management
class ClientConnection: public Pollable {
    friend class Client;
    friend class ClientPool;

    Marshal in_, out_;
    rusty::UnsafeCell<SpinLock> out_l_;  // Protects out_ (UnsafeCell for interior mutability)

    // Non-owning pointer to parent Client (for configuration access)
    Client* client_;

    // Shared reference to PollThread for async communication
    rusty::Arc<PollThread> poll_thread_worker_;

    int socket_;

    // Bookmark for request size (will be filled after marshaling)
    rusty::Option<rusty::Box<Marshal::bookmark>> bmark_;

    // Transaction ID counter for RPC requests
    Counter xid_counter_;

    // Map of pending futures awaiting responses (protected by SpinMutex)
    SpinMutex<std::unordered_map<i64, rusty::Arc<Future>>> pending_fu_{std::unordered_map<i64, rusty::Arc<Future>>()};

    enum {
        NEW, CONNECTED, CLOSED
    } status_;

    // Flag set by end_request() to indicate write mode update needed
    // Checked by poll loop after processing events (only used when on poll thread)
    // Cell provides interior mutability for safe access through const methods
    rusty::Cell<bool> pending_write_update_{false};

    // Weak pointer to self, initialized after creation
    // Used to pass weak reference for poll thread registration
    WeakClientConnection weak_self_;

    // Jetpack-specific members
    std::string host_;
    uint64_t packets_{0};
    bool paused_{false};
    bool is_client_mode_{false};  // Jetpack: distinguishes client vs server mode

    // @safe - Cancels all pending futures (has internal @unsafe blocks)
    // SAFETY: Protected by spinlock
    void invalidate_pending_futures();

    /**
     * Only to be called by:
     * 1: ~Client(), which is called when destroying Client
     * 2: handle_error(), which is called by PollThread
     */
    // @safe - Closes connection and cleans up (has internal @unsafe blocks)
    // SAFETY: Thread-safe cleanup sequence
    void close();

public:
    // Public destructor for Arc compatibility
    // @safe - Simple destructor
    ~ClientConnection();

    // @unsafe - Initializes connection
    // SAFETY: Stores references safely
    ClientConnection(Client* client, rusty::Arc<PollThread> poll_thread_worker);

    bool connected() const {
        return status_ == CONNECTED;
    }

    /**
     * Establish TCP connection to remote server.
     * Returns 0 on success, error code on failure.
     */
    // @unsafe - Establishes TCP connection
    // SAFETY: Proper socket creation and error handling
    int connect(const char* addr);

    /**
     * Start a new request. Must be paired with end_request().
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     * NOTE: size does not include the size itself (<xid>..<argN>).
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected)
     */
    // @unsafe - Begins RPC request with marshaling
    // SAFETY: Protected by spinlock, returns Arc<Future> for memory safety
    FutureResult begin_request(i32 rpc_id, const FutureAttr& attr = FutureAttr());

    // @unsafe - Completes request packet
    // SAFETY: Must be called after begin_request
    void end_request();

    // @safe - Marshals data into output buffer
    // @lifetime: (&'a, const T&) -> &'a
    template<class T>
    ClientConnection& operator <<(const T& v) {
        if (status_ == CONNECTED) {
            this->out_ << v;
        }
        return *this;
    }

    // NOTE: this function is used *internally* by Python extension
    // @safe - Marshals data from another Marshal
    // @lifetime: (&'a, Marshal&) -> &'a
    ClientConnection& operator <<(Marshal& m) {
        if (status_ == CONNECTED) {
            this->out_.read_from_marshal(m, m.content_size());
        }
        return *this;
    }

    int fd() const override {
        return socket_;
    }

    std::string host() const {
        return host_;
    }

    // Jetpack: pause/resume for flow control
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }

    // @safe - Returns poll mode based on output buffer
    int poll_mode() const override;

    // Jetpack: content_size helper
    size_t content_size() override {
        return in_.content_size();
    }

    // @safe - Writes buffered data to socket (has internal @unsafe blocks)
    // SAFETY: Protected by output spinlock
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write() override;

    // @unsafe - Reads and processes RPC responses
    // SAFETY: Protected by spinlock, validates packet structure
    bool handle_read() override;

    // Jetpack: split-phase read support
    bool handle_read_one() override;
    bool handle_read_two() override;

    // @safe - Error handler
    void handle_error() override;

    // @safe - Check and clear pending write update flag
    // Called by poll loop after processing events
    bool check_pending_write_update() const override {
        if (pending_write_update_.get()) {
            pending_write_update_.set(false);
            return true;
        }
        return false;
    }

    // Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid);

    // Comparison operator for container support
    friend bool operator==(const rusty::Arc<ClientConnection>& lhs, const rusty::Arc<ClientConnection>& rhs) {
        return lhs.get() == rhs.get();
    }

    // Hash function for containers
    friend struct std::hash<rusty::Arc<ClientConnection>>;
};

} // namespace rrr

// Hash specialization for rusty::Arc<ClientConnection>
namespace std {
template<>
struct hash<rusty::Arc<rrr::ClientConnection>> {
    size_t operator()(const rusty::Arc<rrr::ClientConnection>& arc) const {
        return hash<const rrr::ClientConnection*>()(arc.get());
    }
};
}

namespace rrr {

// @unsafe - RPC client facade that owns a ClientConnection
// SAFETY: Thread-safe through delegation to ClientConnection
// Client provides the user-facing API, ClientConnection handles socket I/O
// Similar to Server/ServerConnection pattern
class Client: public NoCopy {
    // The underlying connection that handles socket I/O
    // UnsafeCell for interior mutability (const methods need to delegate to connection)
    rusty::UnsafeCell<rusty::Option<rusty::Arc<ClientConnection>>> connection_;

    // Shared Arc to PollThread - used to create ClientConnection
    rusty::Arc<PollThread> poll_thread_worker_;

    // Jetpack-specific members using Cell for interior mutability of Copy types
    rusty::Cell<bool> is_client_mode_{false};
    rusty::Cell<long> time_{0};
    rusty::Cell<uint64_t> timeout_{0};
    rusty::Cell<i32> rpc_id_{0};

public:
    // Jetpack-specific public members (Cell for interior mutability through Arc)
    // These are accessed through getters/setters for thread-safety
    void set_client_mode(bool v) const { is_client_mode_.set(v); }
    bool client_mode() const { return is_client_mode_.get(); }
    void set_time(long v) const { time_.set(v); }
    long time() const { return time_.get(); }
    void set_timeout(uint64_t v) const { timeout_.set(v); }
    uint64_t timeout() const { return timeout_.get(); }
    void set_rpc_id(i32 v) const { rpc_id_.set(v); }
    i32 rpc_id() const { return rpc_id_.get(); }

    // @safe - Cleanup destructor (has internal @unsafe blocks)
    // SAFETY: Connection cleanup handled by ClientConnection
    virtual ~Client();

    Client(rusty::Arc<PollThread> poll_thread_worker):
        connection_(rusty::None),
        poll_thread_worker_(poll_thread_worker) { }

    // Factory method to create Client with Arc
    // @unsafe - Returns Arc<Client> with explicit reference counting
    // SAFETY: Arc provides thread-safe reference counting
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread_worker) {
        return rusty::Arc<Client>::make(poll_thread_worker);
    }

    /**
     * Start a new request. Must be paired with end_request().
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected)
     */
    // @unsafe - Begins RPC request with marshaling
    // SAFETY: Delegates to ClientConnection
    FutureResult begin_request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const;

    // @unsafe - Completes request packet
    // SAFETY: Must be called after begin_request
    void end_request() const;

    // @unsafe - Marshals data into output buffer
    // SAFETY: Delegates to ClientConnection
    // @lifetime: (&'a, const T&) -> &'a
    template<class T>
    const Client& operator <<(const T& v) const {
        if (connection_.get()->is_some() && connection_.get()->as_ref().unwrap()->connected()) {
            const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()) << v;
        }
        return *this;
    }

    // NOTE: this function is used *internally* by Python extension
    // @unsafe - Marshals data from another Marshal
    // SAFETY: Delegates to ClientConnection
    // @lifetime: (&'a, Marshal&) -> &'a
    const Client& operator <<(Marshal& m) const {
        if (connection_.get()->is_some() && connection_.get()->as_ref().unwrap()->connected()) {
            const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap()) << m;
        }
        return *this;
    }

    void set_valid(bool valid) const;
    // @unsafe - Establishes TCP connection
    // SAFETY: Creates ClientConnection and connects
    int connect(const char* addr, bool client = true) const;

    void pause() const;
    void resume() const;

    // reentrant, could be called multiple times
    // @safe - Closes socket and cleans up (has internal @unsafe blocks)
    // SAFETY: Idempotent, delegates to ClientConnection
    void close() const;

    // Jetpack compatibility wrapper
    void close_and_release() {
        close();
    }

    int fd() const {
        if (connection_.get()->is_some()) {
            return connection_.get()->as_ref().unwrap()->fd();
        }
        return -1;
    }

    std::string host() const {
        if (connection_.get()->is_some()) {
            return connection_.get()->as_ref().unwrap()->host();
        }
        return "";
    }

    bool connected() const {
        return connection_.get()->is_some() && connection_.get()->as_ref().unwrap()->connected();
    }

    // Get the underlying connection (for advanced use)
    // Returns a reference to the connection if it exists
    const rusty::Option<rusty::Arc<ClientConnection>>& connection() const {
        return *connection_.get();
    }

    // Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid) const;

};

// @safe - Thread-safe pool of client connections using Arc
// MIGRATED: Now uses rusty::Arc<Client> for cached connections
class ClientPool: public NoCopy {
    rrr::Rand rand_;

    // owns a shared reference to PollThread
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;

    // guard cache_
    SpinLock l_;
    // @safe - Uses rusty::Arc<Client> for thread-safe reference counting
    // SAFETY: Arc provides thread-safe reference counting with polymorphism support
    std::map<std::string, std::vector<rusty::Arc<Client>>> cache_;
    int parallel_connections_;

public:

    // @safe - Creates pool with optional PollThread
    // SAFETY: Shared ownership of PollThread
    ClientPool(rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker = rusty::None, int parallel_connections = 1);
    // @safe - Closes all cached connections
    // SAFETY: Properly releases all clients and PollThread via Arc
    ~ClientPool();

    // return cached client connection
    // on error, return None
    // @safe - Gets or creates client connection, returns Option<Arc<Client>>
    // SAFETY: Protected by spinlock, handles connection failures, Arc for thread-safe reference counting
    rusty::Option<rusty::Arc<rrr::Client>> get_client(const std::string& addr);

};

}
