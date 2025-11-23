#include "transport/rdma_transport.h"
#include "../base/all.hpp"

#ifdef ENABLE_RDMA

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <mutex>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

namespace rrr {

// Static members
RDMAContext* RDMATransport::shared_context_ = nullptr;
std::mutex RDMATransport::context_mutex_;

// ============================================================================
// RDMAContext Implementation
// ============================================================================

RDMAContext::RDMAContext() 
    : ctx_(nullptr), pd_(nullptr), comp_channel_(nullptr), cq_(nullptr), cq_fd_(-1) {
}

RDMAContext::~RDMAContext() {
    cleanup();
}

bool RDMAContext::initialize() {
    // Get RDMA device list
    int num_devices = 0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        Log_error("RDMAContext: No RDMA devices found");
        return false;
    }
    
    // Open first available device
    ctx_ = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    
    if (!ctx_) {
        Log_error("RDMAContext: Failed to open RDMA device");
        return false;
    }
    
    // Allocate protection domain
    pd_ = ibv_alloc_pd(ctx_);
    if (!pd_) {
        Log_error("RDMAContext: Failed to allocate protection domain");
        ibv_close_device(ctx_);
        ctx_ = nullptr;
        return false;
    }
    
    // Create completion channel
    comp_channel_ = ibv_create_comp_channel(ctx_);
    if (!comp_channel_) {
        Log_error("RDMAContext: Failed to create completion channel");
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
        ibv_close_device(ctx_);
        ctx_ = nullptr;
        return false;
    }
    
    // Create completion queue
    cq_ = ibv_create_cq(ctx_, 10, nullptr, comp_channel_, 0);
    if (!cq_) {
        Log_error("RDMAContext: Failed to create completion queue");
        ibv_destroy_comp_channel(comp_channel_);
        comp_channel_ = nullptr;
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
        ibv_close_device(ctx_);
        ctx_ = nullptr;
        return false;
    }
    
    // Request completion notifications
    if (ibv_req_notify_cq(cq_, 0) != 0) {
        Log_error("RDMAContext: Failed to request CQ notifications");
        cleanup();
        return false;
    }
    
    // Get completion queue file descriptor
    cq_fd_ = comp_channel_->fd;
    
    Log_debug("RDMAContext: Initialized successfully, CQ fd=%d", cq_fd_);
    return true;
}

void RDMAContext::cleanup() {
    if (cq_) {
        ibv_destroy_cq(cq_);
        cq_ = nullptr;
    }
    if (comp_channel_) {
        ibv_destroy_comp_channel(comp_channel_);
        comp_channel_ = nullptr;
    }
    if (pd_) {
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
    }
    if (ctx_) {
        ibv_close_device(ctx_);
        ctx_ = nullptr;
    }
    cq_fd_ = -1;
}

// ============================================================================
// RDMAConnection Implementation
// ============================================================================

RDMAConnection::RDMAConnection(RDMAContext* ctx)
    : ctx_(ctx), cm_id_(nullptr), qp_(nullptr),
      send_mr_(nullptr), recv_mr_(nullptr),
      send_buffer_(nullptr), recv_buffer_(nullptr),
      send_buffer_used_(0), recv_buffer_used_(0) {
}

RDMAConnection::~RDMAConnection() {
    cleanup();
}

bool RDMAConnection::setup_qp(struct rdma_cm_id* cm_id) {
    cm_id_ = cm_id;
    
    if (!ctx_ || !ctx_->is_initialized()) {
        Log_error("RDMAConnection: Context not initialized");
        return false;
    }
    
    // Get queue pair from cm_id
    qp_ = cm_id_->qp;
    if (!qp_) {
        Log_error("RDMAConnection: Queue pair not available");
        return false;
    }
    
    // Allocate send buffer
    send_buffer_ = (char*)malloc(BUFFER_SIZE);
    if (!send_buffer_) {
        Log_error("RDMAConnection: Failed to allocate send buffer");
        return false;
    }
    
    // Register send buffer memory region
    send_mr_ = ibv_reg_mr(ctx_->get_pd(), send_buffer_, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!send_mr_) {
        Log_error("RDMAConnection: Failed to register send memory region");
        free(send_buffer_);
        send_buffer_ = nullptr;
        return false;
    }
    
    // Allocate receive buffer
    recv_buffer_ = (char*)malloc(BUFFER_SIZE);
    if (!recv_buffer_) {
        Log_error("RDMAConnection: Failed to allocate recv buffer");
        ibv_dereg_mr(send_mr_);
        send_mr_ = nullptr;
        free(send_buffer_);
        send_buffer_ = nullptr;
        return false;
    }
    
    // Register receive buffer memory region
    recv_mr_ = ibv_reg_mr(ctx_->get_pd(), recv_buffer_, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!recv_mr_) {
        Log_error("RDMAConnection: Failed to register recv memory region");
        ibv_dereg_mr(send_mr_);
        send_mr_ = nullptr;
        free(send_buffer_);
        send_buffer_ = nullptr;
        free(recv_buffer_);
        recv_buffer_ = nullptr;
        return false;
    }
    
    Log_debug("RDMAConnection: Setup complete");
    return true;
}

void RDMAConnection::cleanup() {
    if (recv_mr_) {
        ibv_dereg_mr(recv_mr_);
        recv_mr_ = nullptr;
    }
    if (send_mr_) {
        ibv_dereg_mr(send_mr_);
        send_mr_ = nullptr;
    }
    if (recv_buffer_) {
        free(recv_buffer_);
        recv_buffer_ = nullptr;
    }
    if (send_buffer_) {
        free(send_buffer_);
        send_buffer_ = nullptr;
    }
    qp_ = nullptr;
    cm_id_ = nullptr;
}

ssize_t RDMAConnection::post_send(const void* buf, size_t len) {
    if (!qp_ || !send_mr_ || len > BUFFER_SIZE) {
        return -1;
    }
    
    // Copy data to send buffer
    memcpy(send_buffer_, buf, len);
    send_buffer_used_ = len;
    
    // Prepare send work request
    struct ibv_sge sge;
    sge.addr = (uintptr_t)send_buffer_;
    sge.length = len;
    sge.lkey = send_mr_->lkey;
    
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    
    struct ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr) != 0) {
        Log_error("RDMAConnection: Failed to post send");
        return -1;
    }
    
    return len;
}

ssize_t RDMAConnection::post_recv(void* buf, size_t len) {
    if (!qp_ || !recv_mr_) {
        return -1;
    }
    
    // Prepare receive work request (pre-post for next message)
    struct ibv_sge sge;
    sge.addr = (uintptr_t)recv_buffer_;
    sge.length = BUFFER_SIZE;
    sge.lkey = recv_mr_->lkey;
    
    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    
    struct ibv_recv_wr* bad_wr = nullptr;
    if (ibv_post_recv(qp_, &wr, &bad_wr) != 0) {
        Log_error("RDMAConnection: Failed to post recv");
        return -1;
    }
    
    return 0;
}

bool RDMAConnection::poll_completions() {
    if (!ctx_ || !ctx_->get_cq()) {
        return false;
    }
    
    struct ibv_cq* cq = ctx_->get_cq();
    struct ibv_wc wc;
    int num_completions = 0;
    
    // Poll completion queue
    while (ibv_poll_cq(cq, 1, &wc) > 0) {
        num_completions++;
        
        if (wc.status != IBV_WC_SUCCESS) {
            Log_error("RDMAConnection: Work completion error: %s", ibv_wc_status_str(wc.status));
            return false;
        }
        
        if (wc.opcode == IBV_WC_RECV) {
            // Received data - store length
            recv_buffer_used_ = wc.byte_len;
        } else if (wc.opcode == IBV_WC_SEND) {
            // Send completed
            send_buffer_used_ = 0;
        }
    }
    
    // Request next notification if we got completions
    if (num_completions > 0) {
        if (ibv_req_notify_cq(cq, 0) != 0) {
            Log_error("RDMAConnection: Failed to request CQ notification");
        }
    }
    
    return num_completions > 0;
}

ssize_t RDMAConnection::read_received_data(void* buf, size_t len) {
    if (!recv_buffer_ || recv_buffer_used_ == 0) {
        return 0;
    }
    
    // Copy received data to user buffer
    size_t copy_len = (len < recv_buffer_used_) ? len : recv_buffer_used_;
    if (buf) {
        memcpy(buf, recv_buffer_, copy_len);
    }
    
    // Clear used flag after reading
    recv_buffer_used_ = 0;
    
    return copy_len;
}

// ============================================================================
// RDMATransport Implementation
// ============================================================================

RDMATransport::RDMATransport()
    : ctx_(nullptr), conn_(nullptr), cm_id_(nullptr), event_channel_(nullptr),
      is_listening_(false), is_connected_(false) {
}

RDMATransport::RDMATransport(RDMAConnection* conn, struct rdma_cm_id* cm_id)
    : ctx_(nullptr), conn_(conn), cm_id_(cm_id), event_channel_(nullptr),
      is_listening_(false), is_connected_(true) {
    if (conn_) {
        ctx_ = get_or_create_context();
    }
}

RDMATransport::~RDMATransport() {
    close();
}

RDMAContext* RDMATransport::get_or_create_context() {
    std::lock_guard<std::mutex> lock(context_mutex_);
    
    if (!shared_context_) {
        shared_context_ = new RDMAContext();
        if (!shared_context_->initialize()) {
            delete shared_context_;
            shared_context_ = nullptr;
            return nullptr;
        }
    }
    
    return shared_context_;
}

int RDMATransport::connect(const char* addr) {
    if (is_connected_ || cm_id_) {
        Log_error("RDMATransport: Already connected or connection exists");
        return EINVAL;
    }
    
    addr_ = addr;
    
    // Get or create RDMA context
    ctx_ = get_or_create_context();
    if (!ctx_ || !ctx_->is_initialized()) {
        Log_error("RDMATransport: Failed to initialize RDMA context");
        return EINVAL;
    }
    
    // Create event channel
    event_channel_ = rdma_create_event_channel();
    if (!event_channel_) {
        Log_error("RDMATransport: Failed to create event channel");
        return errno;
    }
    
    // Create RDMA CM ID
    if (rdma_create_id(event_channel_, &cm_id_, nullptr, RDMA_PS_TCP) != 0) {
        Log_error("RDMATransport: Failed to create CM ID");
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    // Parse address
    std::string addr_str(addr);
    size_t idx = addr_str.find(":");
    if (idx == std::string::npos) {
        Log_error("RDMATransport: Bad address format: %s", addr);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    std::string host = addr_str.substr(0, idx);
    std::string port = addr_str.substr(idx + 1);
    
    // Resolve address
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int ret = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (ret != 0) {
        Log_error("RDMATransport: getaddrinfo failed: %s", gai_strerror(ret));
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    
    // Resolve RDMA address
    if (rdma_resolve_addr(cm_id_, nullptr, res->ai_addr, 2000) != 0) {
        Log_error("RDMATransport: Failed to resolve RDMA address");
        freeaddrinfo(res);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    freeaddrinfo(res);
    
    // Wait for address resolution event
    struct rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(event_channel_, &event) != 0) {
        Log_error("RDMATransport: Failed to get CM event");
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        Log_error("RDMATransport: Unexpected event: %d", event->event);
        rdma_ack_cm_event(event);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    rdma_ack_cm_event(event);
    
    // Resolve route
    if (rdma_resolve_route(cm_id_, 2000) != 0) {
        Log_error("RDMATransport: Failed to resolve route");
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    // Wait for route resolution event
    if (rdma_get_cm_event(event_channel_, &event) != 0) {
        Log_error("RDMATransport: Failed to get route resolution event");
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        Log_error("RDMATransport: Unexpected event: %d", event->event);
        rdma_ack_cm_event(event);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    rdma_ack_cm_event(event);
    
    // Create queue pair
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 0;
    qp_attr.qp_context = cm_id_;
    qp_attr.sq_sig_all = 0;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.send_cq = ctx_->get_cq();
    qp_attr.recv_cq = ctx_->get_cq();
    
    if (rdma_create_qp(cm_id_, ctx_->get_pd(), &qp_attr) != 0) {
        Log_error("RDMATransport: Failed to create queue pair");
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    // Create connection
    conn_ = new RDMAConnection(ctx_);
    if (!conn_->setup_qp(cm_id_)) {
        Log_error("RDMATransport: Failed to setup connection");
        delete conn_;
        conn_ = nullptr;
        rdma_destroy_qp(cm_id_);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    
    // Connect
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 3;
    
    if (rdma_connect(cm_id_, &conn_param) != 0) {
        Log_error("RDMATransport: Failed to connect");
        conn_->cleanup();
        delete conn_;
        conn_ = nullptr;
        rdma_destroy_qp(cm_id_);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    // Wait for connection established event
    if (rdma_get_cm_event(event_channel_, &event) != 0) {
        Log_error("RDMATransport: Failed to get connection event");
        conn_->cleanup();
        delete conn_;
        conn_ = nullptr;
        rdma_destroy_qp(cm_id_);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        Log_error("RDMATransport: Connection failed, event: %d", event->event);
        rdma_ack_cm_event(event);
        conn_->cleanup();
        delete conn_;
        conn_ = nullptr;
        rdma_destroy_qp(cm_id_);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return ECONNREFUSED;
    }
    rdma_ack_cm_event(event);
    
    is_connected_ = true;
    Log_debug("RDMATransport: Connected to %s", addr);
    return 0;
}

int RDMATransport::bind(const char* addr) {
    if (cm_id_ || is_listening_) {
        Log_error("RDMATransport: Already bound or listening");
        return EINVAL;
    }
    
    addr_ = addr;
    
    // Get or create RDMA context
    ctx_ = get_or_create_context();
    if (!ctx_ || !ctx_->is_initialized()) {
        Log_error("RDMATransport: Failed to initialize RDMA context");
        return EINVAL;
    }
    
    // Create event channel
    event_channel_ = rdma_create_event_channel();
    if (!event_channel_) {
        Log_error("RDMATransport: Failed to create event channel");
        return errno;
    }
    
    // Create RDMA CM ID
    if (rdma_create_id(event_channel_, &cm_id_, nullptr, RDMA_PS_TCP) != 0) {
        Log_error("RDMATransport: Failed to create CM ID");
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    
    // Parse address
    std::string addr_str(addr);
    size_t idx = addr_str.find(":");
    if (idx == std::string::npos) {
        Log_error("RDMATransport: Bad address format: %s", addr);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    std::string host = addr_str.substr(0, idx);
    std::string port = addr_str.substr(idx + 1);
    
    // Resolve address
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    int ret = getaddrinfo((host == "0.0.0.0") ? nullptr : host.c_str(), port.c_str(), &hints, &res);
    if (ret != 0) {
        Log_error("RDMATransport: getaddrinfo failed: %s", gai_strerror(ret));
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return EINVAL;
    }
    
    // Bind to address
    if (rdma_bind_addr(cm_id_, res->ai_addr) != 0) {
        Log_error("RDMATransport: Failed to bind address");
        freeaddrinfo(res);
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
        return errno;
    }
    freeaddrinfo(res);
    
    Log_debug("RDMATransport: Bound to %s", addr);
    return 0;
}

int RDMATransport::listen() {
    if (!cm_id_) {
        Log_error("RDMATransport: CM ID not created");
        return EINVAL;
    }
    
    if (rdma_listen(cm_id_, 10) != 0) {
        Log_error("RDMATransport: Failed to listen");
        return errno;
    }
    
    is_listening_ = true;
    Log_debug("RDMATransport: Listening for connections");
    return 0;
}

std::shared_ptr<Transport> RDMATransport::accept() {
    if (!is_listening_ || !cm_id_ || !event_channel_) {
        return nullptr;
    }
    
    // Get connection request event
    struct rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(event_channel_, &event) != 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            Log_error("RDMATransport: Failed to get connection request event");
        }
        return nullptr;
    }
    
    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        Log_debug("RDMATransport: Unexpected event: %d", event->event);
        rdma_ack_cm_event(event);
        return nullptr;
    }
    
    struct rdma_cm_id* new_cm_id = event->id;
    rdma_ack_cm_event(event);
    
    // Create queue pair for new connection
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 0;
    qp_attr.qp_context = new_cm_id;
    qp_attr.sq_sig_all = 0;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.send_cq = ctx_->get_cq();
    qp_attr.recv_cq = ctx_->get_cq();
    
    if (rdma_create_qp(new_cm_id, ctx_->get_pd(), &qp_attr) != 0) {
        Log_error("RDMATransport: Failed to create queue pair for new connection");
        rdma_reject(new_cm_id, nullptr, 0);
        rdma_destroy_id(new_cm_id);
        return nullptr;
    }
    
    // Create connection
    RDMAConnection* new_conn = new RDMAConnection(ctx_);
    if (!new_conn->setup_qp(new_cm_id)) {
        Log_error("RDMATransport: Failed to setup new connection");
        delete new_conn;
        rdma_destroy_qp(new_cm_id);
        rdma_reject(new_cm_id, nullptr, 0);
        rdma_destroy_id(new_cm_id);
        return nullptr;
    }
    
    // Accept connection
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    
    if (rdma_accept(new_cm_id, &conn_param) != 0) {
        Log_error("RDMATransport: Failed to accept connection");
        new_conn->cleanup();
        delete new_conn;
        rdma_destroy_qp(new_cm_id);
        rdma_reject(new_cm_id, nullptr, 0);
        rdma_destroy_id(new_cm_id);
        return nullptr;
    }
    
    // Wait for connection established event
    if (rdma_get_cm_event(event_channel_, &event) != 0) {
        Log_error("RDMATransport: Failed to get connection established event");
        new_conn->cleanup();
        delete new_conn;
        rdma_destroy_qp(new_cm_id);
        rdma_destroy_id(new_cm_id);
        return nullptr;
    }
    
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        Log_error("RDMATransport: Connection not established, event: %d", event->event);
        rdma_ack_cm_event(event);
        new_conn->cleanup();
        delete new_conn;
        rdma_destroy_qp(new_cm_id);
        rdma_destroy_id(new_cm_id);
        return nullptr;
    }
    rdma_ack_cm_event(event);
    
    Log_debug("RDMATransport: Accepted new connection, fd=%d", new_conn->get_cq_fd());
    
    // Create new transport for accepted connection
    return std::make_shared<RDMATransport>(new_conn, new_cm_id);
}

ssize_t RDMATransport::read(void* buf, size_t len) {
    if (!is_connected_ || !conn_) {
        return -1;
    }
    
    // Poll for completions first (this will populate recv_buffer_)
    conn_->poll_completions();
    
    // Read received data
    ssize_t ret = conn_->read_received_data(buf, len);
    
    // Pre-post receive for next message
    if (ret > 0) {
        conn_->post_recv(nullptr, 0);
    }
    
    return ret;
}

ssize_t RDMATransport::write(const void* buf, size_t len) {
    if (!is_connected_ || !conn_) {
        return -1;
    }
    
    // Post send
    ssize_t ret = conn_->post_send(buf, len);
    if (ret > 0) {
        // Poll for send completion
        conn_->poll_completions();
    }
    return ret;
}

void RDMATransport::close() {
    if (conn_) {
        conn_->cleanup();
        delete conn_;
        conn_ = nullptr;
    }
    
    if (cm_id_) {
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
    }
    
    if (event_channel_) {
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
    }
    
    is_connected_ = false;
    is_listening_ = false;
}

int RDMATransport::fd() {
    if (conn_) {
        return conn_->get_cq_fd();
    }
    if (ctx_) {
        return ctx_->get_cq_fd();
    }
    return -1;
}

bool RDMATransport::is_connected() {
    return is_connected_ && conn_ != nullptr;
}

} // namespace rrr

