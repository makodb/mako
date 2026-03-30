/**
 * @file packet_pool.h
 * @brief Lock-free packet buffer pool to avoid malloc overhead
 *
 * Pre-allocates a pool of packet buffers and provides O(1) allocation/deallocation
 * without mutex contention.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace erpc {

/**
 * Lock-free packet buffer pool using a free list.
 *
 * Design:
 * - Pre-allocates all buffers upfront
 * - Free list implemented as lock-free stack
 * - Each buffer stores next pointer in first 8 bytes when free
 * - ABA problem solved by keeping generation counter
 */
class PacketPool {
 public:
  static constexpr size_t kDefaultPacketSize = 2048;  // Larger than MTU for safety
  static constexpr size_t kDefaultPoolSize = 32768;   // Number of pre-allocated packets (64MB total)

  /**
   * Initialize packet pool.
   * @param packet_size Size of each packet buffer
   * @param pool_size Number of packets to pre-allocate
   */
  PacketPool(size_t packet_size = kDefaultPacketSize,
             size_t pool_size = kDefaultPoolSize)
      : packet_size_(packet_size), pool_size_(pool_size), free_head_(0) {

    // Allocate memory for all packets in one block
    size_t total_size = pool_size_ * packet_size_;
    // Use posix_memalign for C++14 compatibility
    void* ptr = nullptr;
    int ret = posix_memalign(&ptr, 64, total_size);
    if (ret != 0 || ptr == nullptr) {
      throw std::bad_alloc();
    }
    memory_block_ = static_cast<uint8_t*>(ptr);

    // Initialize free list - chain all packets together
    for (size_t i = 0; i < pool_size_ - 1; i++) {
      uint8_t* packet = memory_block_ + i * packet_size_;
      uint8_t* next_packet = memory_block_ + (i + 1) * packet_size_;

      // Store next pointer in first 8 bytes
      *reinterpret_cast<uint8_t**>(packet) = next_packet;
    }

    // Last packet points to null
    uint8_t* last_packet = memory_block_ + (pool_size_ - 1) * packet_size_;
    *reinterpret_cast<uint8_t**>(last_packet) = nullptr;

    // Free list head points to first packet
    // Use tagged pointer to prevent ABA problem: lower 48 bits = pointer, upper 16 bits = tag
    free_head_.store(encode_tagged_ptr(memory_block_, 0), std::memory_order_release);

    // Statistics
    alloc_count_.store(0, std::memory_order_relaxed);
    free_count_.store(0, std::memory_order_relaxed);
    alloc_failures_.store(0, std::memory_order_relaxed);
  }

  ~PacketPool() {
    if (memory_block_ != nullptr) {
      std::free(memory_block_);
    }
  }

  // Non-copyable, non-movable
  PacketPool(const PacketPool&) = delete;
  PacketPool& operator=(const PacketPool&) = delete;

  /**
   * Allocate a packet buffer from pool.
   * @return Pointer to packet buffer, or nullptr if pool is exhausted
   */
  uint8_t* alloc() {
    alloc_count_.fetch_add(1, std::memory_order_relaxed);

    while (true) {
      uint64_t tagged_head = free_head_.load(std::memory_order_acquire);
      uint8_t* head = decode_ptr(tagged_head);
      uint16_t tag = decode_tag(tagged_head);

      if (head == nullptr) {
        // Pool exhausted
        alloc_failures_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }

      // Get next pointer from head
      uint8_t* next = *reinterpret_cast<uint8_t**>(head);

      // Try to update head atomically (with incremented tag)
      uint64_t new_tagged_head = encode_tagged_ptr(next, tag + 1);
      if (free_head_.compare_exchange_weak(tagged_head, new_tagged_head,
                                           std::memory_order_release,
                                           std::memory_order_acquire)) {
        // Success - return the packet
        return head;
      }

      // CAS failed, retry
    }
  }

  /**
   * Free a packet buffer back to pool.
   * @param packet Pointer to packet buffer to free
   */
  void free(uint8_t* packet) {
    if (packet == nullptr) return;

    free_count_.fetch_add(1, std::memory_order_relaxed);

    // Verify packet is from our pool (optional debug check)
    #ifndef NDEBUG
    if (packet < memory_block_ ||
        packet >= memory_block_ + pool_size_ * packet_size_) {
      // Packet not from this pool - this is a bug
      return;
    }
    #endif

    while (true) {
      uint64_t tagged_head = free_head_.load(std::memory_order_acquire);
      uint8_t* head = decode_ptr(tagged_head);
      uint16_t tag = decode_tag(tagged_head);

      // Point this packet to current head
      *reinterpret_cast<uint8_t**>(packet) = head;

      // Try to make this packet the new head (with incremented tag)
      uint64_t new_tagged_head = encode_tagged_ptr(packet, tag + 1);
      if (free_head_.compare_exchange_weak(tagged_head, new_tagged_head,
                                           std::memory_order_release,
                                           std::memory_order_acquire)) {
        // Success
        return;
      }

      // CAS failed, retry
    }
  }

  /**
   * Get pool statistics.
   */
  struct Stats {
    size_t alloc_count;
    size_t free_count;
    size_t alloc_failures;
    size_t in_use;  // alloc_count - free_count
  };

  Stats get_stats() const {
    Stats stats;
    stats.alloc_count = alloc_count_.load(std::memory_order_relaxed);
    stats.free_count = free_count_.load(std::memory_order_relaxed);
    stats.alloc_failures = alloc_failures_.load(std::memory_order_relaxed);
    stats.in_use = stats.alloc_count - stats.free_count;
    return stats;
  }

  /**
   * Get pool capacity.
   */
  size_t capacity() const { return pool_size_; }

  /**
   * Get packet size.
   */
  size_t packet_size() const { return packet_size_; }

 private:
  // Tagged pointer encoding: lower 48 bits = pointer, upper 16 bits = tag
  static uint64_t encode_tagged_ptr(uint8_t* ptr, uint16_t tag) {
    uint64_t ptr_val = reinterpret_cast<uint64_t>(ptr);
    return (static_cast<uint64_t>(tag) << 48) | (ptr_val & 0xFFFFFFFFFFFF);
  }

  static uint8_t* decode_ptr(uint64_t tagged) {
    return reinterpret_cast<uint8_t*>(tagged & 0xFFFFFFFFFFFF);
  }

  static uint16_t decode_tag(uint64_t tagged) {
    return static_cast<uint16_t>(tagged >> 48);
  }

  size_t packet_size_;
  size_t pool_size_;
  uint8_t* memory_block_;

  // Lock-free free list head (tagged pointer)
  alignas(64) std::atomic<uint64_t> free_head_;

  // Statistics
  alignas(64) std::atomic<size_t> alloc_count_;
  alignas(64) std::atomic<size_t> free_count_;
  alignas(64) std::atomic<size_t> alloc_failures_;
};

}  // namespace erpc
