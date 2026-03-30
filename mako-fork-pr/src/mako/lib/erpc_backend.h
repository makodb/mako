// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * erpc_backend.h:
 *   eRPC transport backend implementation
 *   High-performance RDMA-based RPC (1-2 μs latency)
 *
 **********************************************************************/

#ifndef _MAKO_ERPC_BACKEND_H_
#define _MAKO_ERPC_BACKEND_H_

#include "transport_backend.h"
#include "lib/configuration.h"
#include "lib/transport.h"
#include "lib/helper_queue.h"

// eRPC library
#include "rpc.h"
#include "rpc_constants.h"
#include "util/numautils.h"

#include <map>
#include <vector>
#include <unordered_map>
#include <boost/unordered_map.hpp>

namespace mako {

// Forward declarations
class HelperQueue;

// A tag attached to every request we send
struct req_tag_t {
    erpc::MsgBuffer req_msgbuf;
    erpc::MsgBuffer resp_msgbuf;
    uint8_t reqType;
    TransportReceiver *src;
};

// Memory pool for preallocated objects
template <class T>
class AppMemPool {
public:
    size_t num_to_alloc = 1;
    std::vector<T *> backing_ptr_vec;
    std::vector<T *> pool;

    void extend_pool() {
        T *backing_ptr = new T[num_to_alloc];
        for (size_t i = 0; i < num_to_alloc; i++)
            pool.push_back(&backing_ptr[i]);
        backing_ptr_vec.push_back(backing_ptr);
        num_to_alloc *= 2;
    }

    T *alloc() {
        if (pool.empty())
            extend_pool();
        T *ret = pool.back();
        pool.pop_back();
        return ret;
    }

    void free(T *t) { pool.push_back(t); }

    AppMemPool() {}
    ~AppMemPool() {
        for (T *ptr : backing_ptr_vec)
            delete[] ptr;
    }
};

// Forward declarations
class ErpcRequestHandle;
class ErpcBackend;

// eRPC context
class AppContext {
public:
    struct {
        req_tag_t *crt_req_tag;
        AppMemPool<req_tag_t> req_tag_pool;
        boost::unordered_map<TransportReceiver *,
            boost::unordered_map<std::tuple<uint8_t, uint8_t, uint16_t>, int>> sessions;
    } client;

    erpc::Rpc<erpc::CTransport> *rpc = nullptr;

    // Backend pointer for request handler access
    ErpcBackend* backend = nullptr;

    // Queues for helper threads
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders;
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_response;

    // ErpcRequestHandle wrapper storage for helper queue processing
    // Maps void* (used as key) to ErpcRequestHandle wrapper
    std::map<void*, std::unique_ptr<ErpcRequestHandle>> erpc_request_map;
    std::mutex erpc_request_map_lock;

    // Statistics
    uint64_t msg_size_req_sent = 0;
    int msg_counter_req_sent = 0;
    uint64_t msg_size_resp_sent = 0;
    int msg_counter_resp_sent = 0;
};

/**
 * eRPC Transport Backend
 *
 * High-performance RDMA-based RPC implementation using the eRPC library.
 * Provides low-latency (~1-2 μs) inter-shard communication.
 *
 * Thread safety: Individual methods are NOT thread-safe. Caller must
 * ensure proper synchronization if using from multiple threads.
 */
class ErpcBackend : public TransportBackend {
public:
    ErpcBackend(const transport::Configuration& config,
                int shard_idx,
                uint16_t id,
                const std::string& cluster);

    virtual ~ErpcBackend();

    // TransportBackend interface implementation
    int Initialize(const std::string& local_uri,
                   uint8_t numa_node,
                   uint8_t phy_port,
                   uint8_t st_nr_req_types,
                   uint8_t end_nr_req_types) override;

    void Shutdown() override;

    char* AllocRequestBuffer(size_t req_len, size_t resp_len) override;
    void FreeRequestBuffer() override;

    bool SendToShard(TransportReceiver* src,
                    uint8_t req_type,
                    uint8_t shard_idx,
                    uint16_t server_id,
                    size_t msg_len) override;

    bool SendToAll(TransportReceiver* src,
                   uint8_t req_type,
                   int shards_bit_set,
                   uint16_t server_id,
                   size_t resp_len,
                   size_t req_len,
                   int force_center = -1) override;

    bool SendBatchToAll(TransportReceiver* src,
                       uint8_t req_type,
                       uint16_t server_id,
                       size_t resp_len,
                       const std::map<int, std::pair<char*, size_t>>& data) override;

    void RunEventLoop() override;
    void Stop() override;

    void PrintStats() override;

    TransportType GetType() const override { return TransportType::ERPC; }

    // eRPC-specific methods
    int GetSession(TransportReceiver* src,
                   uint8_t shard_idx,
                   uint16_t server_id,
                   int force_center = -1);

    int HandleTimeout(size_t start_tsc, int req_type, const std::string& extra);

    void RunNoQueue();  // For exchange server

    // Access to internal context (for compatibility)
    AppContext* GetContext() { return context_; }

    // Set helper queues
    void SetHelperQueues(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues) {
        context_->queue_holders = queues;
    }

    void SetHelperQueuesResponse(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues) {
        context_->queue_holders_response = queues;
    }

    void SetBreakTimeout(bool bt) { break_timeout_ = bt; }

    // Get helper queues (for compatibility with existing code)
    const std::unordered_map<uint16_t, mako::HelperQueue*>& GetHelperQueues() const {
        return context_->queue_holders;
    }

    const std::unordered_map<uint16_t, mako::HelperQueue*>& GetHelperQueuesResponse() const {
        return context_->queue_holders_response;
    }

private:
    // Configuration
    transport::Configuration config_;
    int shard_idx_;
    uint16_t id_;
    std::string cluster_;
    int cluster_role_;

    // eRPC state
    erpc::Nexus* nexus_{nullptr};
    AppContext* context_{nullptr};

    // Runtime state
    bool stop_{false};
    bool break_timeout_{false};
    uint8_t numa_node_{0};
    uint8_t phy_port_{0};

    // Performance monitoring
    double freq_ghz_{0.0};
    int ms1_cycles_{0};
    size_t start_transport_{0};
    std::chrono::high_resolution_clock::time_point start_transport_clock_;

    // Static callbacks for eRPC
    static void SessionManagementHandler(int session_num,
                                        erpc::SmEventType sm_event_type,
                                        erpc::SmErrType sm_err_type,
                                        void *context);

    static void RequestHandler(erpc::ReqHandle *req_handle, void *context);

    static void ResponseHandler(void *context, void *tag);
};

} // namespace mako

#endif  /* _MAKO_ERPC_BACKEND_H_ */
