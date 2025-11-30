#pragma once
#include <rusty/arc.hpp>
#include <rusty/option.hpp>

#include <unordered_map>
#include <unordered_set>
#include <pthread.h>

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

// for getaddrinfo() used in Server::start()
//struct addrinfo;

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
// External annotations for socket operations
// @external: {
//   accept: [safe, (int, sockaddr*, socklen_t*) -> int]
//   socket: [safe, (int, int, int) -> int]
//   bind: [safe, (int, const sockaddr*, socklen_t) -> int]
//   listen: [safe, (int, int) -> int]
//   close: [safe, (int) -> int]
//   getaddrinfo: [safe, (const char*, const char*, const addrinfo*, addrinfo**) -> int]
//   freeaddrinfo: [safe, (addrinfo*) -> void]
//   setsockopt: [safe, (int, int, int, const void*, socklen_t) -> int]
//   set_nonblocking: [safe, (int, bool) -> int]
//   unlink: [safe, (const char*) -> int]
//   strcpy: [safe, (char*, const char*) -> char*]
//   strlen: [safe, (const char*) -> size_t]
//   gai_strerror: [safe, (int) -> const char*]
//   memset: [safe, (void*, int, size_t) -> void*]
// }

// @unsafe - Contains unsafe handle_read() method that calls Log functions
// SAFETY: Thread-safe server listener with proper socket lifecycle management
class ServerListener: public Pollable {
  friend class Server;
 public:
  std:: string addr_;
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

  // @safe - Not implemented, will abort if called
  // Returns MODE_NO_CHANGE since ServerListener never handles write
  int handle_write() override {verify(0); return Pollable::MODE_NO_CHANGE;}

  // @unsafe - Calls unsafe Log::debug for connection logging
  // SAFETY: Thread-safe with server connection lock
  void handle_read() override;

  // @safe - Not implemented, will abort if called
  void handle_error() override {verify(0);}

  // @safe - Closes server socket
  // Close is marked safe via external annotation
  void close();

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

// @unsafe - Handles individual client connections
// SAFETY: Thread-safe with spinlocks, proper Arc lifetime management
class ServerConnection: public Pollable {

    friend class Server;
    friend class ServerListener;

    Marshal in_, out_;
    mutable SpinLock out_l_;

    Marshal block_read_in;

    Server* server_;
    int socket_;

    rusty::Option<rusty::Box<Marshal::bookmark>> bmark_;

    enum {
        CONNECTED, CLOSED
    } status_;

    // Weak pointer to self, initialized after creation
    // Used to pass weak reference to async handlers
    WeakServerConnection weak_self_;

    // get_shared() is now inherited from Pollable base class

    /**
     * Only to be called by:
     * 1: ~Server(), which is called when destroying Server
     * 2: handle_error(), which is called by PollThread
     */
    // @unsafe - Closes connection and cleans up
    // SAFETY: Thread-safe with server connection lock
    void close();

    // used to surpress multiple "no handler for rpc_id=..." errro
    static std::unordered_set<i32> rpc_id_missing_s;
    static SpinLock rpc_id_missing_l_s;

public:

    // Public destructor for shared_ptr compatibility
    // @safe - Simple destructor updating counter
    ~ServerConnection();


    // @unsafe - Initializes connection with socket
    // SAFETY: Increments server connection counter
    ServerConnection(Server* server, int socket);

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
    // @unsafe - Starts reply marshaling
    // SAFETY: Protected by output spinlock
    void begin_reply(const Request& req, i32 error_code = 0);

    // @unsafe - Completes reply packet
    // SAFETY: Protected by output spinlock
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
    int poll_mode() const override;
    // @unsafe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write() override;
    // @unsafe - Reads and processes RPC requests
    // SAFETY: Creates coroutines for handlers
    void handle_read() override;
    // @safe - Error handler
    void handle_error() override;

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

// @safe - RAII wrapper for deferred RPC replies
class DeferredReply: public NoCopy {
    rusty::Box<rrr::Request> req_;
    WeakServerConnection weak_sconn_;
    std::function<void()> marshal_reply_;
    std::function<void()> cleanup_;

public:

    DeferredReply(rusty::Box<rrr::Request> req, WeakServerConnection weak_sconn,
                  const std::function<void()>& marshal_reply, const std::function<void()>& cleanup)
        : req_(std::move(req)), weak_sconn_(weak_sconn), marshal_reply_(marshal_reply), cleanup_(cleanup) {}

    // @safe - Cleanup destructor with automatic cleanup
    // SAFETY: Proper cleanup order, rusty::Box automatically deletes req_
    ~DeferredReply() {
        cleanup_();
        // req_ automatically cleaned up by rusty::Box destructor
    }

    int run_async(const std::function<void()>& f) {
      // TODO disable threadpool run in RPCs.
//        auto sconn = weak_sconn_.lock();
//        if (sconn) return sconn->run_async(f);
      return 0;
    }

    // @unsafe - Sends reply and self-deletes
    // SAFETY: Locks weak_ptr before use, gracefully handles closed connections
    // Uses const_cast because Arc::get() returns const pointer but we need to mutate
    void reply() {
        auto sconn_opt = weak_sconn_.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).begin_reply(*req_);
            marshal_reply_();
            const_cast<ServerConnection&>(*sconn).end_reply();
        } else {
            // Connection closed, silently drop reply
            Log_debug("Connection closed before reply sent, dropping reply");
        }
        delete this;
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
    ThreadPool* threadpool_;
    int server_sock_;

    Counter sconns_ctr_;

    SpinLock sconns_l_;
    std::unordered_set<rusty::Arc<ServerConnection>> sconns_{};
    rusty::Option<rusty::Arc<ServerListener>> sp_server_listener_;

    enum {
        NEW, RUNNING, STOPPING, STOPPED
    } status_;

    pthread_t loop_th_;

    static void* start_server_loop(void* arg);
    void server_loop(struct addrinfo* svr_addr);

public:
    std::string addr_;

    // @unsafe - Creates server with optional PollThread
    // SAFETY: Shared ownership of PollThread via Arc<Mutex<>>
    Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::None, ThreadPool* thrpool = nullptr);
    // @unsafe - Destroys server and all connections
    // SAFETY: Waits for all connections to close
    virtual ~Server();

    // @unsafe - Starts server on specified address
    // SAFETY: Proper socket binding and thread creation
    int start(const char* bind_addr);

    // @safe - Registers service
    int reg(Service* svc) {
        return svc->__reg_to__(this);
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
    // @safe - Registers RPC handler function
    int reg(i32 rpc_id, const RequestHandler& func);

    // @unsafe
    template<class S>
    int reg(i32 rpc_id, S* svc, void (S::*svc_func)(rusty::Box<Request>, WeakServerConnection)) {

        // disallow duplicate rpc_id
        if (handlers_.find(rpc_id) != handlers_.end()) {
            return EEXIST;
        }

        handlers_[rpc_id] = [svc, svc_func] (rusty::Box<Request> req, WeakServerConnection sconn) {
            (svc->*svc_func)(std::move(req), sconn);
        };

        return 0;
    }

    // @safe - Unregisters RPC handler
    void unreg(i32 rpc_id);
};

} // namespace rrr

