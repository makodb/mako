// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * rrr_rpc_backend.cc:
 *   rrr/rpc transport backend implementation
 *
 **********************************************************************/

#include "rrr_rpc_backend.h"
#include "lib/assert.h"
#include "lib/common.h"
#include "lib/message.h"
#include "lib/helper_queue.h"
#include "thread.h"
#include "benchmarks/sto/sync_util.hh"

#include <chrono>
#include <thread>
#include <cstring>
#include <inttypes.h>

using namespace mako;

// External callbacks registered by bench.cc and dbtest.cc
extern std::function<int(int,int)> bench_callback_;
extern std::function<int(int,int)> dbtest_callback_;

// Constructor
RrrRpcBackend::RrrRpcBackend(const transport::Configuration& config,
                             int shard_idx,
                             uint16_t id,
                             const std::string& cluster)
    : config_(config),
      shard_idx_(shard_idx),
      id_(id),
      cluster_(cluster),
      poll_thread_worker_(rusty::None) {

    cluster_role_ = convertCluster(cluster);
}

// Destructor
RrrRpcBackend::~RrrRpcBackend() {
    Notice("RrrRpcBackend::~RrrRpcBackend: START destructor");
    Shutdown();
    Notice("RrrRpcBackend::~RrrRpcBackend: Shutdown() completed");
}

// Initialize the backend
int RrrRpcBackend::Initialize(const std::string& local_uri,
                              uint8_t numa_node,
                              uint8_t phy_port,
                              uint8_t st_nr_req_types,
                              uint8_t end_nr_req_types) {

    // Create PollThread for event-driven I/O
    poll_thread_worker_ = rusty::Some(rrr::PollThread::create());
    Notice("RrrRpcBackend::Initialize: poll_thread_worker_ created, is_some=%d", poll_thread_worker_.is_some());

    // Extract host and port from local_uri (format: "host:port")
    size_t colon_pos = local_uri.find(':');
    if (colon_pos == std::string::npos) {
        Panic("Invalid local_uri format: %s (expected host:port)", local_uri.c_str());
        return -1;
    }

    std::string port_str = local_uri.substr(colon_pos + 1);

    // Create server to listen for incoming requests
    // Use as_ref().unwrap().clone() instead of unwrap() to avoid moving/destroying the Option
    server_ = new rrr::Server(poll_thread_worker_.as_ref().unwrap().clone());

    // Register request handlers for all request types
    // Note: We capture both req_type and 'this' in the lambda
    for (uint8_t req_type = st_nr_req_types; req_type <= end_nr_req_types; req_type++) {
        server_->reg(req_type, [this, req_type](rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
            RequestHandler(req_type, std::move(req), weak_sconn, this);
        });
    }

    // Start listening on the port
    int ret = server_->start(("0.0.0.0:" + port_str).c_str());
    if (ret != 0) {
        Panic("Failed to start rrr::Server on port %s", port_str.c_str());
        return ret;
    }

    Notice("RrrRpcBackend initialized on %s (listening on 0.0.0.0:%s)",
           local_uri.c_str(), port_str.c_str());

    return 0;
}

// Shutdown
void RrrRpcBackend::Shutdown() {
    // Stop() already handles:
    // - Setting stop_ flag atomically (idempotent)
    // - Closing all client connections
    // - Clearing clients_ map
    // - Deleting server
    // - Signaling helper queues to stop
    Stop();

    Notice("RrrRpcBackend::Shutdown: About to delete server");

    // Delete server first (before shutting down poll_thread_worker)
    if (server_) {
        Notice("RrrRpcBackend::Shutdown: Server pointer is valid, deleting...");
        try {
            delete server_;
            server_ = nullptr;
            Notice("RrrRpcBackend::Shutdown: Server deleted successfully");
        } catch (const std::exception& e) {
            Warning("RrrRpcBackend::Shutdown: Exception during server deletion: %s", e.what());
            server_ = nullptr;
        } catch (...) {
            Warning("RrrRpcBackend::Shutdown: Unknown exception during server deletion");
            server_ = nullptr;
        }
    } else {
        Notice("RrrRpcBackend::Shutdown: Server pointer is null, skipping deletion");
    }

    Notice("RrrRpcBackend::Shutdown: About to shutdown poll_thread_worker_");

    // Shutdown poll thread worker explicitly (after server is deleted)
    if (poll_thread_worker_.is_some()) {
        Notice("RrrRpcBackend::Shutdown: poll_thread_worker_ is valid, calling shutdown()");
        poll_thread_worker_.as_ref().unwrap()->shutdown();
        Notice("RrrRpcBackend::Shutdown: poll_thread_worker_->shutdown() completed");
        poll_thread_worker_ = rusty::None;
        Notice("RrrRpcBackend::Shutdown: poll_thread_worker_ reset to None");
    } else {
        Notice("RrrRpcBackend::Shutdown: poll_thread_worker_ is null, skipping shutdown");
    }

    Notice("RrrRpcBackend::Shutdown: Shutdown sequence completed, destructor will now exit");
}

namespace {
struct ThreadBuffers {
    std::vector<char> request_buffer;
    size_t response_len{0};
};

thread_local ThreadBuffers tls_buffers;
}

// Allocate request buffer
char* RrrRpcBackend::AllocRequestBuffer(size_t req_len, size_t resp_len) {
    tls_buffers.request_buffer.resize(req_len);
    tls_buffers.response_len = resp_len;
    return tls_buffers.request_buffer.data();
}

// Free request buffer
void RrrRpcBackend::FreeRequestBuffer() {
    tls_buffers.response_len = 0;
}

// Get or create client connection to a shard
rusty::Option<rusty::Arc<rrr::Client>> RrrRpcBackend::GetOrCreateClient(uint8_t shard_idx,
                                                          uint16_t server_id,
                                                          int force_center) {
    int clusterRoleSentTo = cluster_role_;

    // Handle shard failure scenarios (same logic as eRPC)
    auto session_key = std::make_tuple(LOCALHOST_CENTER_INT, shard_idx, server_id);

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

    // Check if client already exists
    clients_lock_.lock();

    // Check stop flag while holding lock - if stopping, don't create/return clients
    if (stop_) {
        clients_lock_.unlock();
        Warning("GetOrCreateClient: stop requested, not creating/returning client");
        return rusty::None;  // Return None
    }

    auto it = clients_.find(session_key);
    if (it != clients_.end()) {
        clients_lock_.unlock();
        Debug("GetOrCreateClient: Reusing existing client for shard %d, server %d", shard_idx, server_id);
        return it->second.clone();
    }

    // Create new client
    Debug("GetOrCreateClient: Creating new client for shard %d, server %d", shard_idx, server_id);

    // Check if poll_thread_worker_ is still valid (not shutdown yet)
    if (poll_thread_worker_.is_none()) {
        Warning("GetOrCreateClient: poll_thread_worker_ is None (backend shutting down)");
        clients_lock_.unlock();
        return rusty::None;
    }

    // Use as_ref().unwrap().clone() instead of unwrap() to avoid moving/destroying the Option
    rusty::Arc<rrr::Client> client = rrr::Client::create(poll_thread_worker_.as_ref().unwrap().clone());

    // Connect to destination
    int port = std::atoi(config_.shard(shard_idx, clusterRoleSentTo).port.c_str()) + server_id;
    std::string addr = config_.shard(shard_idx, clusterRoleSentTo).host + ":" + std::to_string(port);

    Debug("GetOrCreateClient: Connecting to %s", addr.c_str());

    int ret = client->connect(addr.c_str());
    if (ret != 0) {
        //Warning("Failed to connect to %s (error %d)", addr.c_str(), ret);
        clients_lock_.unlock();
        return rusty::None;  // Return None
    }

    // Store client
    clients_.emplace(session_key, client.clone());
    clients_lock_.unlock();

    Debug("Created rrr::Client connection to %s", addr.c_str());
    return client;
}

// Send request to single shard
bool RrrRpcBackend::SendToShard(TransportReceiver* src,
                                uint8_t req_type,
                                uint8_t shard_idx,
                                uint16_t server_id,
                                size_t msg_len) {
    // Early return if stopping - don't start new RPC operations
    if (stop_) {
        Warning("RrrRpcBackend::SendToShard: stop requested, not sending (req_type=%d)", req_type);
        return false;
    }

    Debug("RrrRpcBackend::SendToShard: req_type=%d, shard_idx=%d, server_id=%d, msg_len=%zu",
          req_type, shard_idx, server_id, msg_len);

    if (shard_idx >= config_.nshards) {
        Warning("Invalid shardIdx:%d, nshards:%d", shard_idx, config_.nshards);
        return false;
    }

    auto client_opt = GetOrCreateClient(shard_idx, server_id);
    if (client_opt.is_none()) {
        Warning("Failed to get client for shard %d, server %d", shard_idx, server_id);
        return false;
    }
    rusty::Arc<rrr::Client> client = client_opt.unwrap();

    Debug("RrrRpcBackend::SendToShard: Got client, calling begin_request");

    // Begin request with rrr/rpc
    auto fu_result = client->begin_request(req_type);
    if (fu_result.is_err()) {
        Warning("Failed to begin_request for req_type %d", req_type);
        return false;
    }
    auto fu = fu_result.unwrap();

    Debug("RrrRpcBackend::SendToShard: begin_request succeeded, writing request data");

    // Write request data using client's << operator
    rrr::Marshal m;
    m.write(tls_buffers.request_buffer.data(), msg_len);
    *client << m;

    msg_size_req_sent_ += msg_len;
    msg_counter_req_sent_ += 1;

    Debug("RrrRpcBackend::SendToShard: Calling end_request to send RPC");

    // Send request
    client->end_request();

    Debug("RrrRpcBackend::SendToShard: Waiting for response");

    // Wait for response
    fu->timed_wait(1);

    if (fu->timed_out()) {
        throw 1002;
    }

    // Check stop again after wait - client might have been closed during wait
    if (stop_) {
        Warning("RrrRpcBackend::SendToShard: stop requested after wait, aborting");
        return false;  // Arc auto-released
    }

    if (fu->get_error_code() != 0) {
        Warning("RPC error: %d", fu->get_error_code());
        return false;  // Arc auto-released
    }

    // Final check before accessing response - make sure we're not stopping
    if (stop_) {
        Warning("RrrRpcBackend::SendToShard: stop requested before processing response, aborting");
        return false;  // Arc auto-released
    }

    // Read response
    rrr::Marshal& resp_marshal = fu->get_reply();
    std::vector<char> resp_buffer(tls_buffers.response_len);
    resp_marshal.read(resp_buffer.data(), tls_buffers.response_len);

    // Deliver response to receiver (only if not stopping)
    if (!stop_ && src) {
        src->ReceiveResponse(req_type, resp_buffer.data());
    }

    // Arc auto-released when fu goes out of scope
    return !stop_;
}

// Send request to multiple shards
bool RrrRpcBackend::SendToAll(TransportReceiver* src,
                              uint8_t req_type,
                              int shards_bit_set,
                              uint16_t server_id,
                              size_t resp_len,
                              size_t req_len,
                              int force_center) {
    // Early return if stopping - don't start new RPC operations
    if (stop_) {
        Warning("RrrRpcBackend::SendToAll: stop requested, not sending (req_type=%d)", req_type);
        return false;
    }

    Debug("RrrRpcBackend::SendToAll: req_type=%d, shards_bit_set=%d, server_id=%d, req_len=%zu",
          req_type, shards_bit_set, server_id, req_len);

    if (!shards_bit_set) return true;

    // Prepare futures for all shards
    std::vector<rusty::Arc<rrr::Future>> futures;

    for (int shard_idx = 0; shard_idx < config_.nshards; shard_idx++) {
        if ((shards_bit_set >> shard_idx) % 2 == 0) continue;

        Debug("RrrRpcBackend::SendToAll: Sending to shard %d", shard_idx);

        auto client_opt = GetOrCreateClient(shard_idx, server_id, force_center);
        if (client_opt.is_none()) {
            //Warning("Failed to get client for shard %d", shard_idx);
            continue;
        }
        rusty::Arc<rrr::Client> client = client_opt.unwrap();

        Debug("RrrRpcBackend::SendToAll: Got client for shard %d, calling begin_request", shard_idx);

        auto fu_result = client->begin_request(req_type);
        if (fu_result.is_err()) {
            Warning("Failed to begin_request for shard %d", shard_idx);
            continue;
        }
        auto fu = fu_result.unwrap();

        // Write request data using client's << operator
        rrr::Marshal m;
        m.write(tls_buffers.request_buffer.data(), req_len);
        *client << m;

        msg_size_req_sent_ += req_len;
        msg_counter_req_sent_ += 1;

        Debug("RrrRpcBackend::SendToAll: Calling end_request for shard %d", shard_idx);

        client->end_request();
        futures.push_back(std::move(fu));
    }

    Debug("RrrRpcBackend::SendToAll: Sent to %zu shards, waiting for responses", futures.size());

    // Wait for all responses
    for (auto& fu : futures) {
        // Check if stop was requested before waiting
        if (stop_) {
            Warning("RrrRpcBackend::SendToAll: stop requested, aborting wait for response (req_type=%d)", req_type);
            continue;  // Arc auto-released
        }

        // Wait for response with timeout, checking stop flag periodically
        fu->timed_wait(1);

        if (fu->timed_out()) {
            throw 1002;
        }

        // Check stop again after wait
        if (stop_) {
            Warning("RrrRpcBackend::SendToAll: stop requested after wait, aborting (req_type=%d)", req_type);
            continue;  // Arc auto-released
        }

        if (fu->get_error_code() != 0) {
            Warning("RPC error: %d", fu->get_error_code());
            continue;  // Arc auto-released
        }

        // Read response
        rrr::Marshal& resp_marshal = fu->get_reply();
        std::vector<char> resp_buffer(resp_len);
        resp_marshal.read(resp_buffer.data(), resp_len);

        // Deliver response (only if not stopping and src is valid)
        if (!stop_ && src) {
            src->ReceiveResponse(req_type, resp_buffer.data());
        }

        // Arc auto-released at end of loop iteration
    }

    // All Arcs auto-released when futures vector destroyed
    return !stop_;
}

// Send batch request to multiple shards
bool RrrRpcBackend::SendBatchToAll(TransportReceiver* src,
                                   uint8_t req_type,
                                   uint16_t server_id,
                                   size_t resp_len,
                                   const std::map<int, std::pair<char*, size_t>>& data) {
    // Early return if stopping - don't start new RPC operations
    if (stop_) {
        Warning("RrrRpcBackend::SendBatchToAll: stop requested, not sending (req_type=%d)", req_type);
        return false;
    }

    std::vector<rusty::Arc<rrr::Future>> futures;

    for (auto& entry : data) {
        int shard_idx = entry.first;
        char* raw_data = entry.second.first;
        size_t req_len = entry.second.second;

        auto client_opt = GetOrCreateClient(shard_idx, server_id);
        if (client_opt.is_none()) continue;
        rusty::Arc<rrr::Client> client = client_opt.unwrap();

        auto fu_result = client->begin_request(req_type);
        if (fu_result.is_err()) continue;
        auto fu = fu_result.unwrap();

        // Write request data using client's << operator
        rrr::Marshal m;
        m.write(raw_data, req_len);
        *client << m;

        msg_size_req_sent_ += req_len;
        msg_counter_req_sent_ += 1;

        client->end_request();
        futures.push_back(std::move(fu));
    }

    // Wait for all responses
    for (auto& fu : futures) {
        // Check if stop was requested before waiting
        if (stop_) {
            Warning("RrrRpcBackend::SendBatchToAll: stop requested, aborting wait (req_type=%d)", req_type);
            continue;  // Arc auto-released
        }

        // Wait for response with timeout, checking stop flag periodically
        fu->timed_wait(1);

        if (fu->timed_out()) {
            throw 1002;
        }

        // Check stop again after wait
        if (stop_) {
            Warning("RrrRpcBackend::SendBatchToAll: stop requested after wait, aborting (req_type=%d)", req_type);
            continue;  // Arc auto-released
        }

        if (fu->get_error_code() != 0) {
            Warning("RPC error: %d", fu->get_error_code());
            continue;  // Arc auto-released
        }

        // Read response
        rrr::Marshal& resp_marshal = fu->get_reply();
        std::vector<char> resp_buffer(resp_len);
        resp_marshal.read(resp_buffer.data(), resp_len);

        // Deliver response (only if not stopping and src is valid)
        if (!stop_ && src) {
            src->ReceiveResponse(req_type, resp_buffer.data());
        }

        // Arc auto-released at end of loop iteration
    }

    // All Arcs auto-released when futures vector destroyed
    return !stop_;
}

// Run event loop
void RrrRpcBackend::RunEventLoop() {
    // The PollThread runs its own thread for network I/O
    // Here we process responses from helper threads and send them back
    Notice("RrrRpcBackend::RunEventLoop: Starting event loop");

    event_loop_running_.store(true, std::memory_order_release);

    while (!stop_) {
        // Process responses from all helper queues
        for (auto& it : queue_holders_response_) {
            // Check stop flag before processing each queue
            if (stop_) {
                break;
            }

            auto server_id = it.first;
            auto* server_queue = it.second;

            erpc::ReqHandle* req_handle_ptr;
            size_t msg_size = 0;

            // Fetch responses from helper thread queue
            while (!server_queue->is_req_buffer_empty()) {
                // Check stop flag before processing each response
                if (stop_) {
                    break;
                }

                server_queue->fetch_one_req(&req_handle_ptr, msg_size);

                // Cast back to void* key and lookup RrrRequestHandle
                void* key = reinterpret_cast<void*>(req_handle_ptr);

                rrr_request_map_lock_.lock();
                auto map_it = rrr_request_map_.find(key);
                if (map_it == rrr_request_map_.end()) {
                    rrr_request_map_lock_.unlock();
                    Warning("RrrRequestHandle not found for key %p", key);
                    continue;
                }

                // Move ownership out of map
                std::unique_ptr<RrrRequestHandle> rrr_handle = std::move(map_it->second);
                rrr_request_map_.erase(map_it);
                rrr_request_map_lock_.unlock();

                // Validate connection is still valid before sending response
                if (!rrr_handle->sconn) {
                    Warning("ServerConnection is null, skipping response");
                    continue;
                }

                // Send response back via rrr/rpc
                const_cast<rrr::ServerConnection&>(*rrr_handle->sconn).begin_reply(*rrr_handle->original_request);
                rrr::Marshal m;
                m.write(rrr_handle->response_data.data(), msg_size);
                const_cast<rrr::ServerConnection&>(*rrr_handle->sconn) << m;
                const_cast<rrr::ServerConnection&>(*rrr_handle->sconn).end_reply();

                msg_size_resp_sent_ += msg_size;
                msg_counter_resp_sent_ += 1;
            }
        }

        // Small sleep to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    Notice("RrrRpcBackend::RunEventLoop: Stop flag detected, exiting event loop");

    event_loop_running_.store(false, std::memory_order_release);

    Notice("RrrRpcBackend::RunEventLoop: Exited cleanly");
}

// Stop event loop
void RrrRpcBackend::Stop() {
    // Make Stop() idempotent - only the first call proceeds
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) {
        Notice("RrrRpcBackend::Stop: Already stopped, returning");
        return;
    }

    Notice("RrrRpcBackend::Stop: BEGIN - Setting stop flag");

    // Wait for event loop to actually exit (poll with timeout)
    Notice("RrrRpcBackend::Stop: Waiting for event loop to exit...");
    auto start_time = std::chrono::steady_clock::now();
    while (event_loop_running_.load(std::memory_order_acquire)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();

        if (elapsed > 5000) {
            Warning("RrrRpcBackend::Stop: Event loop did not exit within 5 second timeout!");
            break;
        }

        // Small sleep to avoid busy-polling
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!event_loop_running_.load(std::memory_order_acquire)) {
        Notice("RrrRpcBackend::Stop: Event loop exited successfully");
    }

    // Signal all helper queues to stop (both request and response queues)
    Notice("RrrRpcBackend::Stop: Signaling %zu request queues to stop", queue_holders_.size());
    for (auto& entry : queue_holders_) {
        if (entry.second) {
            Notice("RrrRpcBackend::Stop: Stopping request queue for server_id %d", entry.first);
            entry.second->request_stop();
        }
    }

    Notice("RrrRpcBackend::Stop: Signaling %zu response queues to stop", queue_holders_response_.size());
    for (auto& entry : queue_holders_response_) {
        if (entry.second) {
            Notice("RrrRpcBackend::Stop: Stopping response queue for server_id %d", entry.first);
            entry.second->request_stop();
        }
    }

    // Note: We don't delete the server here because its destructor blocks on Pthread_join
    // which can deadlock. The server will be cleaned up in the destructor after
    // poll_thread_worker shutdown.
    Notice("RrrRpcBackend::Stop: Server cleanup deferred to destructor");

    // Close all outstanding client connections to unblock any waiting futures.
    Notice("RrrRpcBackend::Stop: Closing client connections");
    std::vector<rusty::Arc<rrr::Client>> clients_to_close;
    {
        std::lock_guard<std::mutex> guard(clients_lock_);
        Notice("RrrRpcBackend::Stop: Found %zu client connections to close", clients_.size());
        for (auto& entry : clients_) {
            if (entry.second) {
                clients_to_close.push_back(entry.second.clone());
            }
        }
        clients_.clear();
    }

    for (auto& client : clients_to_close) {
        try {
            if (client) {
                client->close();
            }
        } catch (const std::exception& e) {
            Warning("RrrRpcBackend::Stop: Exception closing client: %s", e.what());
        } catch (...) {
            Warning("RrrRpcBackend::Stop: Unknown exception closing client");
        }
    }
    Notice("RrrRpcBackend::Stop: Closed %zu client connections", clients_to_close.size());

    // Clean up any remaining pending requests in the map
    {
        std::lock_guard<std::mutex> guard(rrr_request_map_lock_);
        size_t remaining = rrr_request_map_.size();
        if (remaining > 0) {
            Notice("RrrRpcBackend::Stop: Cleaning up %zu remaining pending requests", remaining);
            rrr_request_map_.clear();
        }
    }

    Notice("RrrRpcBackend stats: msg_size_resp_sent: %" PRIu64 " bytes, counter: %d, avg: %lf",
           msg_size_resp_sent_, msg_counter_resp_sent_,
           msg_size_resp_sent_ / (msg_counter_resp_sent_ + 0.0));
    Notice("RrrRpcBackend::Stop: END");
}


// Print statistics
void RrrRpcBackend::PrintStats() {
    Notice("RrrRpcBackend request stats: msg_size_req_sent: %" PRIu64 " bytes, counter: %d, avg: %lf",
           msg_size_req_sent_, msg_counter_req_sent_,
           msg_size_req_sent_ / (msg_counter_req_sent_ + 0.0));
}

// Static request handler for rrr::Server
void RrrRpcBackend::RequestHandler(uint8_t req_type, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn, RrrRpcBackend* backend) {
    if (!backend) {
        Warning("RequestHandler called with null backend pointer!");
        return;
    }

    // Check if backend is stopping - don't process new requests during shutdown
    if (backend->stop_) {
        Debug("RequestHandler: Backend is stopping, ignoring request type %d", req_type);
        return;
    }
    // Upgrade weak reference to get Arc
    auto sconn_opt = weak_sconn.upgrade();
    if (!sconn_opt.is_some()) {
        Warning("ServerConnection closed before handling request");
        return;
    }
    auto sconn = sconn_opt.unwrap();

    // Handle special request types
    if (req_type == watermarkReqType) {
        Debug("Received watermarkReqType");

        // Read request
        basic_request_t basic_req;
        req->m.read(&basic_req, sizeof(basic_req));

        // Prepare response
        get_int_response_t resp;
        resp.result = sync_util::sync_logger::retrieveShardW();
        resp.req_nr = basic_req.req_nr;
        resp.status = ErrorCode::SUCCESS;
        resp.shard_index = TThread::get_shard_index();

        // Send response
        const_cast<rrr::ServerConnection&>(*sconn).begin_reply(*req);
        rrr::Marshal m;
        m.write(&resp, sizeof(resp));
        const_cast<rrr::ServerConnection&>(*sconn) << m;
        const_cast<rrr::ServerConnection&>(*sconn).end_reply();

        backend->msg_size_resp_sent_ += sizeof(resp);
        backend->msg_counter_resp_sent_ += 1;
        return;
    }

    if (req_type == warmupReqType) {
        Debug("Received warmupReqType");

        warmup_request_t warmup_req;
        req->m.read(&warmup_req, sizeof(warmup_req));

        get_int_response_t resp;
        resp.result = 1;
        resp.req_nr = warmup_req.req_nr;
        resp.status = ErrorCode::SUCCESS;
        resp.shard_index = TThread::get_shard_index();

        const_cast<rrr::ServerConnection&>(*sconn).begin_reply(*req);
        rrr::Marshal m;
        m.write(&resp, sizeof(resp));
        const_cast<rrr::ServerConnection&>(*sconn) << m;
        const_cast<rrr::ServerConnection&>(*sconn).end_reply();

        backend->msg_size_resp_sent_ += sizeof(resp);
        backend->msg_counter_resp_sent_ += 1;
        return;
    }

    if (req_type == controlReqType) {
        control_request_t ctrl_req;
        req->m.read(&ctrl_req, sizeof(ctrl_req));

        Warning("Received controlReqType, control: %d, shardIndex: %lld, target_server_id: %llu",
                ctrl_req.control, ctrl_req.value, ctrl_req.targert_server_id);

        bool is_datacenter_failure = ctrl_req.targert_server_id == 10000;

        if (is_datacenter_failure) {
            if (dbtest_callback_)
                dbtest_callback_(ctrl_req.control, ctrl_req.value);
        } else {
            if (bench_callback_)
                bench_callback_(ctrl_req.control, ctrl_req.value);
        }

        get_int_response_t resp;
        resp.result = 0;
        resp.req_nr = ctrl_req.req_nr;
        resp.status = ErrorCode::SUCCESS;
        resp.shard_index = TThread::get_shard_index();

        const_cast<rrr::ServerConnection&>(*sconn).begin_reply(*req);
        rrr::Marshal m;
        m.write(&resp, sizeof(resp));
        const_cast<rrr::ServerConnection&>(*sconn) << m;
        const_cast<rrr::ServerConnection&>(*sconn).end_reply();

        backend->msg_size_resp_sent_ += sizeof(resp);
        backend->msg_counter_resp_sent_ += 1;
        return;
    }

    // Normal requests: enqueue to helper queue
    // Extract request size first to determine server ID before creating RrrRequestHandle
    size_t req_size = req->m.content_size();
    if (req_size < sizeof(TargetServerIDReader)) {
        Warning("Request too small to contain server ID: %zu < %zu", req_size, sizeof(TargetServerIDReader));
        return;
    }

    // Peek at request data to extract server ID
    std::vector<char> temp_buffer(req_size);
    req->m.read(temp_buffer.data(), req_size);
    auto* target_server_id_reader = (TargetServerIDReader*)temp_buffer.data();
    uint16_t target_server_id = target_server_id_reader->targert_server_id;

    // Create RrrRequestHandle with backend and server_id
    auto rrr_handle = std::make_unique<RrrRequestHandle>(std::move(req), sconn, req_type, backend, target_server_id);

    // Store the already-extracted request data
    rrr_handle->request_data = std::move(temp_buffer);

    // Find the appropriate helper queue
    auto it = backend->queue_holders_.find(target_server_id);
    if (it == backend->queue_holders_.end()) {
        Warning("No helper queue found for server_id %d (available queues: %zu)",
                target_server_id, backend->queue_holders_.size());
        // Print all available queue IDs
        for (auto& q : backend->queue_holders_) {
            Warning("  Available queue for server_id: %d", q.first);
        }
        return;
    }
    auto* helper_queue = it->second;

    // Allocate response buffer (helper thread will fill this)
    rrr_handle->response_data.resize(8192);  // Max response size

    // Store in map and get pointer to use as key
    void* key = rrr_handle.get();
    backend->rrr_request_map_lock_.lock();
    backend->rrr_request_map_[key] = std::move(rrr_handle);
    backend->rrr_request_map_lock_.unlock();

    // Enqueue to helper queue (cast void* to erpc::ReqHandle* for compatibility)
    helper_queue->add_one_req(reinterpret_cast<erpc::ReqHandle*>(key), 0);
}

// RrrRequestHandle::EnqueueResponse - enqueues response to response queue
void RrrRequestHandle::EnqueueResponse(size_t msg_size) {
    if (!backend) {
        Warning("RrrRequestHandle::EnqueueResponse: backend is null!");
        return;
    }

    // Find the response queue for this server
    auto it = backend->GetHelperQueuesResponse().find(server_id);
    if (it == backend->GetHelperQueuesResponse().end()) {
        Warning("RrrRequestHandle::EnqueueResponse: No response queue found for server_id %d", server_id);
        return;
    }

    auto* response_queue = it->second;

    // Enqueue response (using GetOpaqueHandle as the key, same as for requests)
    response_queue->add_one_req(reinterpret_cast<erpc::ReqHandle*>(GetOpaqueHandle()), msg_size);
}
