// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * udptransport.h:
 *   message-passing network interface that uses UDP message delivery
 *   and libasync
 *
 **********************************************************************/

#ifndef _LIB_FASTTRANSPORT_H_
#define _LIB_FASTTRANSPORT_H_

#include "lib/helper_queue.h"
#include "lib/configuration.h"
#include "lib/transport.h"
#include "lib/message.h"

// include eRPC library
#include "rpc.h"
#include "rpc_constants.h"
#include "util/numautils.h"

#include <gflags/gflags.h>
#include <event2/event.h>

#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <mutex>
#include <atomic>
#include <netinet/in.h>
#include <chrono>

#include <boost/unordered_map.hpp>

void register_fasttransport_for_bench(std::function<int(int,int)>);
void register_fasttransport_for_dbtest(std::function<int(int,int)>);

/*
 * Class FastTransport implements a multi-threaded
 * transport layer based on eRPC which works with
 * a client - server configuration, where the server
 * may have multiple shards.
 *
 * The Register function is used to register a transport
 * receiver. The transport is responsible for sending and
 * dispatching messages from/to its receivers accordingly.
 * A transport receiver can either be a client or a server
 * shard. A transport instance's receivers must be
 * of the same type.
 */

// A tag attached to every request we send;
// it is passed to the response function
struct req_tag_t
{
    erpc::MsgBuffer req_msgbuf;
    erpc::MsgBuffer resp_msgbuf;
    uint8_t reqType;
    TransportReceiver *src;
};

// A basic mempool for preallocated objects of type T. eRPC has a faster,
// hugepage-backed one.
template <class T>
class AppMemPool
{
public:
    size_t num_to_alloc = 1;
    std::vector<T *> backing_ptr_vec;
    std::vector<T *> pool;

    void extend_pool()
    {
        T *backing_ptr = new T[num_to_alloc];
        for (size_t i = 0; i < num_to_alloc; i++)
            pool.push_back(&backing_ptr[i]);
        backing_ptr_vec.push_back(backing_ptr);
        num_to_alloc *= 2;
    }

    T *alloc()
    {
        if (pool.empty())
            extend_pool();
        T *ret = pool.back();
        pool.pop_back();
        return ret;
    }

    void free(T *t) { pool.push_back(t); }

    AppMemPool() {}
    ~AppMemPool()
    {
        for (T *ptr : backing_ptr_vec)
            delete[] ptr;
    }
};

// eRPC context passed between request and responses
class AppContext
{
public:
    struct
    {
        // This is maintained between calls to GetReqBuf and SendRequest
        // to reduce copying
        req_tag_t *crt_req_tag;
        // Request tags used for RPCs exchanged with the servers
        AppMemPool<req_tag_t> req_tag_pool;
        boost::unordered_map<TransportReceiver *, boost::unordered_map<std::tuple<uint8_t, uint8_t, uint16_t>, int>> sessions;
    } client;


    // common to both servers and clients
    erpc::Rpc<erpc::CTransport> *rpc = nullptr;

    // queues for helper threads
    // uint8_t ==> server_id
    std::unordered_map<uint16_t, srolis::HelperQueue*> queue_holders;
    // cached queue for the helper threads
    std::unordered_map<uint16_t, srolis::HelperQueue*> queue_holders_response;

    // monitor the package size
    uint64_t msg_size_req_sent;
    int msg_counter_req_sent;
    uint64_t msg_size_resp_sent;
    int msg_counter_resp_sent;
};

class FastTransport : public Transport
{
public:
    FastTransport(std::string file,
                  std::string &ip,
                  std::string cluster,
                  uint8_t st_nr_req_types,
                  uint8_t end_nr_req_types,
                  uint8_t phy_port,
                  uint8_t numa_node,
                  int shardIdx,
                  uint16_t id);
    void stats();

    int handleTimeout(size_t start_tsc, int req_type, std::string extra);
    void Run();
    void RunNoQueue();
    void Statistics();
    void Stop();
    void setBreakTimeout(bool);
    int Timer(uint64_t ms, timer_callback_t cb) override;
    bool CancelTimer(int id) override;
    void CancelAllTimers() override;

    bool SendRequestToShard(TransportReceiver *src, uint8_t reqType, uint8_t shardIdx, uint16_t dstRpcIdx, size_t msgLen) override;
    bool SendRequestToAll(TransportReceiver *src,
                            uint8_t reqType,
                            int shards_to_send_bit_set,
                            uint16_t id,
                            size_t respMsgLen,
                            size_t reqMsgLen, int forceCenter) override;
    bool SendBatchRequestToAll(
        TransportReceiver *src,
        uint8_t req_type,
        uint16_t id,
        size_t resp_msg_len,
        const std::map<int, std::pair<char*, size_t>> &data_to_send
    ) override;
    char *GetRequestBuf(size_t reqLen, size_t respLen) override;
    int GetSession(TransportReceiver *src, uint8_t replicaIdx, uint16_t dstRpcIdx, int forceCenter) override;

    uint16_t GetID() override { return id; };

    AppContext *c;

private:
    // Configuration of the shards
    transport::Configuration config;

    // The port of the fast NIC
    uint8_t phy_port;

    // numa node on which this transport thread is running
    uint8_t numa_node;

    // Index of the shard running on
    int shardIdx;
    uint16_t id;
    std::string cluster;
    int clusterRole; // 0: localhost, 1: learner, 2: p1, 3: p2

    // Nexus object
    erpc::Nexus *nexus;

    struct FastTransportTimerInfo
    {
        FastTransport *transport;
        timer_callback_t cb;
        event *ev;
        int id;
    };

    event_base *eventBase;
    std::vector<event *> signalEvents;
    bool stop = false;
    // if the old leader is killed, the alive leader partitions have to break the timeout
    bool breakTimeout = false;

    uint64_t lastTimerId;
    using timers_map = std::map<int, FastTransportTimerInfo *>;
    timers_map timers;
    std::mutex timers_lock;
    double freq_ghz_;
    bool isUpdateConfig;

    int ms1_cycles;

    size_t start_transport;
    std::chrono::high_resolution_clock::time_point start_transport_clock;

    void OnTimer(FastTransportTimerInfo *info);
    // static void SocketCallback(evutil_socket_t fd, short what, void *arg);
    static void TimerCallback(evutil_socket_t fd, short what, void *arg);
    static void LogCallback(int severity, const char *msg);
    static void FatalCallback(int err);
    static void SignalCallback(evutil_socket_t fd, short what, void *arg);
};

// A basic session management handler that expects successful responses
static void basic_sm_handler(int session_num, erpc::SmEventType sm_event_type,
                             erpc::SmErrType sm_err_type, void *_context)
{

    auto *c = static_cast<AppContext *>(_context);

    Assert(sm_err_type == erpc::SmErrType::kNoError);
    if (!(sm_event_type == erpc::SmEventType::kConnected ||
          sm_event_type == erpc::SmEventType::kDisconnected))
    {
        throw std::runtime_error("Received unexpected SM event.");
    }

    Debug("Rpc %u: Session number %d %s. Error %s. "
          "Time elapsed = %.3f s.\n",
          c->rpc->get_rpc_id(), session_num,
          erpc::sm_event_type_str(sm_event_type).c_str(),
          erpc::sm_err_type_str(sm_err_type).c_str(),
          c->rpc->sec_since_creation());
}

#endif // _LIB_FASTTRANSPORT_H_
