#ifdef ERPC_FAKE
#include "fake_transport.h"
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <chrono>
#include <stdexcept>

namespace erpc {

constexpr size_t FakeTransport::kMaxDataPerPkt;

FakeTransport::FakeTransport(uint16_t sm_udp_port, uint8_t rpc_id,
                            uint8_t phy_port, size_t numa_node,
                            FILE *trace_file)
    : Transport(TransportType::kFake, rpc_id, phy_port, numa_node, trace_file),
      socket_fd_(-1), epoll_fd_(-1), local_port_(sm_udp_port + 10000 + rpc_id),
      rx_thread_(nullptr), stop_rx_thread_(false), rx_ring_(nullptr), rx_tail_(0) {

  // Resolve local IP address for socket communication
  resolve_local_ip_address();

  // Create UDP socket
  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    throw std::runtime_error("FakeTransport: Failed to create socket: " +
                           std::string(strerror(errno)));
  }

  // Set socket options
  int reuse = 1;
  if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    close(socket_fd_);
    throw std::runtime_error("FakeTransport: Failed to set SO_REUSEADDR: " +
                           std::string(strerror(errno)));
  }

  // Set SO_REUSEPORT for scalable multi-threaded receive (kernel load balancing)
  if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
    close(socket_fd_);
    throw std::runtime_error("FakeTransport: Failed to set SO_REUSEPORT: " +
                           std::string(strerror(errno)));
  }

  // Bind socket to local address
  memset(&local_addr_, 0, sizeof(local_addr_));
  local_addr_.sin_family = AF_INET;
  local_addr_.sin_addr.s_addr = INADDR_ANY;
  local_addr_.sin_port = htons(local_port_);

  if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&local_addr_),
           sizeof(local_addr_)) < 0) {
    close(socket_fd_);
    throw std::runtime_error("FakeTransport: Failed to bind socket: " +
                           std::string(strerror(errno)));
  }

  // Set non-blocking mode
  int flags = fcntl(socket_fd_, F_GETFL, 0);
  if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(socket_fd_);
    throw std::runtime_error("FakeTransport: Failed to set non-blocking: " +
                           std::string(strerror(errno)));
  }

  // Create epoll instance for event-driven polling
  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) {
    close(socket_fd_);
    throw std::runtime_error("FakeTransport: Failed to create epoll: " +
                           std::string(strerror(errno)));
  }

  // Add socket to epoll
  struct epoll_event ev;
  ev.events = EPOLLIN;  // Level-triggered for lower latency (no EPOLLET)
  ev.data.fd = socket_fd_;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket_fd_, &ev) < 0) {
    close(epoll_fd_);
    close(socket_fd_);
    throw std::runtime_error("FakeTransport: Failed to add socket to epoll: " +
                           std::string(strerror(errno)));
  }

  // Initialize memory registration functions
  init_mem_reg_funcs();
}

FakeTransport::~FakeTransport() {
  cleanup_rx_thread();

  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
  }

  if (socket_fd_ >= 0) {
    close(socket_fd_);
  }

  // Clean up any remaining packets in lock-free queue
  while (!rx_packet_queue_.empty()) {
    auto result = rx_packet_queue_.try_pop();
    if (result.first) {
      packet_pool_.free(result.second.get_data());
    }
  }
}

void FakeTransport::init_mem_reg_funcs() {
  // For fake transport using UDP sockets, we don't need actual memory registration
  // Provide dummy functions that return valid but unused values
  reg_mr_func_ = [](void* /* buf */, size_t /* size */) -> Transport::mem_reg_info {
    return Transport::mem_reg_info(nullptr, 0);
  };
  
  dereg_mr_func_ = [](Transport::mem_reg_info /* mr_info */) {
    // Nothing to do for fake transport
  };
}

void FakeTransport::init_hugepage_structures(HugeAlloc *huge_alloc, 
                                            uint8_t **rx_ring) {
  huge_alloc_ = huge_alloc;
  
  // Store pointer to eRPC's rx_ring array for direct access
  rx_ring_ = rx_ring;
  
  // Initialize all ring entries to null
  for (size_t i = 0; i < kNumRxRingEntries; i++) {
    rx_ring_[i] = nullptr;
  }
  
  // Start receive thread
  stop_rx_thread_ = false;
  rx_thread_ = new std::thread(&FakeTransport::rx_thread_func, this);
}

void FakeTransport::fill_local_routing_info(routing_info_t *routing_info) const {
  auto *socket_ri = reinterpret_cast<socket_routing_info_t*>(routing_info->buf_);
  // Use the resolved local IP address
  socket_ri->ipv4_addr = local_ipv4_addr_;
  socket_ri->udp_port = local_port_;
  socket_ri->padding = 0;
  printf("DEBUG: fill_local_routing_info: ipv4_addr=0x%x, udp_port=%u\n", 
         socket_ri->ipv4_addr, socket_ri->udp_port);
  fflush(stdout);
}

// Resolve the port information from the remote server you plan to send to
bool FakeTransport::resolve_remote_routing_info(routing_info_t *routing_info) {
  // Socket routing info is already in the correct format
  // Just validate that we can parse it
  auto *socket_ri = reinterpret_cast<socket_routing_info_t*>(routing_info->buf_);
  bool result = (socket_ri->ipv4_addr != 0 && socket_ri->udp_port != 0);
  printf("DEBUG: resolve_remote_routing_info: ipv4_addr=0x%x, udp_port=%u, result=%d\n", 
         socket_ri->ipv4_addr, socket_ri->udp_port, result);
  fflush(stdout);
  return result;
}

size_t FakeTransport::get_bandwidth() const {
  return 0; // Not implemented for socket
}

std::string FakeTransport::routing_info_str(routing_info_t *routing_info) {
  auto *socket_ri = reinterpret_cast<socket_routing_info_t*>(routing_info->buf_);
  struct in_addr addr;
  addr.s_addr = socket_ri->ipv4_addr;
  return std::string(inet_ntoa(addr)) + ":" + std::to_string(socket_ri->udp_port);
}

void FakeTransport::tx_burst(const tx_burst_item_t *tx_burst_arr, size_t num_pkts) {
  for (size_t i = 0; i < num_pkts; i++) {
    const auto &item = tx_burst_arr[i];
    
    if (item.drop_) continue; // Skip dropped packets

    // Get routing info
    auto *socket_ri = reinterpret_cast<socket_routing_info_t*>(item.routing_info_->buf_);
    
    // Set up destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = socket_ri->ipv4_addr;
    dest_addr.sin_port = htons(socket_ri->udp_port);
    
    // Get packet data from MsgBuffer
    pkthdr_t *pkthdr;
    if (item.pkt_idx_ == 0) {
      pkthdr = item.msg_buffer_->get_pkthdr_0();
    } else {
      pkthdr = item.msg_buffer_->get_pkthdr_n(item.pkt_idx_);
    }
    uint8_t *pkt_buf = reinterpret_cast<uint8_t*>(pkthdr);
    size_t pkt_size = item.msg_buffer_->get_pkt_size<kMaxDataPerPkt>(item.pkt_idx_);
    
    // Send packet
    ssize_t bytes_sent = sendto(socket_fd_, pkt_buf, pkt_size, MSG_DONTWAIT,
                               reinterpret_cast<struct sockaddr*>(&dest_addr),
                               sizeof(dest_addr));
    
    if (bytes_sent < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        if (trace_file_ != nullptr) {
          fprintf(trace_file_, "FakeTransport: Send error: %s\n", strerror(errno));
        }
      }
    }
  }
}

void FakeTransport::tx_flush() {
  // Nothing to do for UDP sockets - packets are sent immediately
}

size_t FakeTransport::rx_burst() {
  size_t packets_processed = 0;

  // Lock-free dequeue - multiple workers can call this concurrently
  while (packets_processed < kPostlist) {
    auto result = rx_packet_queue_.try_pop();
    if (!result.first) {
      // Queue is empty
      break;
    }

    PacketInfo pkt_info = result.second;

    // Store packet pointer directly in eRPC's RX ring
    // Use atomic operations to avoid races between multiple consumers
    size_t tail_val = rx_tail_.fetch_add(1, std::memory_order_relaxed);
    size_t ring_index = tail_val & (kNumRxRingEntries - 1);
    rx_ring_[ring_index] = pkt_info.get_data();

    packets_processed++;
  }

  return packets_processed;
}

void FakeTransport::post_recvs(size_t /* num_recvs */) {
  // Nothing to do - receive thread handles this automatically
}

void FakeTransport::rx_thread_func() {
  // Prepare structures for recvmmsg (batch receive)
  struct mmsghdr msgs[kRecvBatchSize];
  struct iovec iovecs[kRecvBatchSize];
  uint8_t *buffers[kRecvBatchSize];
  struct sockaddr_in addrs[kRecvBatchSize];

  // Pre-allocate buffers from pool
  for (size_t i = 0; i < kRecvBatchSize; i++) {
    buffers[i] = packet_pool_.alloc();
    if (buffers[i] == nullptr) {
      // Pool exhausted during initialization - fatal error
      fprintf(stderr, "FakeTransport: Failed to allocate initial RX buffers\n");
      return;
    }

    iovecs[i].iov_base = buffers[i];
    iovecs[i].iov_len = packet_pool_.packet_size();

    msgs[i].msg_hdr.msg_name = &addrs[i];
    msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
    msgs[i].msg_hdr.msg_iov = &iovecs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_control = nullptr;
    msgs[i].msg_hdr.msg_controllen = 0;
    msgs[i].msg_hdr.msg_flags = 0;
    msgs[i].msg_len = 0;
  }

  struct epoll_event events[kRecvBatchSize];
  const int epoll_timeout_ms = 1;  // 1ms timeout (low latency mode)

  while (!stop_rx_thread_) {
    // Wait for socket to be readable (event-driven polling)
    int nfds = epoll_wait(epoll_fd_, events, kRecvBatchSize, epoll_timeout_ms);

    if (nfds < 0) {
      if (errno == EINTR) continue;  // Interrupted by signal, retry
      if (trace_file_ != nullptr) {
        fprintf(trace_file_, "FakeTransport: epoll_wait error: %s\n", strerror(errno));
      }
      break;
    }

    if (nfds == 0) {
      // Timeout - check stop flag and continue
      continue;
    }

    // Socket is readable - receive batch of packets
    int num_msgs = recvmmsg(socket_fd_, msgs, kRecvBatchSize, MSG_DONTWAIT, nullptr);

    if (num_msgs < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        if (trace_file_ != nullptr) {
          fprintf(trace_file_, "FakeTransport: recvmmsg error: %s\n", strerror(errno));
        }
      }
      continue;
    }

    // Process received packets
    for (int i = 0; i < num_msgs; i++) {
      size_t pkt_size = msgs[i].msg_len;

      // Try to enqueue packet (lock-free)
      PacketInfo pkt_info(buffers[i], pkt_size);
      bool enqueued = rx_packet_queue_.try_push(pkt_info);

      if (!enqueued) {
        // Queue is full - drop packet and free buffer
        packet_pool_.free(buffers[i]);

        if (trace_file_ != nullptr) {
          fprintf(trace_file_, "FakeTransport: RX queue full, dropping packet\n");
        }
      }

      // Allocate new buffer for next receive
      buffers[i] = packet_pool_.alloc();
      if (buffers[i] == nullptr) {
        // Pool exhausted - allocate from heap as fallback
        buffers[i] = static_cast<uint8_t*>(malloc(packet_pool_.packet_size()));
        if (buffers[i] == nullptr) {
          fprintf(stderr, "FakeTransport: Fatal - cannot allocate RX buffer\n");
          stop_rx_thread_ = true;
          return;
        }
      }

      // Update iovec for next receive
      iovecs[i].iov_base = buffers[i];
      iovecs[i].iov_len = packet_pool_.packet_size();
      msgs[i].msg_len = 0;  // Reset message length
    }
  }

  // Cleanup: free any remaining buffers
  for (size_t i = 0; i < kRecvBatchSize; i++) {
    if (buffers[i] != nullptr) {
      packet_pool_.free(buffers[i]);
    }
  }
}

void FakeTransport::cleanup_rx_thread() {
  if (rx_thread_ != nullptr) {
    stop_rx_thread_ = true;
    rx_thread_->join();
    delete rx_thread_;
    rx_thread_ = nullptr;
  }

  if (rx_ring_ != nullptr) {
    // Free any remaining packets in the ring back to pool
    for (size_t i = 0; i < kNumRxRingEntries; i++) {
      if (rx_ring_[i] != nullptr) {
        packet_pool_.free(rx_ring_[i]);
        rx_ring_[i] = nullptr;
      }
    }
    rx_ring_ = nullptr;
  }
}

void FakeTransport::resolve_local_ip_address() {
  // For socket-based transport, we determine local IP by connecting to a remote address
  // This lets the OS choose the best local interface/IP automatically
  int temp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (temp_sock < 0) {
    local_ipv4_addr_ = inet_addr("127.0.0.1");
    return;
  }
  
  // Connect to a public DNS server (doesn't actually send data for UDP)
  struct sockaddr_in remote_addr;
  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_addr.s_addr = inet_addr("8.8.8.8");
  remote_addr.sin_port = htons(53);
  
  if (connect(temp_sock, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
    close(temp_sock);
    local_ipv4_addr_ = inet_addr("127.0.0.1");
    return;
  }
  
  // Get the local address chosen by the OS
  struct sockaddr_in local_addr;
  socklen_t addr_len = sizeof(local_addr);
  if (getsockname(temp_sock, (struct sockaddr*)&local_addr, &addr_len) < 0) {
    close(temp_sock);
    local_ipv4_addr_ = inet_addr("127.0.0.1");
    return;
  }
  
  close(temp_sock);
  local_ipv4_addr_ = local_addr.sin_addr.s_addr;
}

}  // namespace erpc

#endif
