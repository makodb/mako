#pragma once
#include <rusty/rusty.hpp>
#include <rusty/result.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>

#include <unordered_map>
#include <chrono>
#include <mutex>

#include "misc/marshal.hpp"
#include "reactor/epoll_wrapper.h"
#include "reactor/reactor.h"
#include "connection_state.hpp"
#include "reconnect_policy.hpp"
#include "request_queue.hpp"
#include "connection_metrics.hpp"
#include "request_options.hpp"
#include "load_balancer.hpp"

namespace rrr {

// Stream operator for RefMut<Marshal> - allows get_reply() >> x pattern
// This forwards to Marshal's operator>> while caller holds the guard
template<typename U>
Marshal& operator>>(rusty::RefMut<Marshal>& guard, U& value) {
    return *guard >> value;
}

template<typename U>
Marshal& operator>>(rusty::RefMut<Marshal>&& guard, U& value) {
    return *guard >> value;
}

} // namespace rrr

// External safety annotations for system functions and STL operations
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Fiber, and rusty-cpp types
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
//   rrr::Future::create: [safe]
//   rrr::ClientConnection::request: [safe]
//   rrr::Client::request: [safe]
//   std::forward: [safe]
//   std::__cxx11::basic_string::basic_string: [safe]
// }
// NOTE: Marshal methods (set_bookmark, write_bookmark, get_and_reset_write_cnt, empty, content_size)
// are now annotated @safe in-place in marshal.hpp

namespace rrr {

/**
 * Behavior when a request is made while disconnected.
 */
enum class DisconnectBehavior {
    QUEUE,      // Queue requests for later replay (default)
    FAIL_FAST   // Immediately fail with ENOTCONN
};

/**
 * Configuration for request buffering during disconnection.
 */
struct BufferingConfig {
    DisconnectBehavior behavior = DisconnectBehavior::QUEUE;
    size_t max_pending = 1000;           // Max queued requests
    uint32_t default_ttl_ms = 30000;     // 30 second TTL
    OverflowStrategy overflow = OverflowStrategy::DROP_OLDEST;
    bool enabled = true;

    // @unsafe - Returns struct by value
    static BufferingConfig defaults() {
        // @unsafe { struct construction }
        return BufferingConfig{};
    }

    // @unsafe - Returns struct by value
    static BufferingConfig disabled() {
        // @unsafe { struct construction }
        BufferingConfig config;
        config.enabled = false;
        config.behavior = DisconnectBehavior::FAIL_FAST;
        return config;
    }

    // @unsafe - Returns struct by value
    RequestQueueConfig to_queue_config() const {
        // @unsafe { struct construction }
        RequestQueueConfig qc;
        qc.max_size = max_pending;
        qc.default_ttl_ms = default_ttl_ms;
        qc.overflow_strategy = overflow;
        qc.enabled = enabled;
        return qc;
    }
};

/**
 * TCP Keepalive configuration for connection health monitoring.
 *
 * Configures OS-level TCP keepalive probes to detect dead connections.
 * When enabled, the OS will send keepalive probes after the connection
 * has been idle for `idle_sec` seconds, then at `interval_sec` intervals.
 * If `count` probes go unanswered, the connection is considered dead.
 */
// @safe - POD config struct for TCP keepalive settings
struct KeepaliveConfig {
    bool enabled = true;       // Enable TCP keepalive probes
    int idle_sec = 60;         // TCP_KEEPIDLE: seconds before first probe
    int interval_sec = 10;     // TCP_KEEPINTVL: seconds between probes
    int count = 5;             // TCP_KEEPCNT: max unanswered probes before dropping

    // @safe - Default constructor
    KeepaliveConfig() = default;

    // @safe - Copy constructor
    KeepaliveConfig(const KeepaliveConfig&) = default;

    // @safe - Copy assignment
    KeepaliveConfig& operator=(const KeepaliveConfig&) = default;

    // Fast detection preset: 10s idle, 2s interval, 3 probes = 16s to detect failure
    // @safe - POD struct copy is memory-safe
    static KeepaliveConfig aggressive() {
        KeepaliveConfig config;
        config.enabled = true;
        config.idle_sec = 10;
        config.interval_sec = 2;
        config.count = 3;
        return config;
    }

    // Standard preset: 60s idle, 10s interval, 5 probes = 110s to detect failure
    // @safe - POD struct copy is memory-safe
    static KeepaliveConfig relaxed() {
        KeepaliveConfig config;
        config.enabled = true;
        config.idle_sec = 60;
        config.interval_sec = 10;
        config.count = 5;
        return config;
    }

    // Disabled preset
    // @safe - POD struct copy is memory-safe
    static KeepaliveConfig disabled() {
        KeepaliveConfig config;
        config.enabled = false;
        config.idle_sec = 0;
        config.interval_sec = 0;
        config.count = 0;
        return config;
    }
};

/**
 * ClientPool configuration for health-aware connection pooling.
 *
 * Controls connection limits, health checking, and idle timeout behavior.
 */
// @safe - POD config struct for pool settings
struct PoolConfig {
    int min_connections = 1;             // Minimum connections per address
    int max_connections = 4;             // Maximum connections per address
    uint64_t idle_timeout_ms = 300000;   // 5 minutes default (0 = no timeout)
    bool health_check_enabled = true;    // Enable health-based removal
    uint64_t unhealthy_threshold_percent = 50;  // Remove if success rate < this %
    uint64_t min_requests_for_health = 10;      // Min requests before health check
    LoadBalancingStrategy load_balancing = LoadBalancingStrategy::RANDOM;  // Selection strategy

    // @safe - Default constructor
    PoolConfig() = default;

    // @safe - Copy constructor
    PoolConfig(const PoolConfig&) = default;

    // @safe - Copy assignment
    PoolConfig& operator=(const PoolConfig&) = default;

    // Default preset: balanced settings
    // @safe - POD struct copy is memory-safe
    static PoolConfig defaults() {
        return PoolConfig();
    }

    // Aggressive preset: more connections, shorter timeout, stricter health
    // @safe - POD struct copy is memory-safe
    static PoolConfig aggressive() {
        PoolConfig config;
        config.min_connections = 2;
        config.max_connections = 8;
        config.idle_timeout_ms = 60000;  // 1 minute
        config.health_check_enabled = true;
        config.unhealthy_threshold_percent = 70;  // Stricter
        config.min_requests_for_health = 5;
        return config;
    }

    // Conservative preset: fewer connections, longer timeout, lenient health
    // @safe - POD struct copy is memory-safe
    static PoolConfig conservative() {
        PoolConfig config;
        config.min_connections = 1;
        config.max_connections = 2;
        config.idle_timeout_ms = 600000;  // 10 minutes
        config.health_check_enabled = true;
        config.unhealthy_threshold_percent = 30;  // More lenient
        config.min_requests_for_health = 20;
        return config;
    }

    // Disabled health checking preset
    // @safe - POD struct copy is memory-safe
    static PoolConfig no_health_check() {
        PoolConfig config;
        config.health_check_enabled = false;
        return config;
    }
};

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

// @safe - Thread-safe future for async RPC results
// Uses rusty::Arc for memory safety, RefCell/Cell for interior mutability
// MIGRATED: Now uses rusty::Arc<Future> instead of RefCounted for memory safety
class Future {
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
    rusty::RefCell<Marshal> reply_;  // RefCell for interior mutability with runtime borrow checking

    uint64_t timeout_{1000000}; // default timeout 1s (jetpack)
    rusty::Mutex<State> state_;  // Mutex protects State (ready/timed_out flags)
    rusty::Condvar ready_cond_;  // Uses interior mutability (const methods like Rust's &self)

    // Phase 2.4: Retry support
    rusty::Cell<RequestOptions> options_;     // Request options (timeout, retry config)
    rusty::Cell<TimeoutType> timeout_type_{TimeoutType::NONE};  // Type of timeout that occurred
    rusty::Cell<uint16_t> retry_count_{0};    // Number of retries attempted

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
    // Arc::make is safe - it just allocates and constructs
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

    // @unsafe - rusty-cpp false positive: sec IS initialized
    // Wait using configured options timeout
    // Returns true if ready, false if timed out
    bool wait_with_options() const {
        auto opts = options_.get();
        if (opts.timeout_ms == 0) {
            wait();  // No timeout
            return ready();
        }
        double sec = static_cast<double>(opts.timeout_ms) / 1000.0;
        timed_wait(sec);
        return ready() && !timed_out();
    }

    // @safe - Uses rusty::Mutex
    bool timed_out() const {
        auto guard = state_.lock().unwrap();
        return (*guard).timed_out;
    }

    // @safe - Returns guard for reply (Rust-idiomatic lifetime safety)
    // Caller holds the guard, ensuring the reference can't outlive it
    rusty::RefMut<Marshal> get_reply() const {
        wait();
        return reply_.borrow_mut();
    }

    // @safe - Calls wait methods, uses @unsafe for timed_wait which uses std::chrono
    i32 get_error_code() const {
        if (timeout_ > 0) {
            double x = timeout_;
            x = x / 1000000;
            // @unsafe
            { timed_wait(x); }
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
    // Phase 2.4: Retry Support Accessors
    // =========================================================================

    // @safe - Get request options
    RequestOptions get_options() const {
        return options_.get();
    }

    // @safe - Set request options
    void set_options(const RequestOptions& opts) {
        options_.set(opts);
    }

    // @safe - Get timeout type that occurred
    TimeoutType get_timeout_type() const {
        return timeout_type_.get();
    }

    // @safe - Set timeout type
    void set_timeout_type(TimeoutType type) {
        timeout_type_.set(type);
    }

    // @safe - Get current retry count
    uint16_t get_retry_count() const {
        return retry_count_.get();
    }

    // @safe - Increment retry count and return new value
    uint16_t increment_retry_count() {
        uint16_t current = retry_count_.get();
        retry_count_.set(current + 1);
        return current + 1;
    }

    // @safe - Check if should retry based on options and current state
    bool should_retry() const {
        auto opts = options_.get();
        return opts.can_retry(retry_count_.get());
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

// @unsafe - Inherits from @interface Pollable (rusty-cpp namespace resolution bug workaround)
// Similar to ServerConnection but for client-side connections
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership
// Note: connect() and handle_read() contain @unsafe blocks for socket I/O
class ClientConnection: public Pollable {
    friend class Client;
    friend class ClientPool;

    Marshal in_;
    SpinMutex<Marshal> out_;  // Lock + data combined (has interior mutability)

    // Shared reference to PollThread for async communication
    rusty::Arc<PollThread> poll_thread_worker_;

    int socket_;

    // Transaction ID counter for RPC requests
    // mutable because Counter uses atomics internally for thread-safe interior mutability
    mutable Counter xid_counter_;

    // Map of pending futures awaiting responses (protected by SpinMutex)
    SpinMutex<std::unordered_map<i64, rusty::Arc<Future>>> pending_fu_{std::unordered_map<i64, rusty::Arc<Future>>()};

    // Connection state machine for lifecycle management
    ConnectionStateMachine state_machine_;

    // Reconnection policy and state
    ReconnectPolicy reconnect_policy_;
    rusty::Cell<bool> reconnecting_{false};
    std::string reconnect_address_;  // Address to reconnect to

    // Request buffering during disconnection
    mutable BufferingConfig buffering_config_;  // mutable for const set_buffering_config()
    mutable RequestQueue pending_queue_;  // mutable for const request() access

    // Server restart detection: tracks server instance ID
    // 0 means no ID received yet (initial state)
    rusty::Cell<uint64_t> server_instance_id_{0};

    // Callback invoked when server restart is detected (ID changes)
    // Parameters: (old_id, new_id)
    mutable std::function<void(uint64_t, uint64_t)> on_server_restart_;

    // TCP Keepalive configuration for connection health monitoring (Cell for interior mutability)
    rusty::Cell<KeepaliveConfig> keepalive_config_;

    // Last activity timestamp for idle detection (milliseconds since epoch)
    // Updated on send/receive operations
    rusty::Cell<uint64_t> last_activity_time_{0};

    // Connection health metrics
    ConnectionMetrics metrics_;

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
    rusty::Cell<bool> paused_{false};  // Cell for interior mutability
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
    // @safe - Closes connection and cleans up
    // SAFETY: Thread-safe cleanup sequence
    void close() override;

    /**
     * Mark connection as closing without closing the socket.
     * Used by Client::close() to update state before poll thread closes socket.
     * This avoids race conditions with pending CmdAddPollable commands.
     */
    // @safe - Just updates state machine
    void mark_closing();

    // Public destructor for Arc compatibility
    // @safe - Simple destructor
    ~ClientConnection();

    // @safe - Initializes connection (only stores references)
    ClientConnection(rusty::Arc<PollThread> poll_thread_worker);

    // @safe - Simple status check using state machine
    bool connected() const {
        return state_machine_.is_connected();
    }

    // @safe - Get current connection state
    ConnectionState connection_state() const {
        return state_machine_.state();
    }

    /**
     * Establish TCP connection to remote server.
     * Returns 0 on success, error code on failure.
     */
    // @unsafe - Establishes TCP connection
    // SAFETY: Proper socket creation and error handling
    int connect(const char* addr);

    /**
     * Set the reconnection policy for this connection.
     * The policy controls automatic reconnection behavior after failures.
     */
    // @safe - Sets reconnection policy (wraps operator= in @unsafe block)
    void set_reconnect_policy(const ReconnectPolicy& policy) {
        // @unsafe - struct assignment operator not annotated
        { reconnect_policy_ = policy; }
    }

    // @safe - Get the current reconnection policy
    // @lifetime: (&'a) -> &'a
    const ReconnectPolicy& reconnect_policy() const {
        return reconnect_policy_;
    }

    // @safe - Check if a reconnection attempt is in progress
    bool is_reconnecting() const {
        return reconnecting_.get();
    }

    /**
     * Attempt to reconnect to the last connected address.
     * Can only be called when connection is in FAILED or DISCONNECTED state.
     *
     * @param on_complete Optional callback called with success/failure result
     * @return 0 on success (reconnection started), error code on failure
     */
    // @unsafe - Attempts reconnection (calls connect which has socket operations)
    int reconnect(std::function<void(bool)> on_complete = nullptr);

    /**
     * Set the buffering configuration for this connection.
     * Controls whether requests are queued during disconnection.
     * Note: const because internal state is mutable.
     */
    // @safe - Sets buffering configuration
    void set_buffering_config(const BufferingConfig& config) const;

    // @safe - Get the current buffering configuration
    // @lifetime: (&'a) -> &'a
    const BufferingConfig& buffering_config() const {
        return buffering_config_;
    }

    // @unsafe - Uses RequestQueue which uses std::list
    size_t pending_request_count() const {
        // @unsafe { RequestQueue::size }
        return pending_queue_.size();
    }

    // @unsafe - Uses SpinMutex + std::unordered_map access
    size_t pending_future_count() const {
        auto pending_guard = pending_fu_.lock().unwrap();
        return pending_guard->size();
    }

    // @unsafe - Uses RequestQueue which uses std::list
    // Note: const because pending_queue_ is mutable
    void clear_pending_requests(int error_code = ECONNABORTED) const {
        // @unsafe { RequestQueue::clear_all }
        pending_queue_.clear_all(error_code);
    }

    // === Server Restart Detection API ===

    /**
     * Get the last known server instance ID.
     * Returns 0 if no ID has been set yet (initial state).
     */
    // @safe - Simple getter using Cell
    uint64_t server_instance_id() const {
        return server_instance_id_.get();
    }

    /**
     * Set the callback to be invoked when server restart is detected.
     * The callback receives (old_id, new_id) when the server's instance ID changes.
     *
     * @param callback Function to call on restart detection
     */
    // @unsafe - std::function assignment through const (interior mutability via mutable)
    void set_on_server_restart(std::function<void(uint64_t, uint64_t)> callback) const {
        on_server_restart_ = std::move(callback);
    }

    /**
     * Check and update the server instance ID.
     * If the new ID differs from the stored ID (and stored ID was non-zero),
     * the on_server_restart callback is invoked.
     *
     * @param new_id The new server instance ID
     * @return true if server restart was detected, false otherwise
     */
    // @unsafe - Updates Cell and may call callback (std::function operations)
    bool check_server_instance(uint64_t new_id) const {
        uint64_t old_id = server_instance_id_.get();

        // Always update the stored ID
        server_instance_id_.set(new_id);

        // Detect restart: old ID was set (non-zero) and differs from new ID
        if (old_id != 0 && old_id != new_id) {
            Log_info("Server restart detected: old_id=%lu new_id=%lu", old_id, new_id);
            // @unsafe { std::function::operator bool and callback execution }
            if (on_server_restart_) {
                on_server_restart_(old_id, new_id);
            }
            return true;
        }
        return false;
    }

    // === Connection Validation API ===

    /**
     * Configure TCP keepalive for the connection.
     * Should be called before connect() or after reconnection.
     *
     * @param config The keepalive configuration to apply
     */
    // @safe - Uses Cell for interior mutability
    void set_keepalive(const KeepaliveConfig& config) const {
        keepalive_config_.set(config);
    }

    /**
     * Get the current keepalive configuration.
     */
    // @safe - Returns copy via Cell::get()
    KeepaliveConfig keepalive_config() const {
        return keepalive_config_.get();
    }

    /**
     * Update the last activity timestamp.
     * Called on send/receive operations to track connection activity.
     *
     * @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
     */
    // @safe - Stores timestamp in Cell
    void update_last_activity(uint64_t current_time_ms) const {
        last_activity_time_.set(current_time_ms);
    }

    /**
     * Get the last activity timestamp (milliseconds since epoch).
     */
    // @safe - Simple getter using Cell
    uint64_t last_activity_time() const {
        return last_activity_time_.get();
    }

    /**
     * Check if the connection is idle (no activity for idle_ms milliseconds).
     *
     * @param idle_ms Idle threshold in milliseconds
     * @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
     * @return true if idle for longer than threshold, false otherwise
     */
    // @safe - Simple timestamp comparison
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
        auto last = last_activity_time_.get();
        if (last == 0) return false;  // No activity recorded yet
        return (current_time_ms - last) > idle_ms;
    }

    /**
     * Validate the connection is still alive and healthy.
     *
     * Checks:
     * 1. Connection state is CONNECTED
     * 2. Socket is valid (fd >= 0)
     * 3. Socket has no pending errors (getsockopt SO_ERROR)
     *
     * @return true if connection is healthy, false if it needs reconnection
     */
    // @unsafe - Uses getsockopt system call
    bool validate_connection() const;

    /**
     * Get the connection metrics.
     * Returns a const reference to the metrics for this connection.
     */
    // @safe - Simple getter
    // @lifetime: (&'a) -> &'a
    const ConnectionMetrics& metrics() const {
        return metrics_;
    }

private:
    // @safe - Replay queued requests after reconnection
    // Returns number of requests replayed
    size_t replay_pending_requests();

    // @unsafe - Apply keepalive options to socket
    // Called after socket creation in connect()
    void apply_keepalive_options();

public:

    /**
     * Send an RPC request with a lambda for writing arguments.
     * Thread-safe: lock is acquired and released within this single call.
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     * NOTE: size does not include the size itself (<xid>..<argN>).
     *
     * If disconnected and buffering is enabled (QUEUE behavior), the request
     * is queued for replay after reconnection. The returned Future will be
     * completed when the request is eventually sent and a response received.
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success (or queued for later)
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected and buffering disabled)
     */
    // @unsafe - Thread-safe RPC request with lambda for marshaling
    // Contains multiple operations requiring unsafe context:
    // - Counter::next (atomic but not annotated)
    // - STL map operations (well-defined but not annotated)
    // - Marshal operator<< (serialization)
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        // Check connection status - if not connected, handle buffering
        if (!state_machine_.is_connected()) {
            // Check if buffering is enabled with QUEUE behavior
            if (buffering_config_.enabled &&
                buffering_config_.behavior == DisconnectBehavior::QUEUE) {
                // Queue the request for later replay
                return queue_request(rpc_id, attr, std::forward<F>(write_fn));
            }
            return FutureResult::Err(ENOTCONN);
        }

        auto guard = out_.lock().unwrap();

        // Double-check connection status after acquiring lock
        if (!state_machine_.is_connected()) {
            // Check if buffering is enabled with QUEUE behavior
            if (buffering_config_.enabled &&
                buffering_config_.behavior == DisconnectBehavior::QUEUE) {
                return queue_request(rpc_id, attr, std::forward<F>(write_fn));
            }
            return FutureResult::Err(ENOTCONN);
        }

        auto fu = Future::create(xid_counter_.next(), attr);

        {
            auto pending_guard = pending_fu_.lock().unwrap();
            pending_guard->insert_or_assign(fu->xid_, fu);
        }

        // Check if connection closed while we were setting up
        if (!state_machine_.is_connected()) {
            {
                auto pending_guard = pending_fu_.lock().unwrap();
                auto it = pending_guard->find(fu->xid_);
                if (it != pending_guard->end()) {
                    pending_guard->erase(it);
                }
            }
            // Check if buffering is enabled with QUEUE behavior
            if (buffering_config_.enabled &&
                buffering_config_.behavior == DisconnectBehavior::QUEUE) {
                return queue_request(rpc_id, attr, std::forward<F>(write_fn));
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
            poll_thread_worker_->update_mode(*this, PollMode::READ | PollMode::WRITE);
        }

        // Record request sent in metrics
        metrics_.record_request_sent();

        return FutureResult::Ok(fu);
    }

private:
    // @unsafe - Queue request for later replay (called when disconnected)
    // Uses Counter::next, Marshal operators, and RequestQueue
    template<typename F>
    FutureResult queue_request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        // @unsafe { Counter::next }
        auto fu = Future::create(xid_counter_.next(), attr);

        // Create queued request with serialized payload
        QueuedRequest queued;
        queued.xid = fu->xid_;
        queued.rpc_id = rpc_id;
        queued.ttl_ms = buffering_config_.default_ttl_ms;
        queued.payload = std::make_shared<Marshal>();

        // Serialize request to payload (including size placeholder)
        Marshal::bookmark bmark = queued.payload->set_bookmark(sizeof(i32));
        // @unsafe { Marshal operators }
        *queued.payload << v64(queued.xid);
        *queued.payload << rpc_id;
        write_fn(*queued.payload);  // User writes arguments

        // Fill in packet size (not counting the size field itself)
        i32 request_size = queued.payload->get_and_reset_write_cnt();
        queued.payload->write_bookmark(bmark, request_size);

        // Store future in pending map (for response handling)
        {
            auto pending_guard = pending_fu_.lock().unwrap();
            pending_guard->insert_or_assign(fu->xid_, fu);
        }

        // Set callback to notify future on queue failure (e.g., overflow, expiry)
        auto weak_fu = fu;  // Copy Arc for callback
        queued.callback = [this, weak_fu](int err) {
            if (err != 0) {
                // Remove from pending map
                {
                    auto pending_guard = pending_fu_.lock().unwrap();
                    auto it = pending_guard->find(weak_fu->xid_);
                    if (it != pending_guard->end()) {
                        pending_guard->erase(it);
                    }
                }
                // Notify future with error
                weak_fu->error_code_.set(err);
                weak_fu->notify_ready(weak_fu);
            }
        };

        // Try to enqueue
        if (!pending_queue_.enqueue(std::move(queued))) {
            // Queue rejected (e.g. DROP_NEWEST/FULL). Ensure we do not leak an
            // internal pending future even if enqueue() did not invoke callback.
            if (!fu->ready()) {
                {
                    auto pending_guard = pending_fu_.lock().unwrap();
                    auto it = pending_guard->find(fu->xid_);
                    if (it != pending_guard->end()) {
                        pending_guard->erase(it);
                    }
                }
                fu->error_code_.set(EAGAIN);
                fu->notify_ready(fu);
            }
            return FutureResult::Err(EAGAIN);
        }

        return FutureResult::Ok(fu);
    }

public:

    // @unsafe - Convenience overload without callback (calls @unsafe request)
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        return request(rpc_id, FutureAttr(), std::forward<F>(write_fn));
    }

    // @unsafe - Convenience overload for requests with no arguments (calls @unsafe request)
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        return request(rpc_id, attr, [](Marshal&) {});
    }

    // =========================================================================
    // Phase 2.4: Request with Options (Timeout/Retry Support)
    // =========================================================================

    /**
     * Send an RPC request with explicit options for timeout and retry.
     * Sets the options on the returned Future for use with wait_with_options().
     *
     * @param rpc_id The RPC method ID
     * @param options Request options (timeout, retry config)
     * @param attr Future attributes (callback, etc.)
     * @param write_fn Lambda to write request arguments
     * @return Result<Arc<Future>, i32>
     */
    // @unsafe - Same as request(), plus sets options
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const {
        auto result = request(rpc_id, attr, std::forward<F>(write_fn));
        if (result.is_ok()) {
            result.ok()->set_options(options);
        }
        return result;
    }

    // @unsafe - Convenience overload without FutureAttr
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      F&& write_fn) const {
        return request_with_options(rpc_id, options, FutureAttr(), std::forward<F>(write_fn));
    }

    // @safe - Returns file descriptor
    int fd() const override {
        return socket_;
    }

    // @safe - Simple getter (string copy is safe)
    std::string host() const {
        // @unsafe
        { return host_; }
    }

    // @safe - Jetpack: pause/resume for flow control (Cell for interior mutability)
    void pause() const { paused_.set(true); }
    // @safe
    void resume() const { paused_.set(false); }

    // @safe - Returns poll mode based on output buffer
    int poll_mode() const override;

    // @safe - Jetpack: content_size helper
    size_t content_size() override {
        return in_.content_size();
    }

    // @safe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write() override;

    // @safe - Reads and processes RPC responses
    bool handle_read() override;

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

    // @safe - Check if connection was closed
    // Called by poll loop to detect and remove closed connections
    bool is_closed() const override {
        return state_machine_.is_terminal();
    }

    // @safe - Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid) const;

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
// (Marked unsafe due to mutable field for interior mutability)
// Thread-safe through delegation to ClientConnection
// Client provides the user-facing API, ClientConnection handles socket I/O
// Similar to Server/ServerConnection pattern
class Client {
    // The underlying connection that handles socket I/O
    // RefCell for interior mutability (const methods need to delegate to connection)
    mutable rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>> connection_;

    // Shared Arc to PollThread - used to create ClientConnection
    rusty::Arc<PollThread> poll_thread_worker_;

    // Jetpack-specific members using Cell for interior mutability of Copy types
    rusty::Cell<bool> is_client_mode_{false};
    rusty::Cell<long> time_{0};
    rusty::Cell<uint64_t> timeout_{0};
    rusty::Cell<i32> rpc_id_{0};

    // Pending keepalive config (applied when connection is created) - Cell for interior mutability
    rusty::Cell<KeepaliveConfig> pending_keepalive_config_;

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
    // Arc::make is safe - it just allocates and constructs
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread_worker) {
        return rusty::Arc<Client>::make(poll_thread_worker);
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
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return FutureResult::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        // @unsafe
        { return guard->as_ref().unwrap()->request(rpc_id, attr, std::forward<F>(write_fn)); }
    }

    // @safe - Convenience overload without callback
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        // @unsafe
        { return request(rpc_id, FutureAttr(), std::forward<F>(write_fn)); }
    }

    // @safe - Convenience overload for requests with no arguments
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        // @unsafe
        { return request(rpc_id, attr, [](Marshal&) {}); }
    }

    // =========================================================================
    // Phase 2.4: Request with Options (Timeout/Retry Support)
    // =========================================================================

    /**
     * Send an RPC request with explicit options for timeout and retry.
     * Sets the options on the returned Future for use with wait_with_options().
     */
    // @safe - Thread-safe RPC request with options
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return FutureResult::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        // @unsafe
        { return guard->as_ref().unwrap()->request_with_options(
            rpc_id, options, attr, std::forward<F>(write_fn)); }
    }

    // @safe - Convenience overload without FutureAttr
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      F&& write_fn) const {
        // @unsafe
        { return request_with_options(rpc_id, options, FutureAttr(), std::forward<F>(write_fn)); }
    }

    // @safe - Sets connection validity
    void set_valid(bool valid) const;
    // @unsafe - Establishes TCP connection (contains const_cast and unsafe connect)
    int connect(const char* addr, bool client = true) const;

    /**
     * Set the reconnection policy for this client.
     * The policy controls automatic reconnection behavior after failures.
     */
    // @safe - Sets reconnection policy
    void set_reconnect_policy(const ReconnectPolicy& policy) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // const_cast needed since ClientConnection::set_reconnect_policy is not const
            auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
            conn.set_reconnect_policy(policy);
        }
    }

    /**
     * Set the buffering configuration for this client.
     * Controls whether requests are queued during disconnection.
     */
    // @unsafe - Calls ClientConnection::set_buffering_config (interior mutability)
    void set_buffering_config(const BufferingConfig& config) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_buffering_config(config);  // @unsafe
        }
    }

    // @unsafe - Uses RequestQueue which uses std::list
    size_t pending_request_count() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe { ClientConnection::pending_request_count }
            return guard->as_ref().unwrap()->pending_request_count();
        }
        return 0;
    }

    // @unsafe - Uses RequestQueue which uses std::list
    void clear_pending_requests(int error_code = ECONNABORTED) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe { ClientConnection::clear_pending_requests }
            guard->as_ref().unwrap()->clear_pending_requests(error_code);
        }
    }

    // @safe - Check if reconnection is in progress
    bool is_reconnecting() const {
        auto guard = connection_.borrow();
        return guard->is_some() && guard->as_ref().unwrap()->is_reconnecting();
    }

    /**
     * Attempt to reconnect to the last connected address.
     * Can only be called when connection is in FAILED or DISCONNECTED state.
     *
     * @param on_complete Optional callback called with success/failure result
     * @return 0 on success (reconnection started), error code on failure
     */
    // @unsafe - Attempts reconnection (calls connect which has socket operations)
    int reconnect(std::function<void(bool)> on_complete = nullptr) const;

    // @safe - Pauses the connection
    void pause() const;
    // @safe - Resumes the connection
    void resume() const;

    // reentrant, could be called multiple times
    // @safe - Closes socket and cleans up
    void close() const;

    // @safe - Jetpack compatibility wrapper
    void close_and_release() {
        close();
    }

    // @safe - Returns file descriptor
    int fd() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->fd();
        }
        return -1;
    }

    // @safe - Returns host string
    std::string host() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe
            { return guard->as_ref().unwrap()->host(); }
        }
        // @unsafe
        { return ""; }
    }

    // @safe - Returns connection status
    bool connected() const {
        auto guard = connection_.borrow();
        return guard->is_some() && guard->as_ref().unwrap()->connected();
    }

    // @safe - Returns current connection state
    ConnectionState connection_state() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->connection_state();
        }
        return ConnectionState::NEW;
    }

    /**
     * Try to reconnect if the connection is in a failed state.
     * Returns true if connection is now available (either was already connected
     * or reconnection succeeded), false if reconnection failed or not possible.
     *
     * This is a convenience method that checks the connection state and
     * attempts reconnection only if needed.
     */
    // @unsafe - May call reconnect which does socket operations
    bool try_reconnect_if_needed() const {
        auto state = connection_state();
        if (state == ConnectionState::CONNECTED) {
            return true;  // Already connected
        }
        if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
            // Try to reconnect
            int result = reconnect();
            return result == 0;
        }
        // CONNECTING or other states - can't help
        return false;
    }

    // @safe - Returns a clone of the connection Option
    // Returns None if not connected, Some(Arc<ClientConnection>) if connected
    rusty::Option<rusty::Arc<ClientConnection>> connection() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return rusty::Some(guard->as_ref().unwrap().clone());
        }
        return rusty::None;
    }

    // === Server Restart Detection API ===

    /**
     * Get the last known server instance ID.
     * Returns 0 if no ID has been set yet or no connection exists.
     */
    // @safe - Delegates to ClientConnection
    uint64_t server_instance_id() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->server_instance_id();
        }
        return 0;
    }

    /**
     * Set the callback to be invoked when server restart is detected.
     * The callback receives (old_id, new_id) when the server's instance ID changes.
     *
     * @param callback Function to call on restart detection
     */
    // @unsafe - Delegates to @unsafe ClientConnection::set_on_server_restart
    void set_on_server_restart(std::function<void(uint64_t, uint64_t)> callback) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_on_server_restart(std::move(callback));
        }
    }

    /**
     * Check and update the server instance ID.
     * If the new ID differs from the stored ID (and stored ID was non-zero),
     * the on_server_restart callback is invoked.
     *
     * @param new_id The new server instance ID
     * @return true if server restart was detected, false otherwise
     */
    // @unsafe - Delegates to @unsafe ClientConnection::check_server_instance
    bool check_server_instance(uint64_t new_id) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->check_server_instance(new_id);
        }
        return false;
    }

    // === Connection Validation API ===

    /**
     * Configure TCP keepalive for the connection.
     * If called before connect(), the config is stored and applied when connection is created.
     * If called after connect(), the config is applied immediately to the existing connection.
     *
     * @param config The keepalive configuration to apply
     */
    // @safe - Uses Cell for interior mutability
    void set_keepalive(const KeepaliveConfig& config) const {
        // Always store locally (for use in connect() if called before connection exists)
        pending_keepalive_config_.set(config);

        // If connection exists, also apply immediately
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_keepalive(config);
        }
    }

    /**
     * Get the current keepalive configuration.
     * Returns the connection's config if connected, or the pending config otherwise.
     */
    // @safe - Returns copy via Cell::get()
    KeepaliveConfig keepalive_config() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->keepalive_config();
        }
        // Return pending config if no connection exists
        return pending_keepalive_config_.get();
    }

    /**
     * Check if the connection is idle.
     *
     * @param idle_ms Idle threshold in milliseconds
     * @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
     * @return true if idle for longer than threshold, false if not idle or no connection
     */
    // @safe - Delegates to @safe ClientConnection::is_idle
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->is_idle(idle_ms, current_time_ms);
        }
        return false;
    }

    /**
     * Validate the connection is still alive and healthy.
     *
     * @return true if connection is healthy, false if it needs reconnection or doesn't exist
     */
    // @unsafe - Delegates to ClientConnection::validate_connection (uses getsockopt syscall)
    bool validate_connection() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe { getsockopt system call }
            return guard->as_ref().unwrap()->validate_connection();
        }
        return false;
    }

    /**
     * Get the connection metrics.
     * Returns reference to empty metrics if no connection exists.
     */
    // @safe - Returns const reference (static empty for no-connection case)
    // @lifetime: (&'a) -> &'a
    const ConnectionMetrics& metrics() const {
        static const ConnectionMetrics empty_metrics;
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->metrics();
        }
        return empty_metrics;
    }

    /**
     * Check if a connection exists and has metrics.
     * Use this to verify before calling metrics() if you need to distinguish
     * between "no connection" and "connection with zero activity".
     */
    // @safe - Simple connection check
    bool has_connection() const {
        auto guard = connection_.borrow();
        return guard->is_some();
    }

    // @safe - Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid) const;

};

// @safe - Thread-safe pool of client connections using Arc
// MIGRATED: Now uses rusty::Arc<Client> for cached connections
class ClientPool {
    rrr::Rand rand_;

    // owns a shared reference to PollThread
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;

    // guard cache_
    SpinLock l_;
    // @safe - Uses rusty::Arc<Client> for thread-safe reference counting
    // SAFETY: Arc provides thread-safe reference counting with polymorphism support
    std::map<std::string, std::vector<rusty::Arc<Client>>> cache_;

    // Pool configuration (Cell for interior mutability)
    rusty::Cell<PoolConfig> config_;

    // Load balancer state per address (for round-robin tracking)
    std::map<std::string, LoadBalancerState> lb_state_;

    // Helper: Check if a client is considered healthy
    // @safe - Uses metrics to determine health
    bool is_client_healthy(const rusty::Arc<Client>& client) const;

public:
    // @safe - Creates pool with optional PollThread and config
    ClientPool(rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker = rusty::None,
               const PoolConfig& config = PoolConfig::defaults());
    // @safe - Closes all cached connections
    ~ClientPool();

    // === Configuration ===

    // @safe - Set pool configuration
    void set_pool_config(const PoolConfig& config);

    // @safe - Get current pool configuration
    PoolConfig pool_config() const;

    // === Client Access ===

    // return cached client connection
    // on error, return None
    // @unsafe - Gets or creates client connection
    // SAFETY: Contains raw pointer dereference
    rusty::Option<rusty::Arc<rrr::Client>> get_client(const std::string& addr);

    // === Health Management ===

    // @safe - Get count of healthy clients for an address
    size_t get_healthy_client_count(const std::string& addr);

    // @safe - Remove unhealthy clients for an address
    // Returns number of clients removed
    size_t remove_unhealthy_clients(const std::string& addr);

    // @safe - Close idle clients for an address
    // Returns number of clients closed
    // @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
    size_t close_idle_clients(const std::string& addr, uint64_t current_time_ms);

    // === Pool-wide Operations ===

    // @safe - Remove all unhealthy clients from all addresses
    // Returns total number of clients removed
    size_t remove_all_unhealthy();

    // @safe - Close all idle clients from all addresses
    // Returns total number of clients closed
    // @param current_time_ms Current time in milliseconds
    size_t close_all_idle(uint64_t current_time_ms);

    // @safe - Get total number of cached clients across all addresses
    size_t total_client_count();

    // @safe - Get number of addresses with cached clients
    size_t address_count();

    // === Bulk Reconnection ===

    /**
     * Result of a bulk reconnection operation.
     */
    struct BulkReconnectResult {
        size_t total;       // Total clients attempted
        size_t succeeded;   // Number that reconnected successfully
        size_t failed;      // Number that failed to reconnect
        size_t skipped;     // Number skipped (already connected)
    };

    /**
     * Configuration for bulk reconnection.
     */
    struct BulkReconnectConfig {
        uint32_t max_concurrent = 10;     // Max concurrent reconnections
        uint32_t delay_between_ms = 10;   // Delay between batches
        bool skip_connected = true;       // Skip already connected clients

        // @safe - Default constructor creates POD struct
        BulkReconnectConfig() = default;

        // Presets
        // @safe - Pure function creating POD struct
        static BulkReconnectConfig defaults() {
            return BulkReconnectConfig{};
        }

        // @safe - Pure function creating POD struct
        static BulkReconnectConfig fast() {
            BulkReconnectConfig cfg;
            cfg.max_concurrent = 50;
            cfg.delay_between_ms = 0;
            return cfg;
        }

        // @safe - Pure function creating POD struct
        static BulkReconnectConfig gentle() {
            BulkReconnectConfig cfg;
            cfg.max_concurrent = 5;
            cfg.delay_between_ms = 50;
            return cfg;
        }
    };

    /**
     * Reconnect all clients for a specific address.
     *
     * @param addr The address to reconnect clients for
     * @param config Configuration for the bulk operation
     * @return Result containing success/failure counts
     */
    // @unsafe - Calls reconnect on clients
    BulkReconnectResult reconnect_all(const std::string& addr,
                                      const BulkReconnectConfig& config = BulkReconnectConfig::defaults());

    /**
     * Reconnect all clients across all addresses.
     *
     * @param config Configuration for the bulk operation
     * @return Result containing success/failure counts
     */
    // @unsafe - Calls reconnect on clients
    BulkReconnectResult reconnect_all(const BulkReconnectConfig& config = BulkReconnectConfig::defaults());

};

}
