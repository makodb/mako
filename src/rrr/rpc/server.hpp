// @unsafe - RPC server module uses raw sockets and mutable spinlocks
#pragma once
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>
#include <rusty/vec.hpp>
#include <rusty/rusty.hpp>  // For rusty::Mutex, rusty::Condvar

#include <unordered_map>
#include <unordered_set>
#include <pthread.h>
#include <memory>
#include <chrono>

#include <sys/socket.h>
#include <netdb.h>

#include "misc/marshal.hpp"
#include "reactor/epoll_wrapper.h"
#include "reactor/reactor.h"
#include "utils.hpp"

// External safety annotations for system functions and STL operations
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Coroutine, and rusty-cpp types
// now have in-place annotations in their respective headers.
//
// SAFETY AUDIT: STL container operations are marked [safe] because:
// 1. All operations are used within SpinMutex lock guards (single-threaded access)
// 2. Iterators are not held across lock boundaries
// 3. No iterator invalidation occurs during iteration
//
// System functions (bind, listen, accept) remain [unsafe] as they involve I/O.
//
// @external: {
//   bind: [unsafe, (int, const struct sockaddr*, socklen_t) -> int]
//   listen: [unsafe, (int, int) -> int]
//   accept: [unsafe, (int, struct sockaddr*, socklen_t*) -> int]
//   usleep: [unsafe, (useconds_t) -> int]
//   operator!=: [safe]
//   operator==: [safe]
//   std::unordered_map::find: [safe, (&'a, const K&) -> iterator where return: 'a]
//   std::unordered_map::end: [safe, (&'a) -> iterator]
//   std::unordered_map::begin: [safe, (&'a) -> iterator]
//   std::unordered_map::insert: [safe, (&'a mut, const K&, V) -> pair]
//   std::unordered_map::insert_or_assign: [safe, (&'a mut, const K&, V) -> pair]
//   std::unordered_map::operator[]: [safe, (&'a mut, const K&) -> V& where return: 'a]
//   std::unordered_map::erase: [safe, (&'a mut, iterator) -> iterator]
//   std::unordered_map::clear: [safe, (&'a mut) -> void]
//   std::unordered_set::find: [safe, (&'a, const T&) -> iterator where return: 'a]
//   std::unordered_set::end: [safe, (&'a) -> iterator]
//   std::unordered_set::begin: [safe, (&'a) -> iterator]
//   std::unordered_set::insert: [safe, (&'a mut, const T&) -> pair]
//   std::unordered_set::erase: [safe, (&'a mut, iterator) -> iterator]
//   std::list::push_back: [safe, (&'a mut, const T&) -> void]
//   std::list::begin: [safe, (&'a) -> iterator where return: 'a]
//   std::list::end: [safe, (&'a) -> iterator]
//   std::list::empty: [safe, (&'a) -> bool]
//   std::list::erase: [safe, (&'a mut, iterator) -> iterator]
//   std::__cxx11::list::push_back: [safe, (&'a mut, const T&) -> void]
//   std::vector::push_back: [safe, (&'a mut, const T&) -> void]
//   std::vector::empty: [safe, (&'a) -> bool]
//   std::vector::size: [safe, (&'a) -> size_t]
//   std::function::operator(): [safe]
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
//   rrr::ServerConnection::write_fn: [safe]
// }
// NOTE: Marshal methods (set_bookmark, write_bookmark, get_and_reset_write_cnt, empty, content_size)
// are now annotated @safe in-place in marshal.hpp

// for getaddrinfo() used in Server::start()
//struct addrinfo;

// @unsafe - RPC module uses raw sockets, mutable spinlocks, and pthread primitives
namespace rrr {

class Server;
class ServerConnection;
struct RpcServiceContext;

/**
 * The raw packet sent from client will be like this:
 * <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
 * NOTE: size does not include the size itself (<xid>..<argN>).
 *
 * For the request object, the marshal only contains <arg1>..<argN>,
 * other fields are already consumed.
 */
// @safe - Simple request container
struct Request {
    Marshal m;
    i64 xid;
};

// Forward declaration for WeakServerConnection
class ServerConnection;

// Type alias for Arc weak reference (must be before Service for __dispatch__)
using WeakServerConnection = rusty::sync::Weak<ServerConnection>;

// @interface
class Service {
public:
    virtual ~Service() = default;
    // Virtual method for service registration with index (used by reg_service)
    // Returns list of RPC IDs that this service handles
    // @safe
    virtual int __reg_to__(Server&, size_t svc_index) = 0;

    // @safe - Virtual dispatch method for handling RPC requests
    // Each service implements this to route requests to the appropriate handler
    // Uses virtual dispatch to avoid raw pointer capture and static_cast
    virtual void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection sconn) = 0;
};

/**
 * Shared context for RPC service dispatch.
 *
 * This struct is shared between Server, ServerListener, and ServerConnection
 * via Arc<RpcServiceContext> to avoid raw pointer dependencies.
 *
 * SAFETY: The struct is constructed once in Server::start() and shared via Arc.
 * All fields are immutable after construction. Services use RefCell for interior
 * mutability, allowing non-const __dispatch__ calls through const Arc access.
 *
 * NOTE: RefCell is single-threaded. All RPC dispatches must occur on the same thread.
 */
struct RpcServiceContext {
    // Maps RPC ID to service index for virtual dispatch (immutable after setup)
    const std::unordered_map<i32, size_t> rpc_to_service;

    // Owned services wrapped in RefCell for interior mutability
    // RefCell allows mutable access through const reference (borrow_mut)
    const rusty::Vec<rusty::RefCell<rusty::Box<Service>>> services;

    // Server address for logging (immutable after setup)
    const std::string addr;

    // Constructor taking ownership of all data
    RpcServiceContext(std::unordered_map<i32, size_t> rpc_map,
                      rusty::Vec<rusty::RefCell<rusty::Box<Service>>> svcs,
                      std::string address)
        : rpc_to_service(std::move(rpc_map))
        , services(std::move(svcs))
        , addr(std::move(address)) {}
};

// @unsafe - Server listener handling incoming connections
// SAFETY: Manages socket lifecycle and address info properly
class ServerListener: public Pollable {
  friend class Server;
 public:
  std::string addr_;
  rusty::Arc<RpcServiceContext> ctx_;  // Shared dispatch context
  // File descriptors of accepted connections - Server reads this at shutdown
  SpinMutex<rusty::Vec<int>> sconn_fds_{rusty::Vec<int>()};
  // AddrInfo RAII wrapper - automatically frees addrinfo on destruction
  AddrInfo gai_result_;
  // Pointer into the linked list of addresses (points within gai_result_)
  struct addrinfo* p_svr_addr_{nullptr};

  int server_sock_{0};

  int poll_mode() const override {
    return PollMode::READ;
  }

  size_t content_size() override {
    verify(0);
    return 0;
  }

  int handle_write() override {verify(0); return PollMode::NO_CHANGE;}

  bool handle_read() override;

  void handle_error() override {verify(0);}

  void close() override;

  bool is_closed() const override { return server_sock_ < 0; }

  bool check_pending_write_update() const override { return false; }

  int fd() const override {return server_sock_;}

  // @safe - Constructor with proper error handling
  ServerListener(rusty::Arc<RpcServiceContext> ctx, std::string addr);

//protected:
  // @safe - AddrInfo RAII wrapper handles freeaddrinfo automatically
  virtual ~ServerListener() {
    // gai_result_ RAII wrapper automatically calls freeaddrinfo
    p_svr_addr_ = nullptr;  // Clear pointer into freed memory
  };
};

// @unsafe - Inherits from @interface Pollable (rusty-cpp namespace resolution bug workaround)
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership
class ServerConnection: public Pollable {
    // Handles individual client connections
    // SAFETY: Thread-safe with spinlocks, proper Arc lifetime management

    friend class Server;
    friend class ServerListener;

    Marshal in_;
    SpinMutex<Marshal> out_;  // Lock + data combined (has interior mutability)

    rusty::Arc<RpcServiceContext> ctx_;  // Shared dispatch context
    int socket_;

    enum {
        CONNECTED, CLOSED
    } status_;

    // Flag set by end_reply() to indicate write mode update needed
    // Checked by poll loop after processing events
    // Cell provides interior mutability for safe access through const methods
    rusty::Cell<bool> pending_write_update_{false};

    // Weak pointer to self, initialized after creation
    // Used to pass weak reference to async handlers
    WeakServerConnection weak_self_;

    // get_shared() is now inherited from Pollable base class

public:
    /**
     * Closes the connection and cleans up resources.
     * Called by:
     * 1: PollThreadWorker::do_close_pollable() for thread-safe close
     * 2: handle_error() for error handling
     */
    // @safe - Closes connection and cleans up
    // SAFETY: Thread-safe with server connection lock
    void close() override;

private:
    // used to surpress multiple "no handler for rpc_id=..." errro
    // SpinMutex provides thread-safe interior mutability
    static SpinMutex<std::unordered_set<i32>> rpc_id_missing_s;

public:
    // Jetpack-specific member
    int count = 0;

    // Public destructor - Arc prevents premature destruction
    // @safe - Simple destructor
    ~ServerConnection();

    // @safe - Initializes connection with socket
    ServerConnection(rusty::Arc<RpcServiceContext> ctx, int socket);

    // @safe - Simple status check
    bool connected() {
      return status_ == CONNECTED;
    }

    /**
     * Send a reply message with callback-based marshaling.
     *
     * Reply message format:
     * <size> <xid> <error_code> <ret1> <ret2> ... <retN>
     * NOTE: size does not include size itself (<xid>..<retN>).
     *
     * The write_fn callback receives a Marshal& to write <ret1>..<retN>.
     *
     * Currently used errno:
     * 0: everything is fine
     * ENOENT: method not found
     * EINVAL: invalid packet (field missing)
     */
    // @safe - Sends reply with callback for marshaling response data
    // All operations use interior mutability:
    // - SpinMutex::lock() const: uses UnsafeCell for interior mutability
    // - Cell::set(): interior mutability for pending_write_update_
    // - status_: read-only access
    template<typename F>
    void reply(const Request& req, i32 error_code, F&& write_fn) const {
        auto guard = out_.lock().unwrap();
        v32 v_error_code = error_code;
        v64 v_reply_xid = req.xid;

        Marshal::bookmark bm = guard->set_bookmark(sizeof(i32));
        // @unsafe
        {
            *guard << v_reply_xid;
            *guard << v_error_code;
        }

        write_fn(*guard);

        i32 reply_size = guard->get_and_reset_write_cnt();
        guard->write_bookmark(bm, reply_size);

        if (status_ == CONNECTED) {
            pending_write_update_.set(true);
        }
    }

    // @safe - Sends empty reply
    void reply(const Request& req, i32 error_code = 0) const {
        reply(req, error_code, [](Marshal&) {});
    }

    // @safe - Delegates to thread pool (currently a no-op stub)
    int run_async(const std::function<void()>& f);

    // @safe - Returns file descriptor
    int fd() const override {
        return socket_;
    }

    // @safe - Returns poll mode based on output buffer
    // Uses const_cast for interior mutability (SpinLock marked as external)
    int poll_mode() const override;

    // @safe - Not implemented, will abort if called
    // Jetpack: content_size not used for connection
    size_t content_size() override {
        verify(0);
        return 0;
    }

    // @safe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock (SpinLock marked as external)
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write() override;

    // @safe - Reads and processes RPC requests
    // Memory-safe: Uses Box for request ownership, virtual dispatch for handlers,
    // Arc for shared context, RefCell for interior mutability, Coroutine::CreateRun for async.
    bool handle_read() override;  // Batching mode: reads ALL available requests

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
        return status_ == CLOSED;
    }

    // @safe - Not implemented, will abort if called
    // Jetpack: handle_free stub
    void handle_free() {verify(0);}

};

} // namespace rrr


namespace rrr {

// @safe - RAII wrapper for deferred RPC replies with move semantics
// Handler receives DeferredReply by value (moved) and calls defer.reply()
// The destructor handles cleanup (deletes in_ and out_ params allocated by wrapper)
class DeferredReply {
    rusty::Box<rrr::Request> req_;
    WeakServerConnection weak_sconn_;
    rusty::Function<void(Marshal&)> marshal_reply_;  // Takes Marshal& to write response
    rusty::Function<void()> cleanup_;
    bool replied_ = false;  // Track if reply was sent

public:
    // Movable but not copyable
    DeferredReply(DeferredReply&& other) = default;
    DeferredReply& operator=(DeferredReply&& other) = default;
    DeferredReply(const DeferredReply&) = delete;
    DeferredReply& operator=(const DeferredReply&) = delete;

    // @safe - Initializes deferred reply with move semantics
    DeferredReply(rusty::Box<rrr::Request> req, WeakServerConnection weak_sconn,
                  rusty::Function<void(Marshal&)> marshal_reply, rusty::Function<void()> cleanup)
        : req_(std::move(req)), weak_sconn_(weak_sconn),
          marshal_reply_(std::move(marshal_reply)), cleanup_(std::move(cleanup)) {}

    // @safe - Cleanup destructor
    // SAFETY: cleanup_() deletes in_ and out_ params allocated by wrapper
    ~DeferredReply() {
        if (cleanup_) {
            cleanup_();
        }
        // req_ automatically cleaned up by rusty::Box destructor
    }

    // @safe - Currently a no-op stub
    int run_async(const std::function<void()>& f) {
      // TODO disable threadpool run in RPCs.
      return 0;
    }

    // @safe - Sends reply using callback-based API
    // Can only be called once (checked by replied_ flag)
    // Uses weak pointer upgrade (safe: returns Option)
    void reply() {
        if (replied_) {
            Log_warn("DeferredReply::reply() called multiple times, ignoring");
            return;
        }
        replied_ = true;

        // @unsafe - weak pointer upgrade (safe operation, but rusty-cpp needs annotation)
        {
            auto sconn_opt = weak_sconn_.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                // No const_cast needed: reply() is now a const method with interior mutability
                sconn->reply(*req_, 0, marshal_reply_);
            } else {
                // Connection closed, silently drop reply
                Log_debug("Connection closed before reply sent, dropping reply");
            }
        }
        // Object will be destroyed when it goes out of scope, destructor calls cleanup_()
    }
};

// @unsafe - Main RPC server managing connections
// SAFETY: Thread-safe connection management with spinlocks
class Server: public NoCopy {
    friend class ServerConnection;
 public:
    // Pending registration data (before start() is called)
    // These are moved into RpcServiceContext in start()
    rusty::Vec<rusty::Box<Service>> pending_services_;
    std::unordered_map<i32, size_t> pending_rpc_to_service_;

    // Shared context containing RPC dispatch info and services
    // Created in start() after all registrations are complete
    // None until start() is called
    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_;

    // Poll thread for async I/O - shared with ServerListener
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;

    rusty::Option<rusty::Arc<ServerListener>> server_listener_;

    // Shutdown coordination - allows workers to wait for shutdown signal
    struct ShutdownState { bool shutdown = false; };
    rusty::Mutex<ShutdownState> shutdown_state_{ShutdownState{}};
    rusty::Condvar shutdown_cond_;

public:
    // @safe - Creates server with optional PollThread
    // SAFETY: Shared ownership of PollThread via Arc<Mutex<>>
    Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::None);
    // @safe - Destroys server and requests close for all connections
    // SAFETY: Arc<RpcServiceContext> ensures services live until all connections are done
    virtual ~Server();

    // @unsafe - Starts server on specified address (raw pointer dereference)
    int start(const char* bind_addr);

    // @safe - Registers service and transfers ownership to Server
    // Server owns the service via Box<Service>, ensuring it outlives all RPC handlers.
    // Accepts Box<Derived> which converts to Box<Service> via move constructor.
    // Must be called before start().
    void reg_service(rusty::Box<Service> svc) {
        pending_services_.push(std::move(svc));
        // Get index AFTER push - this is the position of the service we just added
        size_t svc_index = pending_services_.size() - 1;
        // Register handlers using the index (service is safely stored in pending_services_)
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
    }

    /**
     * The svc_func need to do this:
     *
     *  {
     *     // process request
     *     ..
     *
     *     // send reply using callback-based API
     *     server_connection->reply(*req, 0, [&](Marshal& out) {
     *         out << reply_content;
     *     });
     *
     *     // cleanup resource - automatic via unique_ptr
     *     // No need to release, shared_ptr handles connection
     *  }
     */
    // @safe - Registers an RPC ID to be handled by the service at svc_index
    // The actual dispatch is done via virtual Service::__dispatch__
    // No pointers or type erasure - just maps rpc_id to service index
    // Must be called before start().
    int reg_rpc(i32 rpc_id, size_t svc_index) {
        // disallow duplicate rpc_id
        // @unsafe
        {
            if (pending_rpc_to_service_.find(rpc_id) != pending_rpc_to_service_.end()) {
                return EEXIST;
            }
            pending_rpc_to_service_[rpc_id] = svc_index;
            return 0;
        }
    }

    // @safe - Unregisters RPC handler
    void unreg(i32 rpc_id);

    // @safe - Signals shutdown to waiting threads
    void do_shutdown();

    // @safe - Blocks until shutdown is signaled
    void wait_for_shutdown();

    // @safe - Iterate over all registered services
    // Useful for cleanup operations like flushing recorders at shutdown.
    // The callback receives a Service& reference; use dynamic_cast to get concrete type.
    // Must be called after start().
    template<typename F>
    void for_each_service(F&& callback) {
        auto& ctx = ctx_.as_ref().unwrap();
        for (size_t i = 0; i < ctx->services.size(); ++i) {
            auto guard = ctx->services[i].borrow_mut();
            // guard is RefMut<Box<Service>>, *guard is Box<Service>&, **guard is Service&
            callback(**guard);
        }
    }

    // @safe - Returns the number of registered services
    // Can be called before or after start().
    size_t service_count() const {
        if (ctx_.is_some()) {
            return ctx_.as_ref().unwrap()->services.size();
        }
        return pending_services_.size();
    }

    // Returns the server address (copy to avoid reference through Arc)
    // Must be called after start().
    std::string addr() const {
        return ctx_.as_ref().unwrap()->addr;
    }
};

} // namespace rrr
