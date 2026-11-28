// Native committed rows for the STO adapter. The Masstree leaf word is the
// address of mt_row. No ID registry or second key index participates in lookup.
#include "mtree_rows.h"
#include "../spinlock.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

// Bounded transaction scratch reuse, following STO's transaction allocator.
// Only already-destroyed, unreferenced allocations enter a pool. Returning on
// a different thread is safe: that thread owns the receiving pool. The trivial
// state remains valid if another TLS destructor returns a value after cleanup.
template <size_t Bytes> struct scratch_pool {
  void *slots[64]{};
  size_t count = 0;
  bool closed = false;
  void *take() { return count ? slots[--count] : ::operator new(Bytes); }
  void put(void *p) noexcept {
    if (!closed && count != 64)
      slots[count++] = p;
    else
      ::operator delete(p);
  }
  struct cleanup {
    scratch_pool &pool;
    ~cleanup() {
      pool.closed = true;
      while (pool.count)
        ::operator delete(pool.slots[--pool.count]);
    }
  };
};
template <size_t Bytes> scratch_pool<Bytes> &scratch() {
  thread_local scratch_pool<Bytes> pool;
  thread_local typename scratch_pool<Bytes>::cleanup cleanup{pool};
  (void)cleanup;
  return pool;
}

// Packed header plus immutable bytes, adapted from STO stuffed_str::make at
// 0d26e2e0b. Heap buffers stay immutable; unshared embedded buffers use the
// version-checked atomic update/copy protocol below.
struct mt_value {
  static constexpr size_t flag = size_t(1) << (sizeof(size_t) * 8 - 1);
  // The high reference bit marks a buffer embedded in a native row. Its last
  // reference releases that row allocation instead of deleting this header.
  std::atomic<size_t> references;
  std::atomic<size_t> length_and_presence;
  mt_value(size_t n, bool live, bool embedded = false)
      : references((embedded ? flag : 0) | 1),
        length_and_presence(n | (live ? flag : 0)) {}
  size_t size() const {
    return length_and_presence.load(std::memory_order_relaxed) & ~flag;
  }
  bool present() const {
    return length_and_presence.load(std::memory_order_relaxed) & flag;
  }
  const unsigned char *data() const {
    return reinterpret_cast<const unsigned char *>(this + 1);
  }
  static mt_value *make(const void *p, size_t n, bool live) {
    if (n >= flag || n > SIZE_MAX - sizeof(mt_value))
      throw std::bad_alloc();
    auto memory = n <= 16 ? scratch<sizeof(mt_value) + 16>().take()
                          : ::operator new(sizeof(mt_value) + n);
    auto v = new (memory) mt_value(n, live);
    if (n)
      std::memcpy(v + 1, p, n);
    return v;
  }
};
static_assert(sizeof(mt_value) == 16);
// A native buffer owner. Copying retains the allocation; it never copies bytes.
struct value_ref {
  mt_value *ptr;
  explicit value_ref(mt_value *p) : ptr(p) {}
  value_ref(const value_ref &v) : ptr(mako_value_clone(v.ptr)) {}
  ~value_ref() { mako_value_drop(ptr); }
  value_ref &operator=(const value_ref &) = delete;
  const mt_value *operator->() const { return ptr; }
};
// The prepared publication becomes the previous-value history node. It has no
// separate shared_ptr control block or wrapper allocation. Only its row and the
// prepared Rust handle own it; snapshots retain the independent immutable byte
// buffer.
struct mt_write {
  std::atomic<size_t> references{1};
  mt_row *const row;
  value_ref value;
  uint32_t time_term = 0;
  mt_write *previous = nullptr;
  bool installed = false;
  mt_write(mt_row *r, mt_value *v) : row(r), value(v) {}
};
struct budget_state {
  const uint64_t maximum;
  std::atomic<uint64_t> used{0};
  explicit budget_state(uint64_t max) : maximum(max) {}
  bool charge(uint64_t n) noexcept {
    auto old = used.load();
    do {
      if (n > maximum - old)
        return false;
    } while (!used.compare_exchange_weak(old, old + n));
    return true;
  }
};
struct mt_row_budget {
  std::shared_ptr<budget_state> state;
};
struct charge_guard {
  std::shared_ptr<budget_state> budget;
  uint64_t bytes;
  charge_guard(std::shared_ptr<budget_state> b, uint64_t n)
      : budget(std::move(b)), bytes(n) {}
  ~charge_guard() {
    if (bytes)
      budget->used.fetch_sub(bytes);
  }
};
struct mt_rows {
  mt_tree *tree = nullptr;
  std::shared_ptr<budget_state> budget;
  std::atomic<uint64_t> charged;
  std::atomic<mt_row *> first{nullptr};
  std::atomic<uint64_t> rows{0}, keys{0};
  uint64_t max_rows, max_keys;
  std::atomic<uint64_t> generation{0}, live{0};
  std::atomic<uint32_t> floor{0};
  std::atomic<bool> poisoned{false};
  mt_rows(std::shared_ptr<budget_state> b, uint64_t r, uint64_t k)
      : budget(std::move(b)), charged(sizeof(mt_rows)), max_rows(r),
        max_keys(k) {}
};
struct mt_row {
  // The version, latch and small current value occupy one cache line. This
  // adapts original STO's in-place packed string update, but only while no
  // external immutable snapshot references the embedded buffer. Larger/shared
  // values use the same immutable heap buffer representation as before.
  std::atomic<uint64_t> state{UINT64_C(1) << 16};
  spinlock mutex;
  uint32_t time_term = 0;
  std::atomic<mt_value *> current;
  mt_value embedded{0, false, true};
  std::atomic<unsigned char> inline_bytes[16]{};
  uint32_t pruned_through = 0;
  bool installed = false;
  // Protected by the row latch. Avoid touching the cold history pointer for
  // ordinary single-version updates. Every history mutation updates this bit.
  bool has_history = false;
  mt_write *history = nullptr;
  mt_row *next = nullptr;
  // One table owner plus one lease while embedded.references is nonzero.
  // Snapshots may keep only this allocation alive after the table is gone.
  std::atomic<size_t> allocation_refs{2};
  const uint16_t key_size;
  explicit mt_row(uint16_t n) : current(&embedded), key_size(n) {}
  const unsigned char *key() const {
    return reinterpret_cast<const unsigned char *>(this + 1);
  }
  static mt_row *make(const void *key, size_t size) {
    if (size > UINT16_MAX)
      throw std::bad_alloc();
    auto destroy = [](void *p) { ::operator delete(p, std::align_val_t(64)); };
    auto memory = std::unique_ptr<void, decltype(destroy)>(
        ::operator new(sizeof(mt_row) + size, std::align_val_t(64)), destroy);
    auto row = new (memory.get()) mt_row(static_cast<uint16_t>(size));
    if (size)
      std::memcpy(static_cast<void *>(row + 1), key, size);
    memory.release();
    return row;
  }
  void release_allocation() {
    if (allocation_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      this->~mt_row();
      ::operator delete(this, std::align_val_t(64));
    }
  }
  void retire() {
    // The table has no remaining row handles. History is already discarded.
    mako_value_drop(current.exchange(nullptr, std::memory_order_relaxed));
    release_allocation();
  }
};
static_assert(offsetof(mt_row, embedded) == 24);
static_assert(offsetof(mt_row, inline_bytes) == 40);
static_assert(offsetof(mt_row, has_history) < 64);
static_assert(offsetof(mt_row, history) == 64);
static_assert(sizeof(mt_row) == 96);
static_assert(sizeof(std::atomic<unsigned char>) == 1 &&
              std::atomic<unsigned char>::is_always_lock_free);

namespace {
template <class F> mt_status boundary(F &&f) noexcept {
  try {
    return f();
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MT_ERR_INTERNAL;
  }
}
void view(const mt_value &v, uint64_t version, mt_row_view &out) noexcept {
  out = {v.data(), v.size(), version, 0, static_cast<uint32_t>(v.present())};
}
// Iterative unlink avoids recursive destruction of a long version history.
// The row latch protects linked history; detached suffixes have one owner.
// Snapshots retain bytes, not links.
void discard(mt_write *node) noexcept {
  while (node &&
         node->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    auto next = node->previous;
    node->~mt_write();
    scratch<sizeof(mt_write)>().put(node);
    node = next;
  }
}
} // namespace
extern "C" mt_status mako_row_budget_create(uint64_t maximum,
                                            mt_row_budget **out) noexcept {
  *out = nullptr;
  return boundary([&]() -> mt_status {
    *out = new mt_row_budget{std::make_shared<budget_state>(maximum)};
    return MT_OK;
  });
}
extern "C" void mako_row_budget_drop(mt_row_budget *b) noexcept { delete b; }
extern "C" uint64_t mako_row_budget_used(const mt_row_budget *b) noexcept {
  return b->state->used.load();
}
extern "C" mt_status mako_rows_create(mt_row_budget *budget, uint64_t r,
                                      uint64_t k, mt_rows **out) noexcept {
  *out = nullptr;
  return boundary([&]() -> mt_status {
    if (!budget->state->charge(sizeof(mt_rows)))
      return MT_ERR_OUT_OF_MEMORY;
    charge_guard charge(budget->state, sizeof(mt_rows));
    *out = new mt_rows(budget->state, r, k);
    charge.bytes = 0;
    return MT_OK;
  });
}
extern "C" void mako_rows_bind(mt_rows *t, mt_tree *tree) noexcept {
  t->tree = tree;
}
extern "C" void mako_rows_destroy(mt_rows *t) noexcept {
  // Rust keeps this owner alive in every row handle and registered STO binding.
  // Close the directory first: its process-lived leaf words then cannot be
  // read.
  if (t->tree)
    mt_tree_release(t->tree);
  auto r = t->first.load();
  while (r) {
    auto next = r->next;
    discard(r->history);
    r->retire();
    r = next;
  }
  t->budget->used.fetch_sub(t->charged.load());
  delete t;
}
namespace {
bool reserve(std::atomic<uint64_t> &count, uint64_t n, uint64_t maximum) {
  auto old = count.load(std::memory_order_relaxed);
  do {
    if (n > maximum - old)
      return false;
  } while (
      !count.compare_exchange_weak(old, old + n, std::memory_order_relaxed));
  return true;
}
struct row_candidate {
  mt_rows *table;
  const void *key;
  size_t size;
  mt_row *allocated = nullptr;
  static mt_status make(void *context, uint64_t *out) {
    auto &candidate = *static_cast<row_candidate *>(context);
    auto t = candidate.table;
    const size_t n = candidate.size;
    return boundary([&]() -> mt_status {
      if (!reserve(t->rows, 1, t->max_rows))
        return MT_ERR_OUT_OF_MEMORY;
      if (!reserve(t->keys, n, t->max_keys)) {
        t->rows.fetch_sub(1);
        return MT_ERR_OUT_OF_MEMORY;
      }
      struct undo_quota {
        mt_rows *table;
        size_t keys;
        bool committed = false;
        ~undo_quota() {
          if (!committed) {
            table->rows.fetch_sub(1);
            table->keys.fetch_sub(keys);
          }
        }
      } quota{t, n};
      const uint64_t bytes = sizeof(mt_row) + n;
      if (!t->budget->charge(bytes))
        return MT_ERR_OUT_OF_MEMORY;
      charge_guard charge(t->budget, bytes);
      auto destroy = [](mt_row *r) { r->retire(); };
      auto row = std::unique_ptr<mt_row, decltype(destroy)>(
          mt_row::make(candidate.key, n), destroy);
      row->next = t->first.load(std::memory_order_relaxed);
      while (!t->first.compare_exchange_weak(row->next, row.get(),
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
      }
      candidate.allocated = row.release();
      quota.committed = true;
      charge.bytes = 0;
      t->charged.fetch_add(bytes, std::memory_order_relaxed);
      *out = reinterpret_cast<uint64_t>(candidate.allocated);
      return MT_OK;
    });
  }
};
} // namespace
extern "C" mt_status mako_rows_find(mt_rows *t, mt_thread *worker,
                                    const void *key, size_t n, mt_row **out,
                                    mt_leaf_stamp *stamp) noexcept {
  *out = nullptr;
  if (t->poisoned.load())
    return MT_ERR_POISONED;
  uint64_t address = 0;
  auto result = mako_row_index_find(t->tree, worker, key, n, &address, stamp);
  if (result == MT_OK) {
    *out = reinterpret_cast<mt_row *>(address);
    // Original MassTrans prefetches its leaf value before observing the row.
    // Only this private pointer-valued lane may do so; generic integer IDs
    // retain their no-prefetch contract. The table owns every returned row.
    if (address)
      __builtin_prefetch(*out, 0, 3);
  }
  return result;
}
extern "C" mt_status mako_rows_resolve(mt_rows *t, mt_thread *worker,
                                       const void *key, size_t n, mt_row **out,
                                       mt_row_insertion *change) noexcept {
  *change = {};
  mt_leaf_stamp observed{};
  auto result = mako_rows_find(t, worker, key, n, out, &observed);
  if (result != MT_OK || *out)
    return result;
  row_candidate candidate{t, key, n};
  uint64_t address = 0;
  result = mako_row_index_insert(t->tree, worker, key, n, &candidate,
                                 row_candidate::make, &address, change);
  if (candidate.allocated) {
    if (result != MT_OK) {
      // Keep any possibly published referent alive. No abort may undo an
      // unknown cursor publication; table destruction owns final reclamation.
      t->poisoned.store(true);
      return MT_ERR_POISONED;
    }
    t->generation.fetch_add(1);
  }
  if (result == MT_OK) {
    *out = reinterpret_cast<mt_row *>(address);
    // Original MassTrans prefetches its leaf value before observing the row.
    // Only this private pointer-valued lane may do so; generic integer IDs
    // retain their no-prefetch contract. The table owns every returned row.
    if (address)
      __builtin_prefetch(*out, 0, 3);
  }
  return result;
}
extern "C" uint64_t mako_rows_generation(const mt_rows *t) noexcept {
  return t->generation.load();
}
extern "C" uint64_t mako_rows_count(const mt_rows *t) noexcept {
  return t->live.load();
}
extern "C" void mako_rows_floor(mt_rows *t, uint32_t floor) noexcept {
  auto old = t->floor.load();
  while (old < floor && !t->floor.compare_exchange_weak(old, floor)) {
  }
}
extern "C" mt_status mako_row_read(mt_rows *t, mt_row *r, uint32_t owner,
                                   const mt_visibility *v, uint32_t checkpoint,
                                   mt_value **out,
                                   mt_row_view *result) noexcept {
  *out = nullptr;
  *result = {};
  return boundary([&]() -> mt_status {
    if (t->poisoned.load())
      return MT_ERR_POISONED;
    std::lock_guard guard(r->mutex);
    const auto state = r->state.load(std::memory_order_acquire);
    const auto held_by = state & UINT16_MAX;
    if (!checkpoint && held_by && held_by != owner)
      return MT_ERR_BUSY;
    auto value = r->current.load(std::memory_order_relaxed);
    auto time = r->time_term;
    if (v) {
      auto previous = r->history;
      while (time && time % 10 != v->term &&
             v->stable[time % 10] != UINT32_MAX &&
             time / 10 > v->stable[time % 10]) {
        if (!previous)
          return MT_ERR_BUSY;
        value = previous->value.ptr;
        time = previous->time_term;
        previous = previous->previous;
      }
      if (v->failed != UINT32_MAX && time / 10 > v->failed)
        return MT_ERR_BUSY;
    }
    *out = mako_value_clone(value);
    view(**out, state >> 16, *result);
    result->time_term = time;
    return MT_OK;
  });
}
extern "C" mt_status mako_row_copy(mt_rows *t, mt_row *r, unsigned char *data,
                                   size_t capacity,
                                   mt_row_view *result) noexcept {
  *result = {};
  if (t->poisoned.load())
    return MT_ERR_POISONED;
  const auto observed = r->state.load(std::memory_order_acquire);
  if (observed & UINT16_MAX)
    return MT_ERR_BUSY;
  if (r->current.load(std::memory_order_acquire) == &r->embedded) {
    // Adapt stuffed_str::copy_payload_atomic from 0d26e2e0b. These bytes
    // belong to the retained row allocation and cannot be freed mid-copy.
    // Heap pointers still use the pinning latch below. Atomic byte loads
    // permit concurrent in-place updates without a C++ data race.
    const auto length =
        r->embedded.length_and_presence.load(std::memory_order_relaxed);
    const auto size = length & ~mt_value::flag;
    if (size > sizeof(r->inline_bytes))
      return MT_ERR_INTERNAL;
    if (size <= capacity) {
      for (size_t i = 0; i != size; ++i)
        data[i] = r->inline_bytes[i].load(std::memory_order_relaxed);
    }
    // A byte/length load that observes any new store synchronizes with the
    // writer's release fence, so this final state load cannot accept the old
    // unlocked generation after copying part of a newer value.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (r->state.load(std::memory_order_relaxed) != observed)
      return MT_ERR_BUSY;
    result->size = size;
    result->present = (length & mt_value::flag) != 0;
    result->version = observed >> 16;
    return size > capacity ? MT_ERR_BUFFER_TOO_SMALL : MT_OK;
  }
  std::lock_guard guard(r->mutex);
  const auto state = r->state.load(std::memory_order_acquire);
  if (state & UINT16_MAX)
    return MT_ERR_BUSY;
  const auto value = r->current.load(std::memory_order_relaxed);
  view(*value, state >> 16, *result);
  result->data = nullptr;
  if (result->size > capacity)
    return MT_ERR_BUFFER_TOO_SMALL;
  if (result->size)
    std::memcpy(data, value->data(), result->size);
  return MT_OK;
}
extern "C" mt_status mako_value_new(const void *bytes, size_t n,
                                    uint32_t present, mt_value **out,
                                    mt_row_view *result) noexcept {
  *out = nullptr;
  return boundary([&]() -> mt_status {
    if ((!bytes && n) || present > 1 || (!present && n))
      return MT_ERR_INVALID;
    *out = mt_value::make(bytes, n, present);
    view(**out, 0, *result);
    return MT_OK;
  });
}
extern "C" mt_value *mako_value_clone(const mt_value *v) noexcept {
  auto mutable_v = const_cast<mt_value *>(v);
  if ((mutable_v->references.fetch_add(1, std::memory_order_relaxed) &
       ~mt_value::flag) > mt_value::flag / 2)
    std::terminate();
  return mutable_v;
}
extern "C" void mako_value_drop(mt_value *v) noexcept {
  if (!v)
    return;
  const auto prior = v->references.fetch_sub(1, std::memory_order_acq_rel);
  if ((prior & ~mt_value::flag) != 1)
    return;
  if (prior & mt_value::flag) {
    auto row = reinterpret_cast<mt_row *>(reinterpret_cast<unsigned char *>(v) -
                                          offsetof(mt_row, embedded));
    row->release_allocation();
  } else {
    const bool small = v->size() <= 16;
    v->~mt_value();
    if (small)
      scratch<sizeof(mt_value) + 16>().put(v);
    else
      ::operator delete(v);
  }
}
extern "C" mt_status mako_row_check(mt_rows *t, mt_row *r, uint64_t version,
                                    uint32_t owner) noexcept {
  if (t->poisoned.load())
    return MT_ERR_POISONED;
  const auto state = r->state.load(std::memory_order_acquire);
  const auto held_by = state & UINT16_MAX;
  return (state >> 16) == version && (!held_by || held_by == owner)
             ? MT_OK
             : MT_ERR_BUSY;
}
extern "C" mt_status mako_row_lock(mt_rows *t, mt_row *r, uint32_t owner) noexcept {
  if (t->poisoned.load())
    return MT_ERR_POISONED;
  if (!owner || owner > UINT16_MAX)
    return MT_ERR_INVALID;
  auto state = r->state.load(std::memory_order_acquire);
  if (state & UINT16_MAX)
    return MT_ERR_BUSY;
  return r->state.compare_exchange_strong(state, state | owner,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)
             ? MT_OK
             : MT_ERR_BUSY;
}
extern "C" mt_status mako_write_prepare(mt_rows *t, mt_row *r, uint32_t owner,
                                        const mt_value *v,
                                        mt_write **out) noexcept {
  *out = nullptr;
  return boundary([&]() -> mt_status {
    if (t->poisoned.load())
      return MT_ERR_POISONED;
    if (owner) {
      std::lock_guard guard(r->mutex);
      if ((r->state.load() & UINT16_MAX) != owner || r->installed)
        return MT_ERR_INVALID;
    }
    // Owner zero only allocates private scratch. Installation still requires
    // the exact acquired row guard and a newer validated version.
    // Allocate the node before retaining its payload so allocation failure
    // cannot leak a native reference.
    auto storage = scratch<sizeof(mt_write)>().take();
    *out = new (storage) mt_write(r, mako_value_clone(v));
    return MT_OK;
  });
}
extern "C" void mako_write_drop(mt_write *w) noexcept { discard(w); }
extern "C" mt_status mako_row_install(mt_rows *t, mt_row *r, uint32_t owner, mt_write *w,
                                      uint64_t version,
                                      uint32_t time) noexcept {
  std::lock_guard guard(r->mutex);
  const auto state = r->state.load(std::memory_order_acquire);
  if (t->poisoned.load() || (state & UINT16_MAX) != owner || !owner ||
      w->row != r || w->installed || r->installed || version <= (state >> 16) ||
      version > (UINT64_MAX >> 16)) {
    t->poisoned.store(true);
    return MT_ERR_POISONED;
  }
  const auto current = r->current.load(std::memory_order_relaxed);
  const auto was_live = current->present();
  const auto embedded_refs =
      r->embedded.references.load(std::memory_order_acquire);
  const auto prepared_length =
      w->value->length_and_presence.load(std::memory_order_relaxed);
  const auto prepared_size = prepared_length & ~mt_value::flag;
  const auto prepared_data = w->value->data();
  const bool reuse_current =
      current == &r->embedded && embedded_refs == (mt_value::flag | 1);
  const bool revive_unused = embedded_refs == mt_value::flag;
  if (!time && !r->time_term && prepared_size <= sizeof(r->inline_bytes) &&
      (reuse_current || revive_unused)) {
    if (revive_unused) {
      r->allocation_refs.fetch_add(1, std::memory_order_relaxed);
      r->embedded.references.store(mt_value::flag | 1,
                                   std::memory_order_relaxed);
    }
    // The row latch excludes new snapshot pins, and the reference count proves
    // no older snapshot can observe these bytes while they are overwritten.
    // Publish the lock acquisition before relaxed stores through the same
    // release-fence/atomic-byte/acquire-fence protocol as original STO.
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i != prepared_size; ++i)
      r->inline_bytes[i].store(prepared_data[i], std::memory_order_relaxed);
    r->embedded.length_and_presence.store(
        prepared_length, std::memory_order_relaxed);
    if (!reuse_current)
      mako_value_drop(
          r->current.exchange(&r->embedded, std::memory_order_release));
    if (r->has_history) {
      discard(std::exchange(r->history, nullptr));
      r->has_history = false;
    }
  } else {
    w->value.ptr = r->current.exchange(w->value.ptr, std::memory_order_release);
    w->time_term = r->time_term;
    if (time || r->time_term) {
      w->previous = r->history;
      w->references.fetch_add(1, std::memory_order_relaxed);
      r->history = w;
      r->has_history = true;
    } else if (r->has_history) {
      discard(std::exchange(r->history, nullptr));
      r->has_history = false;
    }
  }
  r->time_term = time;
  r->state.store((version << 16) | owner, std::memory_order_release);
  r->installed = true;
  w->installed = true;
  if (was_live != r->current.load(std::memory_order_relaxed)->present()) {
    if (was_live)
      t->live.fetch_sub(1);
    else
      t->live.fetch_add(1);
  }
  const auto floor = t->floor.load();
  // Retain exactly the history prefix authorized by the visibility floor.
  if (r->has_history) {
    if (floor && r->history->time_term / 10 < floor) {
      discard(std::exchange(r->history, nullptr));
    } else if (floor > r->pruned_through) {
      auto *link = &r->history;
      while (*link) {
        if ((*link)->time_term / 10 < floor) {
          discard(std::exchange(*link, nullptr));
          break;
        }
        link = &(*link)->previous;
      }
    }
    r->has_history = r->history != nullptr;
  }
  r->pruned_through = std::max(r->pruned_through, floor);
  return MT_OK;
}
extern "C" mt_status mako_row_unlock(mt_rows *t, mt_row *r, uint32_t owner,
                                     uint32_t disposition) noexcept {
  // The private ABI requires the exclusive live guard/ticket. Only that
  // owner accesses installed while the row lock is held. Checkpoint readers
  // never touch it, and the release store orders it before the next owner's
  // successful acquire. No buffer/history mutation needs the pinning latch.

  const auto state = r->state.load(std::memory_order_acquire);
  if (!owner || (state & UINT16_MAX) != owner || disposition > 2 ||
      disposition == 2 || (disposition == 0 && r->installed) ||
      (disposition == 1 && !r->installed)) {
    t->poisoned.store(true);
    return MT_ERR_POISONED;
  }
  r->installed = false;
  // This exact owner is the only writer of the state while its lock is held.
  r->state.store(state & ~uint64_t(UINT16_MAX), std::memory_order_release);
  return MT_OK;
}

extern "C" void mako_row_key(const mt_row *r, const unsigned char **p,
                             size_t *n) noexcept {
  *p = r->key();
  *n = r->key_size;
}

extern "C" uint64_t mako_rows_abi(void) noexcept {
  return (UINT64_C(4) << 48) | (uint64_t(sizeof(mt_row_view)) << 32) |
         (uint64_t(alignof(mt_row_view)) << 24) |
         (uint64_t(sizeof(mt_visibility)) << 8) |
         uint64_t(alignof(mt_visibility));
}
