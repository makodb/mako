// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * fasttransport.h:
 *   High-performance transport layer with pluggable backends
 *   Supports eRPC (RDMA) and rrr/rpc (TCP/IP)
 *
 **********************************************************************/

#ifndef _LIB_FASTTRANSPORT_H_
#define _LIB_FASTTRANSPORT_H_

#include "lib/helper_queue.h"
#include "lib/configuration.h"
#include "lib/transport.h"
#include "lib/message.h"
#include "lib/transport_backend.h"

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

void register_fasttransport_for_bench(std::function<int(int,int)>);
void register_fasttransport_for_dbtest(std::function<int(int,int)>);

/*
 * Class FastTransport implements a multi-threaded transport layer
 * with pluggable backends (eRPC for RDMA, rrr/rpc for TCP/IP).
 *
 * This class delegates to a TransportBackend implementation selected
 * at runtime based on configuration. It maintains API compatibility
 * with existing code while allowing transport switching.
 *
 * The Register function is used to register a transport receiver.
 * The transport is responsible for sending and dispatching messages
 * from/to its receivers accordingly. A transport receiver can either
 * be a client or a server shard. A transport instance's receivers
 * must be of the same type.
 */

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

    virtual ~FastTransport();

    void stats();
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

    uint16_t GetID() override { return id_; };

    // Set/Get helper queues for backend (used by server)
    void SetHelperQueues(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues);
    void SetHelperQueuesResponse(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues);

    mako::HelperQueue* GetHelperQueue(uint16_t id);
    mako::HelperQueue* GetHelperQueueResponse(uint16_t id);

private:
    // Transport backend (eRPC, rrr/rpc, etc.)
    mako::TransportBackend* backend_{nullptr};

    // Configuration of the shards
    transport::Configuration config_;

    // Index of the shard running on
    int shard_idx_;
    uint16_t id_;
    std::string cluster_;

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

#endif // _LIB_FASTTRANSPORT_H_
