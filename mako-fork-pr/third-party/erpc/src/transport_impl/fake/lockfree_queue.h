/**
 * @file lockfree_queue.h
 * @brief Lock-free SPMC (Single Producer Multiple Consumer) queue for packet buffering
 *
 * This implementation uses atomic operations to enable:
 * - One producer thread (RX thread) pushing packets
 * - Multiple consumer threads (worker threads) popping packets
 * - No mutex contention between producer and consumers
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace erpc {

/**
 * Lock-free bounded SPMC queue with fixed capacity.
 *
 * Design:
 * - Ring buffer with power-of-2 size for efficient modulo
 * - Atomic head/tail indices for lock-free operation
 * - Producer increments tail, consumers increment head
 * - Memory barriers ensure proper ordering
 */
template<typename T, size_t Capacity>
class LockFreeSPMCQueue {
 public:
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

  LockFreeSPMCQueue() : head_(0), tail_(0) {
    // Initialize all slots to empty
    for (size_t i = 0; i < Capacity; i++) {
      buffer_[i].store(T{}, std::memory_order_relaxed);
    }
  }

  ~LockFreeSPMCQueue() = default;

  // Non-copyable, non-movable
  LockFreeSPMCQueue(const LockFreeSPMCQueue&) = delete;
  LockFreeSPMCQueue& operator=(const LockFreeSPMCQueue&) = delete;

  /**
   * Try to push an item (producer only - single thread).
   * @return true if successful, false if queue is full
   */
  bool try_push(const T& item) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);

    // Check if queue is full
    if (tail - head >= Capacity) {
      return false;
    }

    // Store item
    size_t index = tail & (Capacity - 1);
    buffer_[index].store(item, std::memory_order_release);

    // Advance tail
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /**
   * Try to pop an item (consumer - multiple threads).
   * @return pair of (success, item)
   */
  std::pair<bool, T> try_pop() {
    while (true) {
      size_t head = head_.load(std::memory_order_relaxed);
      size_t tail = tail_.load(std::memory_order_acquire);

      // Check if queue is empty
      if (head >= tail) {
        return {false, T{}};
      }

      // Try to claim this slot atomically
      if (!head_.compare_exchange_weak(head, head + 1,
                                       std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
        // Another consumer claimed it, retry
        continue;
      }

      // Successfully claimed, read item
      size_t index = head & (Capacity - 1);
      T item = buffer_[index].load(std::memory_order_acquire);

      // Clear slot (optional, helps debugging)
      buffer_[index].store(T{}, std::memory_order_relaxed);

      return {true, item};
    }
  }

  /**
   * Get approximate size (may be stale due to concurrent operations).
   */
  size_t approximate_size() const {
    size_t tail = tail_.load(std::memory_order_acquire);
    size_t head = head_.load(std::memory_order_acquire);
    return tail > head ? tail - head : 0;
  }

  /**
   * Check if queue is approximately empty.
   */
  bool empty() const {
    return approximate_size() == 0;
  }

  /**
   * Check if queue is approximately full.
   */
  bool full() const {
    return approximate_size() >= Capacity;
  }

  /**
   * Get capacity.
   */
  constexpr size_t capacity() const {
    return Capacity;
  }

 private:
  // Align to cache line to prevent false sharing
  alignas(64) std::atomic<size_t> head_;
  alignas(64) std::atomic<size_t> tail_;

  // Ring buffer of atomic pointers
  alignas(64) std::atomic<T> buffer_[Capacity];
};

/**
 * Packet info structure for the queue.
 * Encoded as a single 64-bit value to allow atomic operations without libatomic:
 * - Lower 48 bits: pointer to packet data
 * - Upper 16 bits: packet size (max 65535 bytes)
 */
struct PacketInfo {
  uint64_t encoded;

  PacketInfo() : encoded(0) {}
  PacketInfo(uint8_t* data, size_t size) {
    encode(data, size);
  }

  void encode(uint8_t* data, size_t size) {
    uint64_t ptr_val = reinterpret_cast<uint64_t>(data);
    // Encode: lower 48 bits = pointer, upper 16 bits = size
    encoded = (ptr_val & 0xFFFFFFFFFFFFULL) | (static_cast<uint64_t>(size & 0xFFFF) << 48);
  }

  uint8_t* get_data() const {
    return reinterpret_cast<uint8_t*>(encoded & 0xFFFFFFFFFFFFULL);
  }

  size_t get_size() const {
    return static_cast<size_t>(encoded >> 48);
  }

  bool is_null() const {
    return encoded == 0;
  }

  // Define equality for atomic operations
  bool operator==(const PacketInfo& other) const {
    return encoded == other.encoded;
  }
};

}  // namespace erpc
