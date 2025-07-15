// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * udptransport.cc:
 *   message-passing network interface that uses UDP message delivery
 *   and libasync
 *
 **********************************************************************/

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/fasttransport.h"
#include "lib/common.h"

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
#include <sched.h>
#include <util.h>

#include <numa.h>
#include <boost/fiber/all.hpp>
#include "lib/common.h"
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/sto/Transaction.hh"

static std::mutex fasttransport_lock;
static volatile bool fasttransport_initialized = false;

std::function<int(int,int)> bench_callback_ = nullptr;
void register_fasttransport_for_bench(std::function<int(int,int)> cb) {
    //Warning("register register bench_callback_");
    bench_callback_ = cb;
}

std::function<int(int,int)> dbtest_callback_ = nullptr;
void register_fasttransport_for_dbtest(std::function<int(int,int)> cb) {
    dbtest_callback_ = cb;
}

// Function called when we received a response to a
// request we sent on this transport - client side
static void fasttransport_response(void *_context, void *_tag)
{
    auto *c = static_cast<AppContext *>(_context);
    auto *rt = reinterpret_cast<req_tag_t *>(_tag);
    rt->src->ReceiveResponse(rt->reqType,
                             reinterpret_cast<char *>(rt->resp_msgbuf.buf_));
    c->rpc->free_msg_buffer(rt->req_msgbuf);
    c->rpc->free_msg_buffer(rt->resp_msgbuf);
    c->client.req_tag_pool.free(rt);
}

// Function called when we received a request - server side
static void fasttransport_request(erpc::ReqHandle *req_handle, void *_context)
{
    // save the req_handle for when we are in the SendMessage function
    auto *c = static_cast<AppContext *>(_context);

    // handle the watermark exchange request
    if (req_handle->get_req_msgbuf()->get_req_type() == srolis::watermarkReqType) {
        Debug("received a watermarkReqType");
        char *reqBuf = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
        char *respBuf = reinterpret_cast<char *>(req_handle->pre_resp_msgbuf_.buf_);
        auto *req = reinterpret_cast<srolis::basic_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<srolis::get_int_response_t *>(respBuf);
        resp->result = sync_util::sync_logger::retrieveShardW();
        resp->req_nr = req->req_nr;
        resp->status = srolis::ErrorCode::SUCCESS;
        resp->shard_index = TThread::get_shard_index();
        auto &respX = req_handle->pre_resp_msgbuf_;
        c->msg_size_resp_sent += sizeof(srolis::get_int_response_t);
        c->msg_counter_resp_sent += 1;
        c->rpc->resize_msg_buffer(&respX, sizeof(srolis::get_int_response_t));
        c->rpc->enqueue_response(req_handle, &respX);
    } else if (req_handle->get_req_msgbuf()->get_req_type() == srolis::warmupReqType) {
        char *reqBuf = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
        char *respBuf = reinterpret_cast<char *>(req_handle->pre_resp_msgbuf_.buf_);
        auto *resp = reinterpret_cast<srolis::get_int_response_t *>(respBuf);
        auto *req = reinterpret_cast<srolis::warmup_request_t *>(reqBuf);
        resp->result = 1;
        resp->req_nr = req->req_nr;
        resp->status = srolis::ErrorCode::SUCCESS;
        resp->shard_index = TThread::get_shard_index();
        auto &respX = req_handle->pre_resp_msgbuf_;
        c->msg_size_resp_sent += sizeof(srolis::get_int_response_t);
        c->msg_counter_resp_sent += 1;
        c->rpc->resize_msg_buffer(&respX, sizeof(srolis::get_int_response_t));
        c->rpc->enqueue_response(req_handle, &respX);
    } else if (req_handle->get_req_msgbuf()->get_req_type() == srolis::controlReqType) {
        char *reqBuf = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
        char *respBuf = reinterpret_cast<char *>(req_handle->pre_resp_msgbuf_.buf_);
        auto *req = reinterpret_cast<srolis::control_request_t*>(reqBuf);
        auto *resp = reinterpret_cast<srolis::get_int_response_t *>(respBuf);
        Warning("# received a controlReqType, control: %d, shardIndex: %lld, targert_server_id: %llu", req->control, req->value, req->targert_server_id);
        
        bool is_datacenter_failure = req->targert_server_id == 10000;
        // callback in the dbtest.cc
        if (is_datacenter_failure)
            dbtest_callback_(req->control, req->value);
        else
            bench_callback_(req->control, req->value); // register_fasttransport_for_bench in bench.cc
        resp->result = 0;
        resp->req_nr = req->req_nr;
        resp->status = srolis::ErrorCode::SUCCESS;
        resp->shard_index = TThread::get_shard_index();
        auto &respX = req_handle->pre_resp_msgbuf_;
        c->msg_size_resp_sent += sizeof(srolis::get_int_response_t);
        c->msg_counter_resp_sent += 1;
        c->rpc->resize_msg_buffer(&respX, sizeof(srolis::get_int_response_t));
        c->rpc->enqueue_response(req_handle, &respX);
        //Warning("# received a controlReqType(back), control: %d, shardIndex: %lld", req->control, req->value);
    }
    else {
        auto *target_server_id_reader = (srolis::TargetServerIDReader *)req_handle->get_req_msgbuf()->buf_;
        auto *helper_queue = c->queue_holders[target_server_id_reader->targert_server_id];
        helper_queue->add_one_req(req_handle, 0);
    }
}

void FastTransport::Statistics() {
    Notice("one Transport msg_size_req_sent: %d bytes, counter: %d, avg: %lf", c->msg_size_req_sent, c->msg_counter_req_sent, c->msg_size_req_sent/(c->msg_counter_req_sent+0.0));
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
    : config(file),
      shardIdx(shardIdx),
      cluster(cluster),
      phy_port(phy_port),
      numa_node(numa_node),
      id(id)
{
    Assert(numa_node <= numa_max_node());
    c = new AppContext();
    freq_ghz_ = srolis::measure_rdtsc_freq();
    isUpdateConfig = false;
    ms1_cycles = srolis::ms_to_cycles(1, freq_ghz_);
    start_transport = srolis::rdtsc();
    start_transport_clock = std::chrono::high_resolution_clock::now();
    breakTimeout = false;
    clusterRole = srolis::convertCluster(cluster);
    
    // The first thread to grab the lock initializes the transport
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
        evthread_use_pthreads(); // TODO: do we really need this even
                                 // when we manipulate one eventbase
                                 // per thread?
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

    // Setup eRPC

    // TODO: why sharing one nexus object between threads does not scale?
    // TODO: create one nexus per numa node
    // right now we create one nexus object per thread
    int port = std::atoi(config.shard(shardIdx, clusterRole).port.c_str());

    std::string local_uri = ip + ":" + std::to_string(port + id);
    //Warning("Created nexus object with local_uri = %s, numa_node = %u, req_types: %d", local_uri.c_str(), numa_node, st_nr_req_types, end_nr_req_types);
    nexus = new erpc::Nexus(local_uri, numa_node, 0);

    // register receive handlers
    for (uint8_t j = st_nr_req_types; j <= end_nr_req_types; j++)
    {
        nexus->register_req_func(j, fasttransport_request, erpc::ReqFuncType::kForeground);
    }

    // Create the RPC object
    c->rpc = new erpc::Rpc<erpc::CTransport>(nexus,
                                             static_cast<void *>(c),
                                             static_cast<uint16_t>(id),
                                             basic_sm_handler, phy_port);
    c->rpc->retry_connect_on_invalid_rpc_id_ = true;
    fasttransport_lock.unlock();
}

void FastTransport::stats() {
    //nexus->print_stats();
}

int FastTransport::handleTimeout(size_t start_tsc, int req_type, std::string extra) {
    if (clusterRole!=srolis::LOCALHOST_CENTER_INT) {
        return 0;
    }
    auto end_transport_clock = std::chrono::high_resolution_clock::now();
    if (end_transport_clock - start_transport_clock < std::chrono::seconds(8)) {
        return 0;
    }
    size_t end_tsc = srolis::rdtsc();
    if ((end_tsc-start_tsc)/(0.0+ms1_cycles)>=5) { // almost no effect on the results
        TThread::skipBeforeRemoteNewOrder = 4;
        TThread::skipBeforeRemotePayment = 4;
        return 1;
    }

    return 0;
}

inline char *FastTransport::GetRequestBuf(size_t reqLen, size_t respLen)
{
    // create a new request tag
    if (reqLen == 0)
        reqLen = c->rpc->get_max_data_per_pkt();
    if (respLen == 0)
        respLen = c->rpc->get_max_data_per_pkt();
    c->client.crt_req_tag = c->client.req_tag_pool.alloc();
    c->client.crt_req_tag->req_msgbuf = c->rpc->alloc_msg_buffer_or_die(reqLen);
    c->client.crt_req_tag->resp_msgbuf = c->rpc->alloc_msg_buffer_or_die(respLen);
    return reinterpret_cast<char *>(c->client.crt_req_tag->req_msgbuf.buf_);
}

inline int FastTransport::GetSession(TransportReceiver *src, uint8_t dstShardIdx, uint16_t id, int forceCenter = -1)
{
    auto session_key = std::make_tuple(srolis::LOCALHOST_CENTER_INT, dstShardIdx, id);

    int clusterRoleSentTo = clusterRole;
    if (sync_util::sync_logger::failed_shard_index>=0) {
        if (clusterRole==srolis::LEARNER_CENTER_INT) // new learner
            clusterRoleSentTo = srolis::LOCALHOST_CENTER_INT;
        
        if (clusterRole==srolis::LOCALHOST_CENTER_INT) {
            if (dstShardIdx==sync_util::sync_logger::failed_shard_index){ // alive leaders
                session_key = std::make_tuple(srolis::LEARNER_CENTER_INT, dstShardIdx, id);
                clusterRoleSentTo = srolis::LEARNER_CENTER_INT;
            }
        }
    }

    if (forceCenter >= 0) {
        session_key = std::make_tuple(forceCenter, dstShardIdx, id);
        clusterRoleSentTo = forceCenter;
    }

    const auto iter = c->client.sessions[src].find(session_key);
    int port = std::atoi(config.shard(dstShardIdx, clusterRoleSentTo).port.c_str()) + id;
    if (iter == c->client.sessions[src].end())
    {
        // create a new session to the replica core
        // use the dafault port from eRPC for control path
        auto x0 = std::chrono::high_resolution_clock::now() ;
        int session_id = c->rpc->create_session(config.shard(dstShardIdx, clusterRoleSentTo).host + ":" + std::to_string(port), id);
        size_t start_tsc = srolis::rdtsc();  // For counting timeout_ms
        while (!c->rpc->is_connected(session_id))
        {
            c->rpc->run_event_loop_once();
        }
        c->client.sessions[src][session_key] = session_id;
        //auto x1 = std::chrono::high_resolution_clock::now() ;
        //printf("session-id open time:%d,dstShardIdx:%d,cluster:%s,shardIdx:%d,pid:%d\n",
        //    std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count(),dstShardIdx, 
        //    srolis::convertClusterRole(clusterRoleSentTo).c_str(),TThread::get_shard_index(),
        //    TThread::getPartitionID());
        return session_id;
    }
    else
    {
        return iter->second;
    }
}

// This function assumes the message has already been copied to the
// req_msgbuf
bool FastTransport::SendRequestToShard(TransportReceiver *src,
                                       uint8_t reqType,
                                       uint8_t shardIdx,
                                       uint16_t id,
                                       size_t msgLen)
{
    if (shardIdx >= config.nshards) {
        Warning("fail shardIdx:%d,nshards:%d", shardIdx, config.nshards);
        ASSERT(shardIdx < config.nshards);
    }
    
    int session_id = GetSession(src, shardIdx, id);

    c->client.crt_req_tag->src = src;
    c->client.crt_req_tag->reqType = reqType;
    c->msg_size_req_sent += msgLen;
    //Warning("SendRequestToShard, reqType:%d, shardIdx:%d, id:%d, msgLen:%d", reqType, shardIdx, id, msgLen);
    c->msg_counter_req_sent += 1;
    c->rpc->resize_msg_buffer(&c->client.crt_req_tag->req_msgbuf, msgLen);

    c->rpc->enqueue_request(session_id,
                            reqType,
                            &c->client.crt_req_tag->req_msgbuf,
                            &c->client.crt_req_tag->resp_msgbuf,
                            fasttransport_response,
                            reinterpret_cast<void *>(c->client.crt_req_tag));
    size_t start_tsc = srolis::rdtsc();  // For counting timeout_ms
    while (src->Blocked() && !stop && !breakTimeout) {
        if (handleTimeout(start_tsc, (int)reqType, "SendRequestToShard")>0)
            throw 1002;

        c->rpc->run_event_loop_once();
    }
#if defined(MEGA_BENCHMARK) || defined(MEGA_BENCHMARK_MICRO)
    usleep(50*1000);
#endif
    if (breakTimeout && shardIdx==0){//abort locally, might cause issue for the ongoing/straggling eRPC requests
        Warning("[SendRequestToShard] abort the current transaction forcefully, tid:%d", TThread::get_shard_index());
        throw ((int)reqType);
    }

    return true;
}

bool FastTransport::SendRequestToAll(TransportReceiver *src,
                                    uint8_t reqType,
                                    int shards_to_send_bit_set,
                                    uint16_t id,
                                    size_t respMsgLen,
                                    size_t reqMsgLen,
                                    int forceCenter = -1) {
    c->rpc->resize_msg_buffer(&c->client.crt_req_tag->req_msgbuf, reqMsgLen);

    int lastShardIdx = config.nshards - 1;
    if (!shards_to_send_bit_set) return true;
    while ((shards_to_send_bit_set >> lastShardIdx) % 2 == 0) lastShardIdx --;

    //Warning("SendRequestToAll, reqType:%d, shards_to_send_bit_set:%d, id:%d, respMsgLen:%d, reqMsgLen:%d, forceCenter:%d", reqType, shards_to_send_bit_set, id, respMsgLen, reqMsgLen, forceCenter);
    bool isSentTo0 = false;
    for (int shardIdx = 0; shardIdx < config.nshards; shardIdx++) {
        if ((shards_to_send_bit_set >> shardIdx) % 2 == 0) continue;
        int session_id = GetSession(src, shardIdx, id, forceCenter);
        if (shardIdx == 0) { isSentTo0 = true; }
        if (shardIdx == lastShardIdx) {
            c->client.crt_req_tag->src = src;
            c->client.crt_req_tag->reqType = reqType;
            c->msg_size_req_sent += reqMsgLen;
            c->msg_counter_req_sent += 1;
            c->rpc->enqueue_request(session_id,
                                    reqType,
                                    &c->client.crt_req_tag->req_msgbuf,
                                    &c->client.crt_req_tag->resp_msgbuf,
                                    fasttransport_response,
                                    reinterpret_cast<void *>(c->client.crt_req_tag));
        } else {
            auto *rt = c->client.req_tag_pool.alloc();
            rt->req_msgbuf = c->rpc->alloc_msg_buffer_or_die(reqMsgLen);
            rt->resp_msgbuf = c->rpc->alloc_msg_buffer_or_die(respMsgLen);
            rt->reqType = reqType;
            rt->src = src;
            std::memcpy(reinterpret_cast<char *>(rt->req_msgbuf.buf_),
                        reinterpret_cast<char *>(c->client.crt_req_tag->req_msgbuf.buf_), reqMsgLen);
            c->msg_size_req_sent += reqMsgLen;
            c->msg_counter_req_sent += 1;
            c->rpc->enqueue_request(session_id, reqType,
                                    &rt->req_msgbuf,
                                    &rt->resp_msgbuf,
                                    fasttransport_response,
                                    reinterpret_cast<void *>(rt));
        }
    }
    size_t start_tsc = srolis::rdtsc();  // For counting timeout_ms
    while (src->Blocked() && !stop && !breakTimeout) {
        if (handleTimeout(start_tsc, (int)reqType, "SendRequestToAll")>0)
            throw 1002;
      
        c->rpc->run_event_loop_once();
    }
#if defined(MEGA_BENCHMARK) || defined(MEGA_BENCHMARK_MICRO)
    usleep(50*1000);
#endif
    if (breakTimeout && isSentTo0){//abort locally
        Warning("[SendRequestToAll] abort the current transaction forcefully, tid:%d", TThread::get_shard_index());
        throw ((int)reqType);
    }

    return true;
}

bool FastTransport::SendBatchRequestToAll(
    TransportReceiver *src,
    uint8_t req_type,
    uint16_t id,
    size_t resp_msg_len,
    const std::map<int, std::pair<char*, size_t>> &data_to_send
) {
    // int cnt = 0;
    bool isSentTo0 = false;
    for (auto it = data_to_send.begin(); it != data_to_send.end(); it++) {
        int shard_idx = it->first;
        char *raw_data = it->second.first;
        size_t req_msg_len = it->second.second;

        int session_id = GetSession(src, shard_idx, id);
        if (shard_idx == 0) { isSentTo0 = true; }
        auto *rt = c->client.req_tag_pool.alloc();
        //Warning("SendBatchRequestToAll, req_type:%d, shard_idx:%d, id:%d, req_msg_len:%d", req_type, shard_idx, id, req_msg_len);
        rt->req_msgbuf = c->rpc->alloc_msg_buffer_or_die(req_msg_len);
        rt->resp_msgbuf = c->rpc->alloc_msg_buffer_or_die(resp_msg_len);
        rt->reqType = req_type;
        rt->src = src;
        std::memcpy(reinterpret_cast<char *>(rt->req_msgbuf.buf_), raw_data, req_msg_len);
        c->msg_size_req_sent += req_msg_len;
        c->msg_counter_req_sent += 1;
        c->rpc->resize_msg_buffer(&rt->req_msgbuf, req_msg_len);

        c->rpc->enqueue_request(session_id, req_type,
                                &rt->req_msgbuf,
                                &rt->resp_msgbuf,
                                fasttransport_response,
                                reinterpret_cast<void *>(rt));
    }
    size_t start_tsc = srolis::rdtsc();  // For counting timeout_ms
    while (src->Blocked() && !stop && !breakTimeout) {
        if (handleTimeout(start_tsc, (int)req_type, "SendBatchRequestToAll")>0)
            throw 1002;

        c->rpc->run_event_loop_once();
    }
#if defined(MEGA_BENCHMARK) || defined(MEGA_BENCHMARK_MICRO)
    usleep(50*1000);
#endif
   
    if (breakTimeout && isSentTo0){//abort locally
        Warning("[SendBatchRequestToAll] abort the current transaction forcefully, tid:%d", TThread::get_shard_index());
        throw ((int)req_type);
    }
    return true;
}

void FastTransport::RunNoQueue() { // reserved for exchange_server
    int cnt=0;
    while (!stop) {
        cnt++;
        c->rpc->run_event_loop_once();
        if (cnt%100==0){
            cnt=0;
            if (!sync_util::sync_logger::exchange_running) break;
        }
    }
}

void FastTransport::Run()
{
    while (!stop)
    {
        c->rpc->run_event_loop_once();
        // send back the response
        for (auto it: c->queue_holders_response) {
            auto server_id = it.first;
            auto *server_queue = it.second;
            erpc::ReqHandle *req_handle;

            while (!server_queue->is_req_buffer_empty()) {
                size_t msg_size = 0;
                server_queue->fetch_one_req(&req_handle, msg_size);
                auto &resp = req_handle->pre_resp_msgbuf_;
                c->msg_size_resp_sent += msg_size;
                c->msg_counter_resp_sent += 1;
                c->rpc->resize_msg_buffer(&resp, msg_size);
                c->rpc->enqueue_response(req_handle, &resp);
                //server_queue->free_one_req();
            }
        }
    }
}

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

void FastTransport::Stop()
{
    stop = true;
    breakTimeout = true;
    Notice("one server msg_size_resp_sent: %d bytes, counter: %d, avg: %lf", c->msg_size_resp_sent, c->msg_counter_resp_sent, c->msg_size_resp_sent/(c->msg_counter_resp_sent+0.0));
}

void FastTransport::setBreakTimeout(bool bt=false) 
{
    breakTimeout = bt;
}
