// @unsafe - RPC server module uses raw sockets and mutable spinlocks
#pragma once
#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>

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

// External safety annotations for system functions used in this module
// @external: {
//   bind: [unsafe, (int, const struct sockaddr*, socklen_t) -> int]
//   listen: [unsafe, (int, int) -> int]
//   accept: [unsafe, (int, struct sockaddr*, socklen_t*) -> int]
//   usleep: [unsafe, (useconds_t) -> int]
// }

// External safety annotations for STL operations used in this module
// @external: {
//   operator!=: [safe, (auto, auto) -> bool]
//   operator==: [safe, (auto, auto) -> bool]
//   std::*::find: [safe, (auto) -> auto]
//   std::*::end: [safe, () -> auto]
//   std::*::begin: [safe, () -> auto]
//   std::*::insert: [safe, (auto) -> auto]
//   std::*::operator[]: [safe, (auto) -> auto]
//   std::*::erase: [safe, (auto) -> auto]
// }

// External safety annotations for Log functions
// @external: {
//   Log::debug: [unsafe]
//   Log::info: [unsafe]
//   Log::error: [unsafe]
//   Log_debug: [unsafe]
//   Log_info: [unsafe]
//   Log_warn: [unsafe]
//   Log_error: [unsafe]
// }

// External safety annotations for SpinLock (interior mutability pattern)
// @external: {
//   SpinLock::lock: [unsafe]
//   SpinLock::unlock: [unsafe]
//   const_cast: [unsafe]
// }

// for getaddrinfo() used in Server::start()
//struct addrinfo;

// @unsafe - RPC module uses raw sockets, mutable spinlocks, and pthread primitives
namespace rrr {

class Server;

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

// @safe - Abstract service interface
class Service {
public:
    virtual ~Service() {}
    // @safe - Virtual method for service registration
    virtual int __reg_to__(Server*) = 0;
};

// @unsafe - Server listener handling incoming connections
// SAFETY: Manages socket lifecycle and address info properly
class ServerListener: public Pollable {
  friend class Server;
 public:
  std::string addr_;
  Server* server_;  // Non-owning pointer to parent server
  // cannot use smart pointers for memory management because this pointer
  // needs to be freed by freeaddrinfo.
  struct addrinfo* p_gai_result_{nullptr};
  struct addrinfo* p_svr_addr_{nullptr};

  int server_sock_{0};

  // @safe - Returns constant poll mode
  int poll_mode() const override {
    return Pollable::READ;
  }

  // Jetpack: content_size not used for listener
  size_t content_size() override {
    verify(0);
    return 0;
  }

  // @safe - Not implemented, will abort if called
  // Returns MODE_NO_CHANGE since ServerListener never handles write
  int handle_write() override {verify(0); return Pollable::MODE_NO_CHANGE;}

  // @unsafe - Calls unsafe Log::debug for connection logging
  // SAFETY: Thread-safe with server connection lock
  // Jetpack: split-phase read support
  bool handle_read_one() override { return handle_read(); }
  bool handle_read_two() override { verify(0); return true; }
  bool handle_read() override;

  // @safe - Not implemented, will abort if called
  void handle_error() override {verify(0);}

  // @safe - Closes server socket
  // Close is marked safe via external annotation
  void close() override;

  // @safe - Check if closed (server_sock_ < 0)
  bool is_closed() const override { return server_sock_ < 0; }

  // @safe - Returns file descriptor
  int fd() const override {return server_sock_;}

  // @safe - Constructor with proper error handling
  ServerListener(Server* s, std::string addr);

//protected:
  // @safe - Frees addrinfo structures
  // freeaddrinfo is marked safe via external annotation
  virtual ~ServerListener() {
    if (p_gai_result_ != nullptr) {
      freeaddrinfo(p_gai_result_);
      p_gai_result_ = nullptr;
      p_svr_addr_ = nullptr;
    }
  };
};

// Forward declaration
class ServerConnection;

// Type alias for Arc weak reference
using WeakServerConnection = rusty::sync::Weak<ServerConnection>;

// @unsafe - Uses mutable SpinLock for interior mutability
class ServerConnection: public Pollable {
    // Handles individual client connections
    // SAFETY: Thread-safe with spinlocks, proper Arc lifetime management

    friend class Server;
    friend class ServerListener;

    Marshal in_, out_;
    // UnsafeCell for interior mutability (const methods need to lock)
    rusty::UnsafeCell<SpinLock> out_l_;

    Server* server_;
    int socket_;

    rusty::Option<rusty::Box<Marshal::bookmark>> bmark_;

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
    // @safe - Closes connection and cleans up (has internal @unsafe blocks)
    // SAFETY: Thread-safe with server connection lock
    void close() override;

private:
    // used to surpress multiple "no handler for rpc_id=..." errro
    // SpinMutex provides thread-safe interior mutability
    static SpinMutex<std::unordered_set<i32>> rpc_id_missing_s;

public:
    // Jetpack-specific member
    int count = 0;

    // Public destructor for shared_ptr compatibility
    // @safe - Simple destructor updating counter
    ~ServerConnection();

    // @unsafe - Initializes connection with socket
    // SAFETY: Increments server connection counter
    ServerConnection(Server* server, int socket);

    bool connected() {
      return status_ == CONNECTED;
    }

    /**
     * Start a reply message. Must be paired with end_reply().
     *
     * Reply message format:
     * <size> <xid> <error_code> <ret1> <ret2> ... <retN>
     * NOTE: size does not include size itself (<xid>..<retN>).
     *
     * User only need to fill <ret1>..<retN>.
     *
     * Currently used errno:
     * 0: everything is fine
     * ENOENT: method not found
     * EINVAL: invalid packet (field missing)
     */
    // @safe - Starts reply marshaling
    // SAFETY: Protected by output spinlock (SpinLock marked as external)
    void begin_reply(const Request& req, i32 error_code = 0);

    // @safe - Completes reply packet
    // SAFETY: Protected by output spinlock, uses weak ref to poll thread
    void end_reply();

    // helper function, do some work in background
    int run_async(const std::function<void()>& f);

    // @safe - Marshals data into output buffer
    // @lifetime: (&'a, const T&) -> &'a
    template<class T>
    ServerConnection& operator <<(const T& v) {
        this->out_ << v;
        return *this;
    }

    // @safe - Marshals data from another Marshal
    // @lifetime: (&'a, Marshal&) -> &'a
    ServerConnection& operator <<(Marshal& m) {
        this->out_.read_from_marshal(m, m.content_size());
        return *this;
    }

    int fd() const override {
        return socket_;
    }

    // @safe - Returns poll mode based on output buffer
    // Uses const_cast for interior mutability (SpinLock marked as external)
    int poll_mode() const override;

    // Jetpack: content_size not used for connection
    size_t content_size() override {
        verify(0);
        return 0;
    }

    // @safe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock (SpinLock marked as external)
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write() override;

    // @unsafe - Reads and processes RPC requests
    // SAFETY: Creates coroutines for handlers
    bool handle_read() override;  // Batching mode: reads ALL available requests

    // Jetpack: split-phase read support
    bool handle_read_one() override { return handle_read(); }
    bool handle_read_two() override { verify(0); return true; }

    // @safe - Error handler (explicit this-> is now safe in rusty-cpp)
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

    // Jetpack: handle_free stub
    void handle_free() {verify(0);}

    // Comparison operator for std::unordered_set<rusty::Arc<ServerConnection>>
    friend bool operator==(const rusty::Arc<ServerConnection>& lhs, const rusty::Arc<ServerConnection>& rhs) {
        return lhs.get() == rhs.get();
    }

    // Hash function for std::unordered_set
    friend struct std::hash<rusty::Arc<ServerConnection>>;
};

} // namespace rrr

// Hash specializations for rusty::Arc types
namespace std {
template<>
struct hash<rusty::Arc<rrr::ServerConnection>> {
    size_t operator()(const rusty::Arc<rrr::ServerConnection>& arc) const {
        return hash<const rrr::ServerConnection*>()(arc.get());
    }
};

template<>
struct hash<rusty::Arc<rrr::ServerListener>> {
    size_t operator()(const rusty::Arc<rrr::ServerListener>& arc) const {
        return hash<const rrr::ServerListener*>()(arc.get());
    }
};
}

namespace rrr {

// @safe - RAII wrapper for deferred RPC replies with move semantics
// Handler receives DeferredReply by value (moved) and calls defer.reply()
// The destructor handles cleanup (deletes in_ and out_ params allocated by wrapper)
class DeferredReply {
    rusty::Box<rrr::Request> req_;
    WeakServerConnection weak_sconn_;
    rusty::Function<void()> marshal_reply_;
    rusty::Function<void()> cleanup_;
    bool replied_ = false;  // Track if reply was sent

public:
    // Movable but not copyable
    DeferredReply(DeferredReply&& other) = default;
    DeferredReply& operator=(DeferredReply&& other) = default;
    DeferredReply(const DeferredReply&) = delete;
    DeferredReply& operator=(const DeferredReply&) = delete;

    DeferredReply(rusty::Box<rrr::Request> req, WeakServerConnection weak_sconn,
                  rusty::Function<void()> marshal_reply, rusty::Function<void()> cleanup)
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

    int run_async(const std::function<void()>& f) {
      // TODO disable threadpool run in RPCs.
      return 0;
    }

    // @unsafe - Sends reply
    // SAFETY: Can only be called once (checked by replied_ flag)
    void reply() {
        if (replied_) {
            Log_warn("DeferredReply::reply() called multiple times, ignoring");
            return;
        }
        replied_ = true;

        // @unsafe - SAFETY: pointer operations within unsafe context
        auto sconn_opt = weak_sconn_.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            // @unsafe - SAFETY: const_cast safe because Arc contents are mutable
            {
                const_cast<ServerConnection&>(*sconn).begin_reply(*req_);
                marshal_reply_();
                const_cast<ServerConnection&>(*sconn).end_reply();
            }
        } else {
            // Connection closed, silently drop reply
            Log_debug("Connection closed before reply sent, dropping reply");
        }
        // Object will be destroyed when it goes out of scope, destructor calls cleanup_()
    }
};

// @unsafe - Main RPC server managing connections
// SAFETY: Thread-safe connection management with spinlocks
class Server: public NoCopy {
    friend class ServerConnection;
 public:
    using RequestHandler = std::function<void(rusty::Box<Request>, WeakServerConnection)>;
    std::unordered_map<i32, RequestHandler> handlers_;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;  // Shared ownership via Arc<Mutex<>>

    Counter sconns_ctr_;

    // SpinMutex provides thread-safe interior mutability for connection set
    SpinMutex<std::unordered_set<rusty::Arc<ServerConnection>>> sconns_{
        std::unordered_set<rusty::Arc<ServerConnection>>()};
    rusty::Option<rusty::Arc<ServerListener>> sp_server_listener_;

    // Owned services - Server takes ownership to ensure services outlive handlers
    // Stores cleanup functions for type-erased service deletion
    std::vector<std::function<void()>> service_cleanups_;

public:
    std::string addr_;

    // @unsafe - Creates server with optional PollThread
    // SAFETY: Shared ownership of PollThread via Arc<Mutex<>>
    Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::None);
    // @unsafe - Destroys server and all connections
    // SAFETY: Waits for all connections to close, then deletes owned services
    virtual ~Server();

    // @unsafe - Starts server on specified address
    // SAFETY: Proper socket binding and thread creation
    int start(const char* bind_addr);

    // @unsafe - Registers service and transfers ownership to Server
    // Server owns the service, ensuring it outlives all RPC handlers.
    // Returns raw pointer for caller to use (valid as long as Server lives).
    // Service types must be movable (not copyable).
    // Unsafe: uses new/delete and raw pointer dereference
    template<class S>
    S* reg_service(S svc) {
        static_assert(std::is_base_of<Service, S>::value, "S must derive from Service");
        S* ptr = new S(std::move(svc));
        service_cleanups_.push_back([ptr]() { delete ptr; });
        ptr->__reg_to__(this);
        return ptr;
    }

    // @unsafe - Registers service without taking ownership (borrowed reference)
    // Caller must ensure service outlives the Server.
    // Use this when services are managed externally (e.g., by Frame/Worker).
    void reg_service_ref(Service& svc) {
        svc.__reg_to__(this);
    }

    /**
     * The svc_func need to do this:
     *
     *  {
     *     // process request
     *     ..
     *
     *     // send reply
     *     server_connection->begin_reply(*req);
     *     *server_connection << {reply_content};
     *     server_connection->end_reply();
     *
     *     // cleanup resource - automatic via unique_ptr
     *     // No need to release, shared_ptr handles connection
     *  }
     */
    // @unsafe - Registers RPC handler function (uses unordered_map)
    int reg_handler(i32 rpc_id, const RequestHandler& func);

    // @unsafe - uses raw pointer svc and member function pointer
    template<class S>
    int reg_method(i32 rpc_id, S* svc, void (S::*svc_func)(rusty::Box<Request>, WeakServerConnection)) {

        // disallow duplicate rpc_id
        if (handlers_.find(rpc_id) != handlers_.end()) {
            return EEXIST;
        }

        handlers_[rpc_id] = [svc, svc_func] (rusty::Box<Request> req, WeakServerConnection sconn) {
            (svc->*svc_func)(std::move(req), sconn);
        };

        return 0;
    }

    // @unsafe - Unregisters RPC handler (uses unordered_map)
    void unreg(i32 rpc_id);
};

} // namespace rrr
