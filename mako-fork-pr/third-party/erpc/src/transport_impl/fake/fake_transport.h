/**
 * @file fake_transport.h
 * @brief Socket-based transport implementation for eRPC that provides
 * wide compatibility across different server platforms.
 */
#pragma once

#ifdef ERPC_FAKE

#include "transport.h"
#include "lockfree_queue.h"
#include "packet_pool.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <thread>
#include <atomic>

namespace erpc {

class FakeTransport : public Transport {
 public:
  static constexpr TransportType kTransportType = TransportType::kFake;
  static constexpr size_t kMTU = 1024;  // Match other eRPC transports
  static constexpr size_t kPostlist = 16;
  static constexpr size_t kUnsigBatch = 64;
  static constexpr size_t kMaxDataPerPkt = (kMTU - sizeof(pkthdr_t));

  /// Socket routing info structure embedded in routing_info_t
  struct socket_routing_info_t {
    uint32_t ipv4_addr;  ///< IPv4 address in network byte order
    uint16_t udp_port;   ///< UDP port in host byte order
    uint16_t padding;    ///< Padding for alignment
  };

  FakeTransport(uint16_t sm_udp_port, uint8_t rpc_id, uint8_t phy_port, 
                size_t numa_node, FILE *trace_file);
  ~FakeTransport();

  void init_hugepage_structures(HugeAlloc *huge_alloc, uint8_t **rx_ring);
  void init_mem_reg_funcs();
  void fill_local_routing_info(routing_info_t *routing_info) const;

  bool resolve_remote_routing_info(routing_info_t *routing_info);
  size_t get_bandwidth() const;

  static std::string routing_info_str(routing_info_t *routing_info);

  void tx_burst(const tx_burst_item_t *tx_burst_arr, size_t num_pkts);
  void tx_flush();
  size_t rx_burst();
  void post_recvs(size_t num_recvs);

 private:
  /**
   * @brief Resolve the local IP address for socket communication
   */
  void resolve_local_ip_address();

  // Socket state
  int socket_fd_;
  int epoll_fd_;  // Epoll for event-driven socket polling
  uint16_t local_port_;
  struct sockaddr_in local_addr_;
  uint32_t local_ipv4_addr_;  // Local IP address (resolved dynamically)

  // Receive thread and buffers
  std::thread *rx_thread_;
  std::atomic<bool> stop_rx_thread_;

  // Lock-free packet queue (SPMC: single producer RX thread, multiple consumer workers)
  static constexpr size_t kRxQueueCapacity = 16384;  // Must be power of 2
  LockFreeSPMCQueue<PacketInfo, kRxQueueCapacity> rx_packet_queue_;

  // Packet buffer pool (lock-free allocation)
  PacketPool packet_pool_;

  // Receive ring buffer management
  uint8_t **rx_ring_;  // Pointer to eRPC's rx_ring array
  std::atomic<size_t> rx_tail_;  // Atomic to avoid races

  // Batched receive support
  static constexpr size_t kRecvBatchSize = 32;  // Receive up to 32 packets per syscall

  void rx_thread_func();
  void cleanup_rx_thread();
};

}  // namespace erpc

#endif
