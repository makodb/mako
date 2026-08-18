// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * fasttransport.cc:
 *   Transport layer over the rrr/rpc (TCP/IP) backend
 *
 **********************************************************************/

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/fasttransport.h"
#include "lib/common.h"
#include "lib/rrr_rpc_backend.h"

#include <event2/event.h>
#include <event2/thread.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>

import std;

static std::mutex fasttransport_lock;
static volatile bool fasttransport_initialized = false;
static bool fasttransport_signal_handlers_enabled = true;

std::function<int(int,int)> bench_callback_ = nullptr;
void register_fasttransport_for_bench(std::function<int(int,int)> cb) {
    bench_callback_ = cb;
}

std::function<int(int,int)> dbtest_callback_ = nullptr;
void register_fasttransport_for_dbtest(std::function<int(int,int)> cb) {
    dbtest_callback_ = cb;
}

void set_fasttransport_signal_handlers_enabled(bool enabled) {
    fasttransport_lock.lock();
    fasttransport_signal_handlers_enabled = enabled;
    fasttransport_lock.unlock();
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

        if (fasttransport_signal_handlers_enabled) {
            // Create signal handlers for graceful shutdown
            // @unsafe - evsignal_new calls non-borrow-checked libevent code
            event *sigterm_event = evsignal_new(eventBase, SIGTERM, SignalCallback, this);
            event *sigint_event = evsignal_new(eventBase, SIGINT, SignalCallback, this);
            if (sigterm_event) {
                signalEvents.push_back(sigterm_event);
            }
            if (sigint_event) {
                signalEvents.push_back(sigint_event);
            }

            for (event *x : signalEvents)
            {
                event_add(x, NULL);
            }
        }

        fasttransport_initialized = true;
    }
    fasttransport_lock.unlock();

    // Create the transport backend
    backend_ = new mako::RrrRpcBackend(config_, shardIdx, id, cluster);

    // Initialize the backend
    int port = std::atoi(config_.shard(shardIdx, mako::convertCluster(cluster)).port.c_str());
    std::string local_uri = ip + ":" + std::to_string(port + id);

    int ret = backend_->Initialize(local_uri, numa_node, phy_port,
                                    st_nr_req_types, end_nr_req_types);
    if (ret != 0) {
        Panic("Failed to initialize transport backend");
    }

    Notice("FastTransport initialized with rrr/rpc backend on %s",
           local_uri.c_str());
}

FastTransport::~FastTransport() {
    // Set shutdown flag first to prevent any new accesses
    shutting_down_.store(true, std::memory_order_release);

    // Acquire lock to ensure no concurrent stats() calls in progress
    {
        std::lock_guard<std::mutex> guard(backend_mutex_);
        if (backend_) {
            backend_->Shutdown();
            delete backend_;
            backend_ = nullptr;
        }
    }

    // Clean up signal event handlers
    // @unsafe - event_free calls non-borrow-checked libevent code
    for (event *ev : signalEvents) {
        if (ev) {
            event_del(ev);
            event_free(ev);
        }
    }
    signalEvents.clear();

    if (eventBase) {
        event_base_free(eventBase);
        eventBase = nullptr;
    }
}

// @safe - Thread-safe stats access
void FastTransport::stats() {
    // Check shutdown flag first (relaxed ordering OK - just an optimization)
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }

    // Acquire lock to prevent race with destructor
    std::lock_guard<std::mutex> guard(backend_mutex_);
    if (backend_) {
        backend_->PrintStats();
    }
}

// @safe - Thread-safe statistics access
void FastTransport::Statistics() {
    // Check shutdown flag first (relaxed ordering OK - just an optimization)
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }

    // Acquire lock to prevent race with destructor
    std::lock_guard<std::mutex> guard(backend_mutex_);
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
    (void)src;
    (void)dstShardIdx;
    (void)id;
    (void)forceCenter;

    // rrr/rpc manages sessions internally.
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
    backend_->RunEventLoop();
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
    // rrr/rpc has no break-timeout hook.
    (void)bt;
}

void FastTransport::SetHelperQueues(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues)
{
    Assert(backend_ != nullptr);
    backend_->SetHelperQueues(queues);
}

void FastTransport::SetHelperQueuesResponse(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues)
{
    Assert(backend_ != nullptr);
    backend_->SetHelperQueuesResponse(queues);
}

mako::HelperQueue* FastTransport::GetHelperQueue(uint16_t id)
{
    Assert(backend_ != nullptr);

    const auto& queues = backend_->GetHelperQueues();
    auto it = queues.find(id);
    return (it != queues.end()) ? it->second : nullptr;
}

mako::HelperQueue* FastTransport::GetHelperQueueResponse(uint16_t id)
{
    Assert(backend_ != nullptr);

    const auto& queues = backend_->GetHelperQueuesResponse();
    auto it = queues.find(id);
    return (it != queues.end()) ? it->second : nullptr;
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
