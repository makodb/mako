#pragma once

#include <unordered_map>

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
//   gai_strerror: [safe, (int) -> const char*]
//   memset: [unsafe, (void*, int, size_t) -> void*]
//   strcpy: [unsafe, (char*, const char*) -> char*]
// }

namespace rrr {

class Future;
class Client;

// @safe - Simple attribute struct for Future callbacks
struct FutureAttr {
    FutureAttr(const std::function<void(Future*)>& cb = std::function<void(Future*)>()) : callback(cb) { }

    // callback should be fast, otherwise it hurts rpc performance
    std::function<void(Future*)> callback;
};

// @safe - Thread-safe future for async RPC results
class Future: public RefCounted {
    friend class Client;

    i64 xid_;
    i32 error_code_;

    FutureAttr attr_;
    Marshal reply_;

    bool ready_;
    bool timed_out_;
    pthread_cond_t ready_cond_;
    pthread_mutex_t ready_m_;

    // @unsafe - Notifies waiters and triggers callbacks
    // SAFETY: Protected by mutex, callback executed in coroutine
    void notify_ready();

protected:

    // protected destructor as required by RefCounted.
    // @unsafe - Destroys pthread primitives
    // SAFETY: Called only when refcount reaches zero
    ~Future() {
        Pthread_mutex_destroy(&ready_m_);
        Pthread_cond_destroy(&ready_cond_);
    }

public:

    // @unsafe - Initializes pthread primitives
    // SAFETY: Mutex and condvar properly destroyed in destructor
    Future(i64 xid, const FutureAttr& attr = FutureAttr())
            : xid_(xid), error_code_(0), attr_(attr), ready_(false), timed_out_(false) {
        Pthread_mutex_init(&ready_m_, nullptr);
        Pthread_cond_init(&ready_cond_, nullptr);
    }

    // @unsafe - Thread-safe ready check
    // SAFETY: Protected by mutex
    bool ready() {
        Pthread_mutex_lock(&ready_m_);
        bool r = ready_;
        Pthread_mutex_unlock(&ready_m_);
        return r;
    }

    // wait till reply done
    // @unsafe - Blocks on condition variable
    // SAFETY: Proper pthread condvar usage
    void wait();

    // @unsafe - Timed wait with timeout
    // SAFETY: Proper pthread timed wait usage
    void timed_wait(double sec);

    Marshal& get_reply() {
        wait();
        return reply_;
    }

    i32 get_error_code() {
        wait();
        return error_code_;
    }

    // @safe - Null-safe release helper
    static inline void safe_release(Future* fu) {
        if (fu != nullptr) {
            fu->release();
        }
    }
};

// @safe - RAII container for managing multiple futures
class FutureGroup {
private:
    std::vector<Future*> futures_;

public:
    void add(Future* f) {
        if (f == nullptr) {
            Log_error("Invalid Future object passed to FutureGroup!");
            return;
        }
        futures_.push_back(f);
    }

    void wait_all() {
        for (auto& f : futures_) {
            f->wait();
        }
    }

    ~FutureGroup() {
        wait_all();
        for (auto& f : futures_) {
            f->release();
        }
    }
};

// @unsafe - RPC client with socket management and marshaling
// SAFETY: Proper socket lifecycle and thread-safe pending futures
class Client: public Pollable {
    Marshal in_, out_;

    /**
     * NOT a refcopy! This is intended to avoid circular reference, which prevents everything from being released correctly.
     */
    PollMgr* pollmgr_;

    int sock_;
    enum {
        NEW, CONNECTED, CLOSED
    } status_;

    Marshal::bookmark* bmark_;

    Counter xid_counter_;
    std::unordered_map<i64, Future*> pending_fu_;

    SpinLock pending_fu_l_;
    SpinLock out_l_;

    // reentrant, could be called multiple times before releasing
    // @unsafe - Closes socket and cleans up
    // SAFETY: Idempotent, properly invalidates futures
    void close();

    // @unsafe - Cancels all pending futures
    // SAFETY: Protected by spinlock
    void invalidate_pending_futures();

protected:

    // @unsafe - Cleanup destructor
    // SAFETY: Ensures all futures are invalidated
    virtual ~Client() {
        invalidate_pending_futures();
    }

public:

    Client(PollMgr* pollmgr): pollmgr_(pollmgr), sock_(-1), status_(NEW), bmark_(nullptr) { }

    /**
     * Start a new request. Must be paired with end_request(), even if nullptr returned.
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     */
    // @unsafe - Begins RPC request with marshaling
    // SAFETY: Protected by spinlock, returns refcounted Future
    Future* begin_request(i32 rpc_id, const FutureAttr& attr = FutureAttr());

    // @unsafe - Completes request packet
    // SAFETY: Must be called after begin_request
    void end_request();

    template<class T>
    Client& operator <<(const T& v) {
        if (status_ == CONNECTED) {
            this->out_ << v;
        }
        return *this;
    }

    // NOTE: this function is used *internally* by Python extension
    Client& operator <<(Marshal& m) {
        if (status_ == CONNECTED) {
            this->out_.read_from_marshal(m, m.content_size());
        }
        return *this;
    }

    // @unsafe - Establishes TCP connection
    // SAFETY: Proper socket creation and cleanup on failure
    int connect(const char* addr);

    void close_and_release() {
        close();
        release();
    }

    int fd() {
        return sock_;
    }

    // @safe - Returns current poll mode based on output buffer
    int poll_mode();
    // @unsafe - Processes incoming data
    // SAFETY: Protected by spinlock for pending futures
    void handle_read();
    // @unsafe - Sends buffered data
    // SAFETY: Protected by output spinlock
    void handle_write();
    // @safe - Error handler that closes connection
    void handle_error();

};

// @safe - Thread-safe pool of client connections
class ClientPool: public NoCopy {
    rrr::Rand rand_;

    // refcopy
    rrr::PollMgr* pollmgr_;

    // guard cache_
    SpinLock l_;
    std::map<std::string, rrr::Client**> cache_;
    int parallel_connections_;

public:

    // @unsafe - Creates pool with optional PollMgr
    // SAFETY: Proper refcounting of PollMgr
    ClientPool(rrr::PollMgr* pollmgr = nullptr, int parallel_connections = 1);
    // @unsafe - Closes all cached connections
    // SAFETY: Properly releases all clients and PollMgr
    ~ClientPool();

    // return cached client connection
    // on error, return nullptr
    // @unsafe - Gets or creates client connection
    // SAFETY: Protected by spinlock, handles connection failures
    rrr::Client* get_client(const std::string& addr);

};

}
