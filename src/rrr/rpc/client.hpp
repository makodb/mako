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

// External safety annotations for system functions and STL operations
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Coroutine, and rusty-cpp types
// now have in-place annotations in their respective headers.
//
// SAFETY AUDIT: STL container operations are marked [safe] because:
// 1. All operations are used within SpinMutex lock guards (single-threaded access)
// 2. Iterators are not held across lock boundaries
// 3. No iterator invalidation occurs during iteration
//
// System functions (socket, etc.) remain [unsafe] as they involve I/O and raw pointers.
//
// @external: {
//   socket: [unsafe]
//   connect: [unsafe]
//   close: [unsafe]
//   setsockopt: [unsafe]
//   getaddrinfo: [unsafe]
//   freeaddrinfo: [unsafe]
//   gai_strerror: [unsafe]
//   memset: [unsafe]
//   strcpy: [unsafe]
//   std::lock_guard: [safe]
//   std::unique_lock: [safe]
//   std::chrono::duration: [safe]
//   std::function: [safe]
//   std::function::operator(): [safe]
//   std::unordered_map::find: [safe, (&'a, const K&) -> iterator where return: 'a]
//   std::unordered_map::end: [safe, (&'a) -> iterator]
//   std::unordered_map::begin: [safe, (&'a) -> iterator]
//   std::unordered_map::insert: [safe, (&'a mut, const K&, V) -> pair]
//   std::unordered_map::insert_or_assign: [safe, (&'a mut, const K&, V) -> pair]
//   std::unordered_map::operator[]: [safe, (&'a mut, const K&) -> V& where return: 'a]
//   std::unordered_map::erase: [safe, (&'a mut, iterator) -> iterator]
//   std::unordered_map::clear: [safe, (&'a mut) -> void]
//   std::vector::push_back: [safe, (&'a mut, const T&) -> void]
//   std::vector::empty: [safe, (&'a) -> bool]
//   std::vector::size: [safe, (&'a) -> size_t]
//   std::map::find: [safe, (&'a, const K&) -> iterator where return: 'a]
//   std::map::end: [safe, (&'a) -> iterator]
//   std::map::begin: [safe, (&'a) -> iterator]
//   std::map::insert: [safe, (&'a mut, const K&, V) -> pair]
//   std::map::operator[]: [safe, (&'a mut, const K&) -> V& where return: 'a]
//   operator!=: [safe]
//   operator==: [safe]
//   std::__detail::operator!=: [safe]
//   std::__detail::operator==: [safe]
//   const_cast: [unsafe]
//   rrr::Counter::next: [safe, (&'a mut) -> i64]
//   Log_error: [safe]
//   Log_debug: [safe]
//   Log_warn: [safe]
//   rrr::operator<<: [safe]
//   rrr::operator>>: [safe]
//   operator<<: [safe]
//   operator>>: [safe]
//   write_fn: [safe]
// }
// NOTE: Marshal methods (set_bookmark, write_bookmark, get_and_reset_write_cnt, empty, content_size)
// are now annotated @safe in-place in marshal.hpp

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
    rusty::Condvar ready_cond_;  // Uses interior mutability (const methods like Rust's &self)

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
        // @unsafe - Arc::make
        { return rusty::Arc<Future>::make(xid, attr); }
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

    // @safe - Simple getter
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

    // @safe - NO-OP: Arc automatically releases when dropped
    static inline void safe_release(rusty::Arc<Future> fu) {
        (void)fu;  // Intentionally empty - Arc handles cleanup
    }

    // @safe - NO-OP: Legacy overload for any remaining raw pointer usage in old code paths
    static inline void safe_release(Future* fu) {
        (void)fu;  // Intentionally empty - should not be called in new code
    }

    // @safe - NO-OP: Overload for FutureResult (Result<Arc<Future>, i32>) - jetpack compatibility
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
    // @safe - Adds future to group
    void add(rusty::Arc<Future> f) {
        if (!f) {  // Check Arc validity (empty Arc check)
            Log_error("Invalid Future object passed to FutureGroup!");
            return;
        }
        futures_.push_back(std::move(f));
    }

    // @safe - Waits for all futures in group
    void wait_all() {
        for (auto& f : futures_) {
            f->wait();
        }
    }

    // @safe - Destructor waits for futures, Arc auto-cleanup
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

    Marshal in_;
    SpinMutex<Marshal> out_;  // Lock + data combined (has interior mutability)

    // Non-owning pointer to parent Client (for configuration access)
    Client* client_;

    // Shared reference to PollThread for async communication
    rusty::Arc<PollThread> poll_thread_worker_;

    int socket_;

    // Transaction ID counter for RPC requests
    Counter xid_counter_;

    // Map of pending futures awaiting responses (protected by SpinMutex)
    SpinMutex<std::unordered_map<i64, rusty::Arc<Future>>> pending_fu_{std::unordered_map<i64, rusty::Arc<Future>>()};

    enum {
        NEW, CONNECTED, CLOSED
    } status_;

    // Flag set by request() to indicate write mode update needed
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

public:
    /**
     * Closes the connection and cleans up resources.
     * Called by:
     * 1: PollThreadWorker::do_close_pollable() for thread-safe close
     * 2: handle_error() for error handling
     */
    // @safe - Closes connection and cleans up (has internal @unsafe blocks)
    // SAFETY: Thread-safe cleanup sequence
    void close() override;

    // Public destructor for Arc compatibility
    // @safe - Simple destructor
    ~ClientConnection();

    // @safe - Initializes connection (only stores references)
    ClientConnection(Client* client, rusty::Arc<PollThread> poll_thread_worker);

    // @safe - Simple status check
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
     * Send an RPC request with a lambda for writing arguments.
     * Thread-safe: lock is acquired and released within this single call.
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     * NOTE: size does not include the size itself (<xid>..<argN>).
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected)
     */
    // @safe - Thread-safe RPC request with lambda for marshaling
    // All operations are now @safe:
    // - SpinMutex::lock() is @safe
    // - Counter::next is [safe] via external annotation
    // - STL map operations are [safe] via external annotations
    // - Marshal operations are @safe
    // - PollThreadWorker::is_on_poll_thread() is @safe
    // - PollThread::update_mode() is @safe
    // - Cell::set() is @safe
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) {
        if (status_ != CONNECTED) {
            return FutureResult::Err(ENOTCONN);
        }

        auto guard = out_.lock().unwrap();

        // Double-check connection status after acquiring lock
        if (status_ != CONNECTED) {
            return FutureResult::Err(ENOTCONN);
        }

        auto fu = Future::create(xid_counter_.next(), attr);

        {
            auto pending_guard = pending_fu_.lock().unwrap();
            pending_guard->insert_or_assign(fu->xid_, fu);
        }

        // Check if connection closed while we were setting up
        if (status_ != CONNECTED) {
            {
                auto pending_guard = pending_fu_.lock().unwrap();
                auto it = pending_guard->find(fu->xid_);
                if (it != pending_guard->end()) {
                    pending_guard->erase(it);
                }
            }
            return FutureResult::Err(ENOTCONN);
        }

        // Set bookmark for packet size (will fill after marshaling)
        Marshal::bookmark bmark = guard->set_bookmark(sizeof(i32));

        *guard << v64(fu->xid_);
        *guard << rpc_id;

        // Call user's write function to marshal arguments
        write_fn(*guard);

        // Fill in the packet size
        i32 request_size = guard->get_and_reset_write_cnt();
        guard->write_bookmark(bmark, request_size);

        // Reset Jetpack flags
        guard->found_dep = false;
        guard->valid_id = false;

        // Signal that we have data to write
        if (PollThreadWorker::is_on_poll_thread()) {
            pending_write_update_.set(true);
        } else {
            poll_thread_worker_->update_mode(*this, Pollable::READ | Pollable::WRITE);
        }

        return FutureResult::Ok(fu);
    }

    // @safe - Convenience overload without callback
    // SAFETY: Internal @unsafe block for template overload call
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) {
        // @unsafe - calls main request overload
        { return request(rpc_id, FutureAttr(), std::forward<F>(write_fn)); }
    }

    // @safe - Convenience overload for requests with no arguments
    // SAFETY: Internal @unsafe block for template overload call
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) {
        // @unsafe - calls main request overload
        { return request(rpc_id, attr, [](Marshal&) {}); }
    }

    // @safe - Simple getter
    int fd() const override {
        return socket_;
    }

    // @safe - Simple getter
    std::string host() const {
        // @unsafe - string copy constructor
        { return host_; }
    }

    // @safe - Jetpack: pause/resume for flow control
    void pause() { paused_ = true; }
    // @safe
    void resume() { paused_ = false; }

    // @safe - Returns poll mode based on output buffer
    int poll_mode() const override;

    // @safe - Jetpack: content_size helper
    size_t content_size() override {
        return in_.content_size();
    }

    // @safe - Writes buffered data to socket (has internal @unsafe blocks)
    // SAFETY: Protected by output spinlock
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write() override;

    // @unsafe - Reads and processes RPC responses
    // SAFETY: Contains raw pointer operations (GetReactor()->Loop())
    bool handle_read() override;

    // @unsafe - Jetpack: split-phase read support (I/O operations)
    bool handle_read_one() override;
    // @unsafe
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

    // @safe - Check if connection was closed (via handle_error)
    // Called by poll loop to detect and remove closed connections
    bool is_closed() const override {
        return status_ == CLOSED;
    }

    // @safe - Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid);

    // Comparison operator for container support
    friend bool operator==(const rusty::Arc<ClientConnection>& lhs, const rusty::Arc<ClientConnection>& rhs) {
        return lhs.get() == rhs.get();
    }

    // Hash function for containers
    friend struct std::hash<rusty::Arc<ClientConnection>>;
};

} // namespace rrr

// @safe - Hash specialization for rusty::Arc<ClientConnection>
namespace std {
template<>
struct hash<rusty::Arc<rrr::ClientConnection>> {
    // @safe - Simple pointer hash
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
    // @safe - Jetpack-specific public members (Cell for interior mutability through Arc)
    // These are accessed through getters/setters for thread-safety
    void set_client_mode(bool v) const { is_client_mode_.set(v); }
    // @safe
    bool client_mode() const { return is_client_mode_.get(); }
    // @safe
    void set_time(long v) const { time_.set(v); }
    // @safe
    long time() const { return time_.get(); }
    // @safe
    void set_timeout(uint64_t v) const { timeout_.set(v); }
    // @safe
    uint64_t timeout() const { return timeout_.get(); }
    // @safe
    void set_rpc_id(i32 v) const { rpc_id_.set(v); }
    // @safe
    i32 rpc_id() const { return rpc_id_.get(); }

    // @safe - Cleanup destructor (has internal @unsafe blocks)
    // SAFETY: Connection cleanup handled by ClientConnection
    virtual ~Client();

    // @safe - Simple initialization
    Client(rusty::Arc<PollThread> poll_thread_worker):
        connection_(rusty::None),
        poll_thread_worker_(poll_thread_worker) { }

    // Factory method to create Client with Arc
    // @safe - Returns Arc<Client>
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread_worker) {
        // @unsafe - Arc::make
        { return rusty::Arc<Client>::make(poll_thread_worker); }
    }

    /**
     * Send an RPC request with a lambda for writing arguments.
     * Thread-safe: lock is acquired and released within this single call.
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected)
     */
    // @safe - Thread-safe RPC request with lambda for marshaling
    // SAFETY: Internal @unsafe block for UnsafeCell access and const_cast
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        // @unsafe - UnsafeCell::get() and const_cast for interior mutability
        {
            if (connection_.get()->is_none()) {
                return FutureResult::Err(ENOTCONN);
            }
            rpc_id_.set(rpc_id);
            return const_cast<ClientConnection&>(*connection_.get()->as_ref().unwrap())
                .request(rpc_id, attr, std::forward<F>(write_fn));
        }
    }

    // @safe - Convenience overload without callback
    // SAFETY: Internal @unsafe block for template overload call
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        // @unsafe - calls main request overload
        { return request(rpc_id, FutureAttr(), std::forward<F>(write_fn)); }
    }

    // @safe - Convenience overload for requests with no arguments
    // SAFETY: Internal @unsafe block for template overload call
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        // @unsafe - calls main request overload
        { return request(rpc_id, attr, [](Marshal&) {}); }
    }

    // @unsafe - Uses const_cast for interior mutability
    // SAFETY: Contains raw pointer dereference
    void set_valid(bool valid) const;
    // @unsafe - Establishes TCP connection (socket system calls)
    int connect(const char* addr, bool client = true) const;

    // @unsafe - Uses const_cast for interior mutability
    // SAFETY: Contains raw pointer cast
    void pause() const;
    // @unsafe - Uses const_cast for interior mutability
    // SAFETY: Contains raw pointer cast
    void resume() const;

    // reentrant, could be called multiple times
    // @unsafe - Closes socket and cleans up
    // Uses UnsafeCell::get() for interior mutability
    void close() const;

    // @unsafe - Jetpack compatibility wrapper
    void close_and_release() {
        close();
    }

    // @unsafe - Accesses UnsafeCell
    // SAFETY: Contains raw pointer dereference in return
    int fd() const {
        if (connection_.get()->is_some()) {
            return connection_.get()->as_ref().unwrap()->fd();
        }
        return -1;
    }

    // @unsafe - Accesses UnsafeCell
    // SAFETY: Contains raw pointer dereference in return
    std::string host() const {
        if (connection_.get()->is_some()) {
            return connection_.get()->as_ref().unwrap()->host();
        }
        return "";
    }

    // @unsafe - Accesses UnsafeCell
    // SAFETY: Contains raw pointer dereference in return
    bool connected() const {
        return connection_.get()->is_some() && connection_.get()->as_ref().unwrap()->connected();
    }

    // @unsafe - Get the underlying connection
    // SAFETY: Contains raw pointer dereference
    const rusty::Option<rusty::Arc<ClientConnection>>& connection() const {
        return *connection_.get();
    }

    // @unsafe - Jetpack: handle_free for explicit future cleanup
    // SAFETY: Contains raw pointer cast
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
    ClientPool(rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker = rusty::None, int parallel_connections = 1);
    // @safe - Closes all cached connections
    ~ClientPool();

    // return cached client connection
    // on error, return None
    // @unsafe - Gets or creates client connection
    // SAFETY: Contains raw pointer dereference
    rusty::Option<rusty::Arc<rrr::Client>> get_client(const std::string& addr);

};

}
