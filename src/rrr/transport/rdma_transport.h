#pragma once

#include "transport/transport.h"
#include "../base/all.hpp"
#ifdef ENABLE_RDMA
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#endif
#include <memory>
#include <vector>
#include <mutex>

namespace rrr {

/**
 * RDMA Context - manages RDMA device, protection domain, and completion queues
 */
class RDMAContext {
private:
    struct ibv_context* ctx_;
    struct ibv_pd* pd_;
    struct ibv_comp_channel* comp_channel_;
    struct ibv_cq* cq_;
    int cq_fd_;  // Completion queue file descriptor for epoll
    
public:
    RDMAContext();
    ~RDMAContext();
    
    bool initialize();
    void cleanup();
    
    struct ibv_context* get_context() { return ctx_; }
    struct ibv_pd* get_pd() { return pd_; }
    struct ibv_cq* get_cq() { return cq_; }
    int get_cq_fd() { return cq_fd_; }
    
    bool is_initialized() { return ctx_ != nullptr && pd_ != nullptr && cq_ != nullptr; }
};

/**
 * RDMA Connection - represents a connected Queue Pair (QP)
 */
class RDMAConnection {
private:
    RDMAContext* ctx_;
    struct rdma_cm_id* cm_id_;
    struct ibv_qp* qp_;
    
    // Memory regions for send/recv buffers
    struct ibv_mr* send_mr_;
    struct ibv_mr* recv_mr_;
    
    // Buffers
    static const size_t BUFFER_SIZE = 1024 * 1024;  // 1MB buffers
    char* send_buffer_;
    char* recv_buffer_;
    size_t send_buffer_used_;
    size_t recv_buffer_used_;
    
    // Completion queue polling
    bool process_completions();
    
public:
    RDMAConnection(RDMAContext* ctx);
    ~RDMAConnection();
    
    bool setup_qp(struct rdma_cm_id* cm_id);
    void cleanup();
    
    struct ibv_qp* get_qp() { return qp_; }
    struct rdma_cm_id* get_cm_id() { return cm_id_; }
    
    // Two-sided RDMA operations
    ssize_t post_send(const void* buf, size_t len);
    ssize_t post_recv(void* buf, size_t len);
    bool poll_completions();
    
    int get_cq_fd() { return ctx_ ? ctx_->get_cq_fd() : -1; }
    
    // Access to receive buffer for reading
    ssize_t read_received_data(void* buf, size_t len);
};

/**
 * RDMA Transport - implements Transport interface using two-sided RDMA
 */
class RDMATransport : public Transport {
private:
    static RDMAContext* shared_context_;  // Shared RDMA context (singleton)
    static std::mutex context_mutex_;
    
    RDMAContext* ctx_;
    RDMAConnection* conn_;
    struct rdma_cm_id* cm_id_;
    struct rdma_event_channel* event_channel_;
    
    bool is_listening_;
    bool is_connected_;
    std::string addr_;
    
    // Helper functions
    static RDMAContext* get_or_create_context();
    bool setup_connection();
    bool wait_for_connection();
    
public:
    RDMATransport();
    virtual ~RDMATransport();
    
    // Transport interface implementation
    virtual int connect(const char* addr) override;
    virtual int bind(const char* addr) override;
    virtual int listen() override;
    virtual std::shared_ptr<Transport> accept() override;
    virtual ssize_t read(void* buf, size_t len) override;
    virtual ssize_t write(const void* buf, size_t len) override;
    virtual void close() override;
    virtual int fd() override;
    virtual bool is_connected() override;
    
    // Constructor for accepted connections (server side)
    RDMATransport(RDMAConnection* conn, struct rdma_cm_id* cm_id);
};

} // namespace rrr

