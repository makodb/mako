#pragma once
#include <atomic>
#include <string>
#include "string_base.hh"
#include <new>

static_assert(std::atomic_ref<char>::is_always_lock_free,
              "MassTrans string payload bytes must be lock-free atomics");
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "MassTrans string payload lengths must be lock-free atomics");
static_assert(std::atomic_ref<char*>::is_always_lock_free,
              "MassTrans string payload pointers must be lock-free atomics");
static_assert(alignof(uint32_t) >=
                  std::atomic_ref<uint32_t>::required_alignment,
              "MassTrans string lengths must satisfy atomic_ref alignment");
static_assert(alignof(char*) >=
                  std::atomic_ref<char*>::required_alignment,
              "MassTrans string payload pointers must satisfy atomic_ref alignment");

template <typename Stuff> 
// Stuff -> uint64_t
// versioned_value
class stuffed_str {
public:
  typedef Stuff stuff_type;

#if defined(MAKO_LOCAL_TEST_HOOKS)
  using test_copy_midpoint_hook = void (*)(void*) noexcept;

  static void test_set_copy_midpoint_hook(test_copy_midpoint_hook hook,
                                          void* context) noexcept {
    test_copy_midpoint_context_ = context;
    test_copy_midpoint_hook_ = hook;
  }

  static void test_clear_copy_midpoint_hook() noexcept {
    test_copy_midpoint_hook_ = nullptr;
    test_copy_midpoint_context_ = nullptr;
  }
#endif

  struct StandardMalloc {
    void *operator()(size_t s) {
      void* p = malloc(s);  // deallocate_rcu in versioned_str_struct
      if (!p)
        throw std::bad_alloc();
      return p;
    }
  };

  template <typename Malloc = StandardMalloc>
  static stuffed_str* make(const char *str, int len, int capacity, const Stuff& val, Malloc m = Malloc()) {
    // TODO: it might be better if we just take the max of size_for() and capacity
    assert(size_for(len) <= capacity);
    //    printf("%d from %lu\n", alloc_size, len + sizeof(stuffed_str));
    auto vs = (stuffed_str*)m(capacity);
    new (vs) stuffed_str(val, len, capacity - sizeof(stuffed_str), str);
    return vs;
  }

  template <typename Malloc = StandardMalloc>
  static stuffed_str* make(const std::string& s, const Stuff& val, Malloc m = Malloc()) {
    return make(s.data(), s.length(), size_for(s.length()), val, m);
  }

  template <typename Str, typename Malloc = StandardMalloc>
  static stuffed_str* make(const lcdf::String_base<Str>& s, const Stuff& val, Malloc m = Malloc()) {
    return make(s.data(), s.length(), size_for(s.length()), val, m);
  }

  static unsigned pad(unsigned v)
  {
    if (likely(v <= 512)) {
      return (v + 15) & ~15;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if UINT_MAX == UINT64_MAX
    v |= v >> 32;
#endif
    v++;
    return v;
  }

  static inline int size_for(int len) {
    return pad(len + sizeof(stuffed_str));
  }

  bool needs_resize(int len) {
    if (TThread::is_multiversion()){
      return false; // for multiversion, it's not necessary to resize anyway
    }
    return len > (int)capacity_;
  }

  template <typename Malloc = StandardMalloc>
  stuffed_str* reserve(int len, Malloc m = Malloc()) {
    if (likely(!needs_resize(len))) {
      return this;
    }
    std::string snapshot;
    copy_payload_atomic(snapshot);
    return stuffed_str::make(snapshot.data(), static_cast<int>(snapshot.size()), len,
                             load_stuff(), m);
  }

  // returns NULL if replacement could happen without a new malloc, otherwise returns new stuffed_str*
  // malloc should be a functor that takes a size and returns a buffer of that size
  template <typename Malloc = StandardMalloc>
  stuffed_str* replace(const char *str, int len, Malloc m = Malloc()) {
    if (likely(!needs_resize(len))) {
      // Published single-version values are read optimistically. Atomic byte
      // accesses make a concurrent copy legal. The release fence follows the
      // record-lock transition and precedes every payload store; paired with
      // copy_payload_atomic's acquire fence, observing any new byte forces the
      // reader's final version load to observe the lock or newer version.
      std::atomic_thread_fence(std::memory_order_release);
      for (int i = 0; i != len; ++i)
        std::atomic_ref<char>(buf_[i]).store(str[i],
                                             std::memory_order_relaxed);
      store_size(len, std::memory_order_release);
      return this;
    }
    //std::cerr << "this should never happen, since we do it resizeIfNeeded func" << std::endl;
    return stuffed_str::make(str, len, size_for(len), load_stuff(), m);
  }

  void modifyData(char* p){
    store_data(p, std::memory_order_release);
  }

  char *data() {
    return load_data(std::memory_order_acquire);
  }
  
  int length() const {
    return static_cast<int>(load_size(std::memory_order_acquire));
  }

  void set_length(int ss) {
    store_size(ss, std::memory_order_release);
  }

  void copy_payload_atomic(std::string& out) const {
    while (!copy_payload_atomic(out, load_stuff())) {
    }
  }

  bool copy_payload_atomic(std::string& out,
                           const Stuff& expected_version) const {
    // Multiversion installs publish a separately allocated newest value by
    // redirecting flex_buf_. Snapshot that published head rather than always
    // copying the original inline buffer, which is retained as history.
    char* const payload = load_data(std::memory_order_acquire);
    const uint32_t length = load_size(std::memory_order_acquire);
    // Pointer and length are separate atomic words. A writer may change one
    // between these loads, so validate the tuple against atomicRead's initial
    // unlocked version before using it. The caller retains its post-copy
    // version check for writers that begin after this point.
    if (load_stuff() != expected_version)
      return false;
    if (payload == buf_)
      assert(length <= capacity_);
    out.resize(length);
#if defined(MAKO_LOCAL_TEST_HOOKS)
    const uint32_t midpoint = length / 2;
    for (uint32_t i = 0; i != midpoint; ++i)
      out[i] = std::atomic_ref<char>(payload[i])
                   .load(std::memory_order_relaxed);
    // Consume the thread-local hook before invoking it so a version retry
    // cannot park twice. This seam exists only in hook-enabled boundary tests.
    const test_copy_midpoint_hook hook = test_copy_midpoint_hook_;
    void* const context = test_copy_midpoint_context_;
    test_clear_copy_midpoint_hook();
    if (hook != nullptr)
      hook(context);
    for (uint32_t i = midpoint; i != length; ++i)
      out[i] = std::atomic_ref<char>(payload[i])
                   .load(std::memory_order_relaxed);
#else
    for (uint32_t i = 0; i != length; ++i)
      out[i] = std::atomic_ref<char>(payload[i])
                   .load(std::memory_order_relaxed);
#endif
    // If any size/payload load read a store sequenced after replace's release
    // fence, synchronize before MassTrans performs its final version load.
    std::atomic_thread_fence(std::memory_order_acquire);
    return true;
  }
  
  int capacity() {
    return capacity_;
  }

  Stuff& stuff() {
    return stuff_;
  }

  Stuff stuff() const {
    return load_stuff();
  }

private:
#if defined(MAKO_LOCAL_TEST_HOOKS)
  inline static thread_local test_copy_midpoint_hook
      test_copy_midpoint_hook_ = nullptr;
  inline static thread_local void* test_copy_midpoint_context_ = nullptr;
#endif

  uint32_t load_size(std::memory_order order) const {
    return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(size_)).load(order);
  }

  void store_size(uint32_t size, std::memory_order order) {
    std::atomic_ref<uint32_t>(size_).store(size, order);
  }

  char* load_data(std::memory_order order) const {
    return std::atomic_ref<char*>(const_cast<char*&>(flex_buf_)).load(order);
  }

  void store_data(char* data, std::memory_order order) {
    std::atomic_ref<char*>(flex_buf_).store(data, order);
  }

  Stuff load_stuff() const {
    return std::atomic_ref<Stuff>(const_cast<Stuff&>(stuff_))
        .load(std::memory_order_acquire);
  }

  stuffed_str(const Stuff& stuff, uint32_t size, uint32_t capacity, const char *buf) :
    stuff_(stuff), size_(size), capacity_(capacity) {
    memcpy(buf_, buf, size);
    flex_buf_ = buf_; // initialize the dynamic pointer, initialize once
  }

  Stuff stuff_;
  uint32_t size_;
  uint32_t capacity_;
  char *flex_buf_;
  char buf_[0]; // zero-length arrays in GNU C
};
