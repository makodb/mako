#pragma once

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
  int poll_mode() {
    return Pollable::READ;
  }
  
  // @safe - Not implemented, will abort if called
  void handle_write() {verify(0);}
  
  // @unsafe - Calls unsafe Log::debug for connection logging
  // SAFETY: Thread-safe with server connection lock
  void handle_read();
  
  // @safe - Not implemented, will abort if called
  void handle_error() {verify(0);}
  
  // @safe - Closes server socket
  // Close is marked safe via external annotation
  void close();
  
  // @safe - Returns file descriptor
  int fd() {return server_sock_;}
  
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

// @unsafe - Handles individual client connections
// SAFETY: Thread-safe with spinlocks, proper refcounting
class ServerConnection: public Pollable {

    friend class Server;

    Marshal in_, out_;
    SpinLock out_l_;

    Marshal block_read_in;

    Server* server_;
    int socket_;

    Marshal::bookmark* bmark_;

    enum {
        CONNECTED, CLOSED
    } status_;

    /**
     * Only to be called by:
     * 1: ~Server(), which is called when destroying Server
     * 2: handle_error(), which is called by PollMgr
     */
    // @unsafe - Closes connection and cleans up
    // SAFETY: Thread-safe with server connection lock
    void close();

    // used to surpress multiple "no handler for rpc_id=..." errro
    static std::unordered_set<i32> rpc_id_missing_s;
    static SpinLock rpc_id_missing_l_s;

protected:

    // Protected destructor as required by RefCounted.
    // @safe - Simple destructor updating counter
    ~ServerConnection();

public:

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
    void begin_reply(Request* req, i32 error_code = 0);

    // @unsafe - Completes reply packet
    // SAFETY: Protected by output spinlock
    void end_reply();

    // helper function, do some work in background
    int run_async(const std::function<void()>& f);

    template<class T>
    ServerConnection& operator <<(const T& v) {
        this->out_ << v;
        return *this;
    }

    ServerConnection& operator <<(Marshal& m) {
        this->out_.read_from_marshal(m, m.content_size());
        return *this;
    }

    int fd() {
        return socket_;
    }

    // @safe - Returns poll mode based on output buffer
    int poll_mode();
    // @unsafe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock
    void handle_write();
    // @unsafe - Reads and processes RPC requests
    // SAFETY: Creates coroutines for handlers
    void handle_read();
    // @safe - Error handler
    void handle_error();
};

// @safe - RAII wrapper for deferred RPC replies
class DeferredReply: public NoCopy {
    rrr::Request* req_;
    rrr::ServerConnection* sconn_;
    std::function<void()> marshal_reply_;
    std::function<void()> cleanup_;

public:

    DeferredReply(rrr::Request* req, rrr::ServerConnection* sconn,
                  const std::function<void()>& marshal_reply, const std::function<void()>& cleanup)
        : req_(req), sconn_(sconn), marshal_reply_(marshal_reply), cleanup_(cleanup) {}

    // @unsafe - Cleanup destructor
    // SAFETY: Proper cleanup order and null checks
    ~DeferredReply() {
        cleanup_();
        delete req_;
        sconn_->release();
        req_ = nullptr;
        sconn_ = nullptr;
    }

    int run_async(const std::function<void()>& f) {
      // TODO disable threadpool run in RPCs.
//        return sconn_->run_async(f);
      return 0;
    }

    // @unsafe - Sends reply and self-deletes
    // SAFETY: Ensures single use with delete this
    void reply() {
        sconn_->begin_reply(req_);
        marshal_reply_();
        sconn_->end_reply();
        delete this;
    }
};

// @unsafe - Main RPC server managing connections
// SAFETY: Thread-safe connection management with spinlocks
class Server: public NoCopy {
    friend class ServerConnection;
 public:
    std::unordered_map<i32, std::function<void(Request*, ServerConnection*)>> handlers_;
    PollMgr* pollmgr_;
    ThreadPool* threadpool_;
    int server_sock_;

    Counter sconns_ctr_;

    SpinLock sconns_l_;
    std::unordered_set<ServerConnection*> sconns_{};
    std::unique_ptr<ServerListener> up_server_listener_{};

    enum {
        NEW, RUNNING, STOPPING, STOPPED
    } status_;

    pthread_t loop_th_;

    static void* start_server_loop(void* arg);
    void server_loop(struct addrinfo* svr_addr);

public:
    std::string addr_;

    // @unsafe - Creates server with optional PollMgr
    // SAFETY: Proper refcounting of PollMgr
    Server(PollMgr* pollmgr = nullptr, ThreadPool* thrpool = nullptr);
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
     *     server_connection->begin_reply();
     *     *server_connection << {reply_content};
     *     server_connection->end_reply();
     *
     *     // cleanup resource
     *     delete request;
     *     server_connection->release();
     *  }
     */
    // @safe - Registers RPC handler function
    int reg(i32 rpc_id, const std::function<void(Request*, ServerConnection*)>& func);

    template<class S>
    int reg(i32 rpc_id, S* svc, void (S::*svc_func)(Request*, ServerConnection*)) {

        // disallow duplicate rpc_id
        if (handlers_.find(rpc_id) != handlers_.end()) {
            return EEXIST;
        }

        handlers_[rpc_id] = [svc, svc_func] (Request* req, ServerConnection* sconn) {
            (svc->*svc_func)(req, sconn);
        };

        return 0;
    }

    // @safe - Unregisters RPC handler
    void unreg(i32 rpc_id);
};

} // namespace rrr

