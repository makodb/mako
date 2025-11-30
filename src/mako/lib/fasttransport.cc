// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * fasttransport.cc:
 *   High-performance transport layer with pluggable backends
 *
 **********************************************************************/

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/fasttransport.h"
#include "lib/common.h"
#include "lib/erpc_backend.h"
#include "lib/rrr_rpc_backend.h"

#include <google/protobuf/message.h>
#include <event2/event.h>
#include <event2/thread.h>

#include <memory>
#include <random>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <thread>

static std::mutex fasttransport_lock;
static volatile bool fasttransport_initialized = false;

std::function<int(int,int)> bench_callback_ = nullptr;
void register_fasttransport_for_bench(std::function<int(int,int)> cb) {
    bench_callback_ = cb;
}

std::function<int(int,int)> dbtest_callback_ = nullptr;
void register_fasttransport_for_dbtest(std::function<int(int,int)> cb) {
    dbtest_callback_ = cb;
}

FastTransport::FastTransport(std::string file,
                             std::string &ip,
                             std::string cluster,
                             uint8_t st_nr_req_types,
                             uint8_t end_nr_req_types,
                             uint8_t phy_port,
                             uint8_t numa_node,
                             int shardIdx,
                             uint16_t id)
    : config_(file),
      shard_idx_(shardIdx),
      id_(id),
      cluster_(cluster)
{
    // Initialize libevent for timers (first time only)
    fasttransport_lock.lock();
    if (fasttransport_initialized)
    {
        // Create the event_base to schedule requests
        eventBase = event_base_new();
        evthread_make_base_notifiable(eventBase);
    }
    else
    {
        // Setup libevent
        evthread_use_pthreads();
        event_set_log_callback(LogCallback);
        event_set_fatal_callback(FatalCallback);

        // Create the event_base to schedule requests
        eventBase = event_base_new();
        evthread_make_base_notifiable(eventBase);

        for (event *x : signalEvents)
        {
            event_add(x, NULL);
        }

        fasttransport_initialized = true;
    }
    fasttransport_lock.unlock();

    // Load transport configuration
    config_.LoadTransportConfig();

    // Create transport backend based on configuration
    switch (config_.transport_type) {
        case mako::TransportType::ERPC:
            backend_ = new mako::ErpcBackend(config_, shardIdx, id, cluster);
            break;

        case mako::TransportType::RRR_RPC:
            backend_ = new mako::RrrRpcBackend(config_, shardIdx, id, cluster);
            break;

        default:
            Panic("Unknown transport type");
    }

    // Initialize the backend
    int port = std::atoi(config_.shard(shardIdx, mako::convertCluster(cluster)).port.c_str());
    std::string local_uri = ip + ":" + std::to_string(port + id);

    int ret = backend_->Initialize(local_uri, numa_node, phy_port,
                                    st_nr_req_types, end_nr_req_types);
    if (ret != 0) {
        Panic("Failed to initialize transport backend");
    }

    Notice("FastTransport initialized with %s backend on %s",
           backend_->GetName(), local_uri.c_str());
}

FastTransport::~FastTransport() {
    if (backend_) {
        backend_->Shutdown();
        delete backend_;
        backend_ = nullptr;
    }

    if (eventBase) {
        event_base_free(eventBase);
        eventBase = nullptr;
    }
}

void FastTransport::stats() {
    if (backend_) {
        backend_->PrintStats();
    }
}

void FastTransport::Statistics() {
    if (backend_) {
        backend_->PrintStats();
    }
}

char *FastTransport::GetRequestBuf(size_t reqLen, size_t respLen)
{
    Assert(backend_ != nullptr);
    return backend_->AllocRequestBuffer(reqLen, respLen);
}

int FastTransport::GetSession(TransportReceiver *src, uint8_t dstShardIdx,
                               uint16_t id, int forceCenter)
{
    Assert(backend_ != nullptr);

    // For eRPC backend, delegate directly
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        return erpc_backend->GetSession(src, dstShardIdx, id, forceCenter);
    }

    // For other backends, session management is internal
    return 0;
}

bool FastTransport::SendRequestToShard(TransportReceiver *src,
                                       uint8_t reqType,
                                       uint8_t shardIdx,
                                       uint16_t id,
                                       size_t msgLen)
{
    Assert(backend_ != nullptr);
    return backend_->SendToShard(src, reqType, shardIdx, id, msgLen);
}

bool FastTransport::SendRequestToAll(TransportReceiver *src,
                                     uint8_t reqType,
                                     int shards_to_send_bit_set,
                                     uint16_t id,
                                     size_t respMsgLen,
                                     size_t reqMsgLen,
                                     int forceCenter)
{
    Assert(backend_ != nullptr);
    return backend_->SendToAll(src, reqType, shards_to_send_bit_set,
                               id, respMsgLen, reqMsgLen, forceCenter);
}

bool FastTransport::SendBatchRequestToAll(
    TransportReceiver *src,
    uint8_t req_type,
    uint16_t id,
    size_t resp_msg_len,
    const std::map<int, std::pair<char*, size_t>> &data_to_send)
{
    Assert(backend_ != nullptr);
    return backend_->SendBatchToAll(src, req_type, id, resp_msg_len, data_to_send);
}

void FastTransport::RunNoQueue()
{
    Assert(backend_ != nullptr);

    // For eRPC backend with special RunNoQueue implementation
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        erpc_backend->RunNoQueue();
    } else {
        backend_->RunEventLoop();
    }
}

void FastTransport::Run()
{
    Assert(backend_ != nullptr);
    backend_->RunEventLoop();
}

void FastTransport::Stop()
{
    Assert(backend_ != nullptr);
    backend_->Stop();
}

void FastTransport::setBreakTimeout(bool bt)
{
    Assert(backend_ != nullptr);

    // For eRPC backend with break timeout support
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        erpc_backend->SetBreakTimeout(bt);
    }
}

void FastTransport::SetHelperQueues(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues)
{
    Assert(backend_ != nullptr);

    // For eRPC backend
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        erpc_backend->SetHelperQueues(queues);
    }
    // For rrr/rpc backend
    else if (backend_->GetType() == mako::TransportType::RRR_RPC) {
        auto* rrr_backend = static_cast<mako::RrrRpcBackend*>(backend_);
        rrr_backend->SetHelperQueues(queues);
    }
}

void FastTransport::SetHelperQueuesResponse(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues)
{
    Assert(backend_ != nullptr);

    // For eRPC backend
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        erpc_backend->SetHelperQueuesResponse(queues);
    }
    // For rrr/rpc backend
    else if (backend_->GetType() == mako::TransportType::RRR_RPC) {
        auto* rrr_backend = static_cast<mako::RrrRpcBackend*>(backend_);
        rrr_backend->SetHelperQueuesResponse(queues);
    }
}

mako::HelperQueue* FastTransport::GetHelperQueue(uint16_t id)
{
    Assert(backend_ != nullptr);

    // For eRPC backend
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        const auto& queues = erpc_backend->GetHelperQueues();
        auto it = queues.find(id);
        return (it != queues.end()) ? it->second : nullptr;
    }
    // For rrr/rpc backend
    else if (backend_->GetType() == mako::TransportType::RRR_RPC) {
        auto* rrr_backend = static_cast<mako::RrrRpcBackend*>(backend_);
        const auto& queues = rrr_backend->GetHelperQueues();
        auto it = queues.find(id);
        return (it != queues.end()) ? it->second : nullptr;
    }

    return nullptr;
}

mako::HelperQueue* FastTransport::GetHelperQueueResponse(uint16_t id)
{
    Assert(backend_ != nullptr);

    // For eRPC backend
    if (backend_->GetType() == mako::TransportType::ERPC) {
        auto* erpc_backend = static_cast<mako::ErpcBackend*>(backend_);
        const auto& queues = erpc_backend->GetHelperQueuesResponse();
        auto it = queues.find(id);
        return (it != queues.end()) ? it->second : nullptr;
    }
    // For rrr/rpc backend
    else if (backend_->GetType() == mako::TransportType::RRR_RPC) {
        auto* rrr_backend = static_cast<mako::RrrRpcBackend*>(backend_);
        const auto& queues = rrr_backend->GetHelperQueuesResponse();
        auto it = queues.find(id);
        return (it != queues.end()) ? it->second : nullptr;
    }

    return nullptr;
}

// ===== Timer Implementation (transport-independent) =====

int FastTransport::Timer(uint64_t ms, timer_callback_t cb)
{
    FastTransportTimerInfo *info = new FastTransportTimerInfo();

    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    timers_lock.lock();
    uint64_t t_id = lastTimerId;
    lastTimerId++;
    timers_lock.unlock();

    info->transport = this;
    info->id = t_id;
    info->cb = cb;
    info->ev = event_new(eventBase, -1, 0,
                         TimerCallback, info);

    if (info->ev == NULL)
    {
        Debug("Error creating new Timer event : %lu", t_id);
    }

    timers_lock.lock();
    timers[info->id] = info;
    timers_lock.unlock();

    int ret = event_add(info->ev, &tv);
    if (ret != 0)
    {
        Debug("Error adding new Timer event to eventbase %lu", t_id);
    }

    return info->id;
}

bool FastTransport::CancelTimer(int id)
{
    FastTransportTimerInfo *info = timers[id];

    if (info == NULL)
    {
        return false;
    }

    event_del(info->ev);
    event_free(info->ev);

    timers_lock.lock();
    timers.erase(info->id);
    timers_lock.unlock();

    delete info;

    return true;
}

void FastTransport::CancelAllTimers()
{
    Debug("Cancelling all Timers");
    while (!timers.empty())
    {
        auto kv = timers.begin();
        CancelTimer(kv->first);
    }
}

void FastTransport::OnTimer(FastTransportTimerInfo *info)
{
    timers_lock.lock();
    timers.erase(info->id);
    timers_lock.unlock();

    event_del(info->ev);
    event_free(info->ev);

    info->cb();

    delete info;
}

void FastTransport::TimerCallback(evutil_socket_t fd, short what, void *arg)
{
    FastTransport::FastTransportTimerInfo *info =
        (FastTransport::FastTransportTimerInfo *)arg;

    ASSERT(what & EV_TIMEOUT);

    info->transport->OnTimer(info);
}

void FastTransport::LogCallback(int severity, const char *msg)
{
    Message_Type msgType;
    switch (severity)
    {
    case _EVENT_LOG_DEBUG:
        msgType = MSG_DEBUG;
        break;
    case _EVENT_LOG_MSG:
        msgType = MSG_NOTICE;
        break;
    case _EVENT_LOG_WARN:
        msgType = MSG_WARNING;
        break;
    case _EVENT_LOG_ERR:
        msgType = MSG_WARNING;
        break;
    default:
        NOT_REACHABLE();
    }

    _Message(msgType, "libevent", 0, NULL, "%s", msg);
}

void FastTransport::FatalCallback(int err)
{
    Panic("Fatal libevent error: %d", err);
}

void FastTransport::SignalCallback(evutil_socket_t fd,
                                   short what, void *arg)
{
    Notice("Terminating on SIGTERM/SIGINT");
    FastTransport *transport = (FastTransport *)arg;
    transport->Stop();
}
