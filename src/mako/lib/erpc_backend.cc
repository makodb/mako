// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * erpc_backend.cc:
 *   eRPC transport backend implementation
 *
 **********************************************************************/

#include "erpc_backend.h"
#include "erpc_request_handle.h"
#include "lib/assert.h"
#include "lib/common.h"
#include "lib/message.h"
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/sto/Transaction.hh"
#include "thread.h"

#include <util.h>
#include <numa.h>
#include <thread>
#include <sched.h>
#include <inttypes.h>

using namespace mako;

// External callbacks registered by bench.cc and dbtest.cc
extern std::function<int(int,int)> bench_callback_;
extern std::function<int(int,int)> dbtest_callback_;

// Static callback: Response handler (client-side)
void ErpcBackend::ResponseHandler(void *_context, void *_tag) {
    auto *c = static_cast<AppContext *>(_context);
    auto *rt = reinterpret_cast<req_tag_t *>(_tag);

    // Debug: ResponseHandler received response
    // Notice("[eRPC] ResponseHandler: received response for req_type=%d, resp_size=%zu",
    //        rt->reqType, rt->resp_msgbuf.get_data_size());

    rt->src->ReceiveResponse(rt->reqType,
                             reinterpret_cast<char *>(rt->resp_msgbuf.buf_));

    c->rpc->free_msg_buffer(rt->req_msgbuf);
    c->rpc->free_msg_buffer(rt->resp_msgbuf);
    c->client.req_tag_pool.free(rt);
}

// Static callback: Request handler (server-side)
void ErpcBackend::RequestHandler(erpc::ReqHandle *req_handle, void *_context) {
    auto *c = static_cast<AppContext *>(_context);

    // Handle watermark exchange request
    if (req_handle->get_req_msgbuf()->get_req_type() == watermarkReqType) {
        Debug("received a watermarkReqType");
        char *reqBuf = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
        char *respBuf = reinterpret_cast<char *>(req_handle->pre_resp_msgbuf_.buf_);
        auto *req = reinterpret_cast<basic_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);

        resp->result = sync_util::sync_logger::retrieveShardW();
        resp->req_nr = req->req_nr;
        resp->status = ErrorCode::SUCCESS;
        resp->shard_index = TThread::get_shard_index();

        auto &respX = req_handle->pre_resp_msgbuf_;
        c->msg_size_resp_sent += sizeof(get_int_response_t);
        c->msg_counter_resp_sent += 1;
        c->rpc->resize_msg_buffer(&respX, sizeof(get_int_response_t));
        c->rpc->enqueue_response(req_handle, &respX);
        return;
    }

    // Handle warmup request
    if (req_handle->get_req_msgbuf()->get_req_type() == warmupReqType) {
        char *reqBuf = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
        char *respBuf = reinterpret_cast<char *>(req_handle->pre_resp_msgbuf_.buf_);
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        auto *req = reinterpret_cast<warmup_request_t *>(reqBuf);

        resp->result = 1;
        resp->req_nr = req->req_nr;
        resp->status = ErrorCode::SUCCESS;
        resp->shard_index = TThread::get_shard_index();

        auto &respX = req_handle->pre_resp_msgbuf_;
        c->msg_size_resp_sent += sizeof(get_int_response_t);
        c->msg_counter_resp_sent += 1;
        c->rpc->resize_msg_buffer(&respX, sizeof(get_int_response_t));
        c->rpc->enqueue_response(req_handle, &respX);
        return;
    }

    // Handle control request
    if (req_handle->get_req_msgbuf()->get_req_type() == controlReqType) {
        char *reqBuf = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
        char *respBuf = reinterpret_cast<char *>(req_handle->pre_resp_msgbuf_.buf_);
        auto *req = reinterpret_cast<control_request_t*>(reqBuf);
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);

        Warning("# received a controlReqType, control: %d, shardIndex: %lld, targert_server_id: %llu",
                req->control, req->value, req->targert_server_id);

        bool is_datacenter_failure = req->targert_server_id == 10000;

        if (is_datacenter_failure) {
            if (dbtest_callback_)
                dbtest_callback_(req->control, req->value);
        } else {
            if (bench_callback_)
                bench_callback_(req->control, req->value);
        }

        resp->result = 0;
        resp->req_nr = req->req_nr;
        resp->status = ErrorCode::SUCCESS;
        resp->shard_index = TThread::get_shard_index();

        auto &respX = req_handle->pre_resp_msgbuf_;
        c->msg_size_resp_sent += sizeof(get_int_response_t);
        c->msg_counter_resp_sent += 1;
        c->rpc->resize_msg_buffer(&respX, sizeof(get_int_response_t));
        c->rpc->enqueue_response(req_handle, &respX);
        return;
    }

    // Normal requests: enqueue to helper queue
    // Determine which helper queue to use
    auto *target_server_id_reader = (TargetServerIDReader *)req_handle->get_req_msgbuf()->buf_;
    uint16_t target_server_id = target_server_id_reader->targert_server_id;
    auto *helper_queue = c->queue_holders[target_server_id];

    // Debug: RequestHandler received request
    // Notice("[eRPC] RequestHandler: received request req_type=%d, target_server_id=%d, req_size=%zu, helper_queue=%p",
    //        req_handle->get_req_msgbuf()->get_req_type(), target_server_id,
    //        req_handle->get_req_msgbuf()->get_data_size(), (void*)helper_queue);

    // Create ErpcRequestHandle wrapper with backend and server_id for transport-agnostic processing
    auto wrapper = std::make_unique<mako::ErpcRequestHandle>(req_handle, c->backend, target_server_id);

    // Store wrapper in map and get pointer to use as key
    void* key = wrapper.get();
    {
        std::lock_guard<std::mutex> lock(c->erpc_request_map_lock);
        c->erpc_request_map[key] = std::move(wrapper);
    }

    // Enqueue to helper queue (cast to erpc::ReqHandle* for compatibility)
    helper_queue->add_one_req(reinterpret_cast<erpc::ReqHandle*>(key), 0);
}

// Static callback: Session management handler
void ErpcBackend::SessionManagementHandler(int session_num,
                                          erpc::SmEventType sm_event_type,
                                          erpc::SmErrType sm_err_type,
                                          void *_context) {
    auto *c = static_cast<AppContext *>(_context);

    Assert(sm_err_type == erpc::SmErrType::kNoError);
    if (!(sm_event_type == erpc::SmEventType::kConnected ||
          sm_event_type == erpc::SmEventType::kDisconnected)) {
        throw std::runtime_error("Received unexpected SM event.");
    }

    Debug("Rpc %u: Session number %d %s. Error %s. Time elapsed = %.3f s.\n",
          c->rpc->get_rpc_id(), session_num,
          erpc::sm_event_type_str(sm_event_type).c_str(),
          erpc::sm_err_type_str(sm_err_type).c_str(),
          c->rpc->sec_since_creation());
}

// Constructor
ErpcBackend::ErpcBackend(const transport::Configuration& config,
                         int shard_idx,
                         uint16_t id,
                         const std::string& cluster)
    : config_(config),
      shard_idx_(shard_idx),
      id_(id),
      cluster_(cluster) {

    cluster_role_ = convertCluster(cluster);
    context_ = new AppContext();
    context_->backend = this;  // Set backend pointer for RequestHandler

    // Initialize timing
    freq_ghz_ = measure_rdtsc_freq();
    ms1_cycles_ = ms_to_cycles(1, freq_ghz_);
    start_transport_ = rdtsc();
    start_transport_clock_ = std::chrono::high_resolution_clock::now();
}

// Destructor
ErpcBackend::~ErpcBackend() {
    Shutdown();
}

// Initialize the backend
int ErpcBackend::Initialize(const std::string& local_uri,
                            uint8_t numa_node,
                            uint8_t phy_port,
                            uint8_t st_nr_req_types,
                            uint8_t end_nr_req_types) {
    numa_node_ = numa_node;
    phy_port_ = phy_port;

    Assert(numa_node <= numa_max_node());

    // Create Nexus
    nexus_ = new erpc::Nexus(local_uri, numa_node, 0);

    // Register request handlers
    for (uint8_t j = st_nr_req_types; j <= end_nr_req_types; j++) {
        nexus_->register_req_func(j, RequestHandler, erpc::ReqFuncType::kForeground);
    }

    // Create RPC object
    context_->rpc = new erpc::Rpc<erpc::CTransport>(
        nexus_,
        static_cast<void *>(context_),
        static_cast<uint16_t>(id_),
        SessionManagementHandler,
        phy_port);

    context_->rpc->retry_connect_on_invalid_rpc_id_ = true;

    Notice("ErpcBackend initialized on %s (numa_node=%d, phy_port=%d)",
           local_uri.c_str(), numa_node, phy_port);

    return 0;
}

// Shutdown
void ErpcBackend::Shutdown() {
    Notice("[SHUTDOWN] ErpcBackend::Shutdown starting");
    Stop();
    Notice("[SHUTDOWN] Stop() completed");

    if (context_) {
        Notice("[SHUTDOWN] Cleaning up context, rpc=%p", (void*)context_->rpc);

        // Check if there are pending wrappers in the map
        {
            std::lock_guard<std::mutex> lock(context_->erpc_request_map_lock);
            size_t pending_count = context_->erpc_request_map.size();
            if (pending_count > 0) {
                Warning("[SHUTDOWN] WARNING: %zu pending request wrappers in map during shutdown!",
                        pending_count);
                // Clear the map to prevent use-after-free
                context_->erpc_request_map.clear();
            }
        }
        Notice("[SHUTDOWN] Wrapper map cleared");

        if (context_->rpc) {
            Notice("[SHUTDOWN] About to delete eRPC Rpc object");
            delete context_->rpc;
            context_->rpc = nullptr;
            Notice("[SHUTDOWN] eRPC Rpc object deleted");
        }

        Notice("[SHUTDOWN] About to delete context");
        delete context_;
        context_ = nullptr;
        Notice("[SHUTDOWN] Context deleted");
    }

    if (nexus_) {
        Notice("[SHUTDOWN] About to delete nexus");
        delete nexus_;
        nexus_ = nullptr;
        Notice("[SHUTDOWN] Nexus deleted");
    }
    Notice("[SHUTDOWN] ErpcBackend::Shutdown completed");
}

// Allocate request buffer
char* ErpcBackend::AllocRequestBuffer(size_t req_len, size_t resp_len) {
    if (req_len == 0)
        req_len = context_->rpc->get_max_data_per_pkt();
    if (resp_len == 0)
        resp_len = context_->rpc->get_max_data_per_pkt();

    context_->client.crt_req_tag = context_->client.req_tag_pool.alloc();
    context_->client.crt_req_tag->req_msgbuf = context_->rpc->alloc_msg_buffer_or_die(req_len);
    context_->client.crt_req_tag->resp_msgbuf = context_->rpc->alloc_msg_buffer_or_die(resp_len);

    return reinterpret_cast<char *>(context_->client.crt_req_tag->req_msgbuf.buf_);
}

// Free request buffer
void ErpcBackend::FreeRequestBuffer() {
    // eRPC frees buffers in response handler
}

// Get or create session
int ErpcBackend::GetSession(TransportReceiver *src,
                            uint8_t shard_idx,
                            uint16_t server_id,
                            int force_center) {
    auto session_key = std::make_tuple(LOCALHOST_CENTER_INT, shard_idx, server_id);

    int clusterRoleSentTo = cluster_role_;

    // Handle shard failure scenarios
    if (sync_util::sync_logger::failed_shard_index >= 0) {
        if (cluster_role_ == LEARNER_CENTER_INT)
            clusterRoleSentTo = LOCALHOST_CENTER_INT;

        if (cluster_role_ == LOCALHOST_CENTER_INT) {
            if (shard_idx == sync_util::sync_logger::failed_shard_index) {
                session_key = std::make_tuple(LEARNER_CENTER_INT, shard_idx, server_id);
                clusterRoleSentTo = LEARNER_CENTER_INT;
            }
        }
    }

    if (force_center >= 0) {
        session_key = std::make_tuple(force_center, shard_idx, server_id);
        clusterRoleSentTo = force_center;
    }

    const auto iter = context_->client.sessions[src].find(session_key);
    int port = std::atoi(config_.shard(shard_idx, clusterRoleSentTo).port.c_str()) + server_id;

    if (iter == context_->client.sessions[src].end()) {
        // Create new session
        int session_id = context_->rpc->create_session(
            config_.shard(shard_idx, clusterRoleSentTo).host + ":" + std::to_string(port),
            server_id);

        size_t start_tsc = rdtsc();
        while (!context_->rpc->is_connected(session_id)) {
            context_->rpc->run_event_loop_once();
        }

        context_->client.sessions[src][session_key] = session_id;
        return session_id;
    } else {
        return iter->second;
    }
}

// Handle timeout
int ErpcBackend::HandleTimeout(size_t start_tsc, int req_type, const std::string& extra) {
    if (cluster_role_ != LOCALHOST_CENTER_INT) {
        return 0;
    }

    auto end_transport_clock = std::chrono::high_resolution_clock::now();
    if (end_transport_clock - start_transport_clock_ < std::chrono::seconds(8)) {
        return 0;
    }

    size_t end_tsc = rdtsc();
    if ((end_tsc - start_tsc) / (0.0 + ms1_cycles_) >= 5) {
        TThread::skipBeforeRemoteNewOrder = 4;
        TThread::skipBeforeRemotePayment = 4;
        return 1;
    }

    return 0;
}

// Send request to single shard
bool ErpcBackend::SendToShard(TransportReceiver* src,
                              uint8_t req_type,
                              uint8_t shard_idx,
                              uint16_t server_id,
                              size_t msg_len) {
    if (shard_idx >= config_.nshards) {
        Warning("fail shardIdx:%d,nshards:%d", shard_idx, config_.nshards);
        ASSERT(shard_idx < config_.nshards);
    }

    // Debug: Track remote requests
    // int my_shard = TThread::get_shard_index();
    // if (shard_idx != my_shard) {
    //     Notice("[eRPC] SendToShard: remote request from shard %d to shard %d, req_type=%d, session_id=%d, msg_len=%zu",
    //            my_shard, shard_idx, req_type, GetSession(src, shard_idx, server_id), msg_len);
    // }

    int session_id = GetSession(src, shard_idx, server_id);

    context_->client.crt_req_tag->src = src;
    context_->client.crt_req_tag->reqType = req_type;
    context_->msg_size_req_sent += msg_len;
    context_->msg_counter_req_sent += 1;
    context_->rpc->resize_msg_buffer(&context_->client.crt_req_tag->req_msgbuf, msg_len);

    // Debug: Track request enqueueing
    // Notice("[eRPC] SendToShard: enqueuing request, session_id=%d, req_type=%d, msg_len=%zu, buffer_size=%zu",
    //        session_id, req_type, msg_len, context_->client.crt_req_tag->req_msgbuf.get_data_size());

    context_->rpc->enqueue_request(session_id,
                            req_type,
                            &context_->client.crt_req_tag->req_msgbuf,
                            &context_->client.crt_req_tag->resp_msgbuf,
                            ResponseHandler,
                            reinterpret_cast<void *>(context_->client.crt_req_tag));

    size_t start_tsc = rdtsc();
    while (src->Blocked() && !stop_ && !break_timeout_) {
        if (HandleTimeout(start_tsc, (int)req_type, "SendToShard") > 0)
            throw 1002;

        context_->rpc->run_event_loop_once();
    }

#if defined(MEGA_BENCHMARK) || defined(MEGA_BENCHMARK_MICRO)
    usleep(50*1000);
#endif

    if (break_timeout_ && shard_idx == 0) {
        Warning("[SendToShard] abort the current transaction forcefully, tid:%d", TThread::get_shard_index());
        throw ((int)req_type);
    }

    return true;
}

// Send request to multiple shards
bool ErpcBackend::SendToAll(TransportReceiver* src,
                            uint8_t req_type,
                            int shards_bit_set,
                            uint16_t server_id,
                            size_t resp_len,
                            size_t req_len,
                            int force_center) {
    context_->rpc->resize_msg_buffer(&context_->client.crt_req_tag->req_msgbuf, req_len);

    int lastShardIdx = config_.nshards - 1;
    if (!shards_bit_set) return true;
    while ((shards_bit_set >> lastShardIdx) % 2 == 0) lastShardIdx--;

    bool isSentTo0 = false;
    for (int shardIdx = 0; shardIdx < config_.nshards; shardIdx++) {
        if ((shards_bit_set >> shardIdx) % 2 == 0) continue;

        int session_id = GetSession(src, shardIdx, server_id, force_center);
        if (shardIdx == 0) { isSentTo0 = true; }

        if (shardIdx == lastShardIdx) {
            context_->client.crt_req_tag->src = src;
            context_->client.crt_req_tag->reqType = req_type;
            context_->msg_size_req_sent += req_len;
            context_->msg_counter_req_sent += 1;
            context_->rpc->enqueue_request(session_id,
                                    req_type,
                                    &context_->client.crt_req_tag->req_msgbuf,
                                    &context_->client.crt_req_tag->resp_msgbuf,
                                    ResponseHandler,
                                    reinterpret_cast<void *>(context_->client.crt_req_tag));
        } else {
            auto *rt = context_->client.req_tag_pool.alloc();
            rt->req_msgbuf = context_->rpc->alloc_msg_buffer_or_die(req_len);
            rt->resp_msgbuf = context_->rpc->alloc_msg_buffer_or_die(resp_len);
            rt->reqType = req_type;
            rt->src = src;
            std::memcpy(reinterpret_cast<char *>(rt->req_msgbuf.buf_),
                        reinterpret_cast<char *>(context_->client.crt_req_tag->req_msgbuf.buf_),
                        req_len);
            context_->msg_size_req_sent += req_len;
            context_->msg_counter_req_sent += 1;
            context_->rpc->enqueue_request(session_id, req_type,
                                    &rt->req_msgbuf,
                                    &rt->resp_msgbuf,
                                    ResponseHandler,
                                    reinterpret_cast<void *>(rt));
        }
    }

    size_t start_tsc = rdtsc();
    while (src->Blocked() && !stop_ && !break_timeout_) {
        if (HandleTimeout(start_tsc, (int)req_type, "SendToAll") > 0)
            throw 1002;

        context_->rpc->run_event_loop_once();
    }

#if defined(MEGA_BENCHMARK) || defined(MEGA_BENCHMARK_MICRO)
    usleep(50*1000);
#endif

    if (break_timeout_ && isSentTo0) {
        Warning("[SendToAll] abort the current transaction forcefully, tid:%d", TThread::get_shard_index());
        throw ((int)req_type);
    }

    return true;
}

// Send batch request to multiple shards
bool ErpcBackend::SendBatchToAll(TransportReceiver* src,
                                 uint8_t req_type,
                                 uint16_t server_id,
                                 size_t resp_len,
                                 const std::map<int, std::pair<char*, size_t>>& data) {
    bool isSentTo0 = false;

    for (auto it = data.begin(); it != data.end(); it++) {
        int shard_idx = it->first;
        char *raw_data = it->second.first;
        size_t req_len = it->second.second;

        int session_id = GetSession(src, shard_idx, server_id);
        if (shard_idx == 0) { isSentTo0 = true; }

        auto *rt = context_->client.req_tag_pool.alloc();
        rt->req_msgbuf = context_->rpc->alloc_msg_buffer_or_die(req_len);
        rt->resp_msgbuf = context_->rpc->alloc_msg_buffer_or_die(resp_len);
        rt->reqType = req_type;
        rt->src = src;
        std::memcpy(reinterpret_cast<char *>(rt->req_msgbuf.buf_), raw_data, req_len);
        context_->msg_size_req_sent += req_len;
        context_->msg_counter_req_sent += 1;
        context_->rpc->resize_msg_buffer(&rt->req_msgbuf, req_len);

        context_->rpc->enqueue_request(session_id, req_type,
                                &rt->req_msgbuf,
                                &rt->resp_msgbuf,
                                ResponseHandler,
                                reinterpret_cast<void *>(rt));
    }

    size_t start_tsc = rdtsc();
    while (src->Blocked() && !stop_ && !break_timeout_) {
        if (HandleTimeout(start_tsc, (int)req_type, "SendBatchToAll") > 0)
            throw 1002;

        context_->rpc->run_event_loop_once();
    }

#if defined(MEGA_BENCHMARK) || defined(MEGA_BENCHMARK_MICRO)
    usleep(50*1000);
#endif

    if (break_timeout_ && isSentTo0) {
        Warning("[SendBatchToAll] abort the current transaction forcefully, tid:%d", TThread::get_shard_index());
        throw ((int)req_type);
    }

    return true;
}

// Run event loop (with response queue handling)
void ErpcBackend::RunEventLoop() {
    while (!stop_) {
        context_->rpc->run_event_loop_once();

        // Send back responses from helper threads
        for (auto it: context_->queue_holders_response) {
            auto server_id = it.first;
            auto *server_queue = it.second;
            erpc::ReqHandle *handle_ptr;

            while (!server_queue->is_req_buffer_empty()) {
                size_t msg_size = 0;
                server_queue->fetch_one_req(&handle_ptr, msg_size);

                // Cast back to void* key and lookup ErpcRequestHandle wrapper
                void* key = reinterpret_cast<void*>(handle_ptr);
                std::unique_ptr<mako::ErpcRequestHandle> wrapper;
                {
                    std::lock_guard<std::mutex> lock(context_->erpc_request_map_lock);
                    auto map_it = context_->erpc_request_map.find(key);
                    if (map_it != context_->erpc_request_map.end()) {
                        wrapper = std::move(map_it->second);
                        context_->erpc_request_map.erase(map_it);
                    }
                }

                if (!wrapper) {
                    Warning("ErpcBackend::RunEventLoop: wrapper not found for key %p", key);
                    continue;
                }

                // Get the actual eRPC handle and send response
                erpc::ReqHandle* req_handle = wrapper->GetErpcHandle();
                auto &resp = req_handle->pre_resp_msgbuf_;
                context_->msg_size_resp_sent += msg_size;
                context_->msg_counter_resp_sent += 1;
                context_->rpc->resize_msg_buffer(&resp, msg_size);

                // Debug: Track response sending
                // Notice("[eRPC] RunEventLoop: sending response, req_type=%d, server_id=%d, msg_size=%zu",
                //        req_handle->get_req_msgbuf()->get_req_type(), server_id, msg_size);

                context_->rpc->enqueue_response(req_handle, &resp);

                // Wrapper is automatically deleted when it goes out of scope
            }
        }
    }
}

// Run event loop without queue handling (for exchange server)
void ErpcBackend::RunNoQueue() {
    int cnt = 0;
    while (!stop_) {
        cnt++;
        context_->rpc->run_event_loop_once();
        if (cnt % 100 == 0) {
            cnt = 0;
            if (!sync_util::sync_logger::exchange_running) break;
        }
    }
}

// Stop event loop
void ErpcBackend::Stop() {
    stop_ = true;
    break_timeout_ = true;

    Notice("ErpcBackend stats: msg_size_resp_sent: %" PRIu64 " bytes, counter: %d, avg: %lf",
           context_->msg_size_resp_sent, context_->msg_counter_resp_sent,
           context_->msg_size_resp_sent / (context_->msg_counter_resp_sent + 0.0));
}

// Print statistics
void ErpcBackend::PrintStats() {
    Notice("ErpcBackend request stats: msg_size_req_sent: %" PRIu64 " bytes, counter: %d, avg: %lf",
           context_->msg_size_req_sent, context_->msg_counter_req_sent,
           context_->msg_size_req_sent / (context_->msg_counter_req_sent + 0.0));
}

// ErpcRequestHandle::EnqueueResponse - enqueues response to response queue
void ErpcRequestHandle::EnqueueResponse(size_t msg_size) {
    if (!backend_) {
        Warning("ErpcRequestHandle::EnqueueResponse: backend is null!");
        return;
    }

    // Find the response queue for this server
    auto it = backend_->GetHelperQueuesResponse().find(server_id_);
    if (it == backend_->GetHelperQueuesResponse().end()) {
        Warning("ErpcRequestHandle::EnqueueResponse: No response queue found for server_id %d", server_id_);
        return;
    }

    auto* response_queue = it->second;

    // Enqueue response (using GetOpaqueHandle as the key, same as for requests)
    response_queue->add_one_req(reinterpret_cast<erpc::ReqHandle*>(GetOpaqueHandle()), msg_size);
}
