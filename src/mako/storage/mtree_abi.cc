// mtree_abi.cc - hardened C boundary for an integral-valued Masstree.

#define MAKO_MTREE_ABI_TRUSTED_RUST_BRIDGE 1
#include "mako/storage/mtree_abi.h"

#include "mako/core.h"
#include "mako/masstree_btree.h"
#include "mako/rcu.h"
#include "mako/silo_runtime.h"
#include "mako/varkey.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace mt_abi_detail {

/* Masstree's generic value printer expects pointer-valued records. */
struct record_value_print {
  static void print(mt_record_id value, FILE *file, const char *prefix,
                    int indent, lcdf::Str key, kvtimestamp_t, char *suffix) {
    file = file == nullptr ? stderr : file;
    prefix = prefix == nullptr ? "" : prefix;
    const char *safe_suffix = suffix == nullptr ? "" : suffix;
    std::fprintf(file, "%s%*s%.*s = %llu%s\n", prefix, indent, "", key.len,
                 key.s, static_cast<unsigned long long>(value), safe_suffix);
  }
};

/*
 * This dedicated parameter set is the reason no RecordId is ever reinterpreted
 * as a C++ pointer. Disabling value prefetch also prevents Masstree from using
 * an integral RecordId as a speculative address.
 */
struct record_params : public Masstree::nodeparams<> {
  using value_type = mt_record_id;
  using value_print_type = record_value_print;
  using threadinfo_type = simple_threadinfo;
  static constexpr bool prefetch = false;
  static constexpr bool RcuRespCaller = true;
};

using record_tree = mbtree<record_params>;

static_assert(!record_params::prefetch);
static_assert(record_params::RcuRespCaller);
static_assert(std::is_same_v<record_tree::value_type, uint64_t>);
static_assert(std::is_standard_layout_v<mt_scan_bound>);
static_assert(std::is_standard_layout_v<mt_scan_entry>);
static_assert(std::is_standard_layout_v<mt_scan_result>);
static_assert(std::is_standard_layout_v<mt_read_scope>);
static_assert(std::is_same_v<mt_rcu_scope, mt_read_scope>);
static_assert(MASSTREE_MAXKEYLEN >= MT_CONFIGURED_MAX_KEY_LENGTH,
              "the native Masstree key limit is below the ABI contract");
static_assert(NMAXCORES <= std::numeric_limits<uint32_t>::max());

using structure_reader_mask_word = uint64_t;
static constexpr size_t kStructureReaderMaskWordBits =
    std::numeric_limits<structure_reader_mask_word>::digits;
static constexpr size_t kStructureReaderMaskWords =
    (NMAXCORES + kStructureReaderMaskWordBits - 1) /
    kStructureReaderMaskWordBits;

} // namespace mt_abi_detail

struct alignas(CACHELINE_SIZE) mt_structure_reader_slot {
  std::atomic<mt_tree *> active_tree{nullptr};
};

struct alignas(CACHELINE_SIZE) mt_structure_sealed_flag {
  std::atomic<bool> sealed{false};
};

struct alignas(CACHELINE_SIZE) mt_structure_writer_flag {
  std::atomic<bool> active{false};
};

struct alignas(CACHELINE_SIZE) mt_structure_writer_lock {
  std::mutex mutex;
};

struct alignas(CACHELINE_SIZE) mt_structure_reader_membership {
  std::array<std::atomic<mt_abi_detail::structure_reader_mask_word>,
             mt_abi_detail::kStructureReaderMaskWords>
      words{};
};

static_assert(sizeof(mt_structure_reader_slot) == CACHELINE_SIZE);
static_assert(alignof(mt_structure_reader_slot) == CACHELINE_SIZE);
static_assert(sizeof(mt_structure_sealed_flag) == CACHELINE_SIZE);
static_assert(alignof(mt_structure_sealed_flag) == CACHELINE_SIZE);
static_assert(sizeof(mt_structure_writer_flag) == CACHELINE_SIZE);
static_assert(alignof(mt_structure_writer_flag) == CACHELINE_SIZE);
static_assert(sizeof(mt_structure_writer_lock) % CACHELINE_SIZE == 0);
static_assert(alignof(mt_structure_writer_lock) == CACHELINE_SIZE);
static_assert(sizeof(mt_structure_reader_membership) % CACHELINE_SIZE == 0);
static_assert(alignof(mt_structure_reader_membership) == CACHELINE_SIZE);
static_assert(std::atomic<mt_tree *>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(
    std::atomic<mt_abi_detail::structure_reader_mask_word>::is_always_lock_free);

/* These definitions complete the opaque C tags from mtree_abi.h. */
struct mt_runtime {
  /* Serializes lifecycle changes only; tree operations never take this lock. */
  mutable std::mutex mutex;
  std::atomic<bool> acquired{false};
  SiloRuntime *native = nullptr;
  uint32_t max_threads = 0;
  uint32_t max_key_length = 0;
  std::atomic<mt_runtime_health_state> health{MT_RUNTIME_HEALTHY};
  std::atomic<mt_thread *> thread_registry{nullptr};
  std::atomic<mt_tree *> tree_registry{nullptr};
  /*
   * One independently cached structural-read publication per native core.
   * Writers inspect only slots named by each tree's persistent reader mask.
   */
  std::array<mt_structure_reader_slot, NMAXCORES> structure_readers{};
  std::vector<mt_thread *> threads;
  std::vector<mt_tree *> trees;
};

struct mt_thread {
  mt_runtime *runtime;
  std::thread::id owner;
  int native_core_id;
  /* Owner-thread-only mirror; native TLS remains the scope authority. */
  bool controlled_rcu_active;
  /* Immutable after release-publication through runtime.thread_registry. */
  mt_thread *registry_next;

  mt_thread(mt_runtime *runtime_arg, std::thread::id owner_arg,
            int native_core_id_arg) noexcept
      : runtime(runtime_arg), owner(owner_arg),
        native_core_id(native_core_id_arg), controlled_rcu_active(false),
        registry_next(nullptr) {}
};

struct mt_tree {
  mt_runtime *runtime;
  std::atomic<bool> open;
  /*
   * Immutable after release-publication through runtime.tree_registry. The
   * registry and the native tree intentionally live for the process lifetime:
   * graceful shutdown is not advertised, and mt_tree_release only closes this
   * facade. That makes a registry match a safe lifetime proof without taking a
   * process-wide lock.
   */
  mt_tree *registry_next;
  /*
   * Masstree's optimistic readers intentionally race with plain structural
   * writes under the implementation's native memory model. The C ABI instead
   * excludes get-or-insert from point readers and scans. Readers publish only
   * into their native-core slot, so steady reads never update one shared
   * reader-count cache line.
   */
  mt_structure_sealed_flag structure_sealed;
  mt_structure_writer_flag structure_writer;
  mt_structure_writer_lock structure_lock;
  /*
   * A bit is set before its core first publishes a structural read on this
   * tree and is never cleared. Core IDs are unique and non-recyclable for the
   * lifetime of the native runtime. Keeping membership per tree avoids making
   * an insertion inspect unrelated workers and process-lifetime loader handles.
   */
  mt_structure_reader_membership structure_readers;
  mt_abi_detail::record_tree native;

  explicit mt_tree(mt_runtime *runtime_arg)
      : runtime(runtime_arg), open(true), registry_next(nullptr), native() {
    for (auto &word : structure_readers.words) {
      word.store(0, std::memory_order_relaxed);
    }
  }
};

namespace {

constexpr uint32_t kNativeMaxThreads = static_cast<uint32_t>(NMAXCORES);
constexpr uint32_t kNativeMaxKeyLength =
    static_cast<uint32_t>(MT_CONFIGURED_MAX_KEY_LENGTH);
constexpr mt_feature_set kFeatures =
    MT_FEATURE_POINT_GET | MT_FEATURE_ATOMIC_GET_OR_INSERT |
    MT_FEATURE_EXPLICIT_HANDLES | MT_FEATURE_BINARY_KEYS |
    MT_FEATURE_INTEGRAL_RECORD_IDS | MT_FEATURE_RUNTIME_HEALTH |
    MT_FEATURE_SINGLETON_RUNTIME | MT_FEATURE_COPIED_RANGE_SCANS |
    MT_FEATURE_SCOPED_POINT_READS | MT_FEATURE_SCOPED_STRIDED_POINT_READS |
    MT_FEATURE_STRIDED_POINT_READS | MT_FEATURE_SCOPED_RCU |
    MT_FEATURE_STRUCTURE_SEAL;

/* Deliberately excludes MT_FEATURE_GRACEFUL_SHUTDOWN. */
static_assert((kFeatures & MT_FEATURE_GRACEFUL_SHUTDOWN) == 0);

thread_local mt_thread *tls_thread = nullptr;

/*
 * Tree facade objects are release-published once and intentionally retained
 * for the process lifetime. Cache only pointers that have already passed the
 * registry walk: an arbitrary foreign pointer is still compared as an opaque
 * value and is never dereferenced. TPC-C alternates among a small working set
 * of tables, so this removes the otherwise linear registry walk from nearly
 * every point operation without weakening the hostile-pointer boundary.
 */
constexpr size_t kTreeValidationCacheSlots = 64;
static_assert(std::has_single_bit(kTreeValidationCacheSlots));

class tree_validation_cache final {
public:
  bool contains(const mt_tree *candidate) const noexcept {
    return entries_[slot(candidate)] == candidate;
  }

  void remember(const mt_tree *candidate) noexcept {
    entries_[slot(candidate)] = candidate;
  }

private:
  static size_t slot(const mt_tree *candidate) noexcept {
    std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(candidate);
    bits = (bits >> 6) ^ (bits >> 17) ^ (bits >> 31);
    return static_cast<size_t>(bits) & (kTreeValidationCacheSlots - 1);
  }

  std::array<const mt_tree *, kTreeValidationCacheSlots> entries_{};
};

thread_local tree_validation_cache tls_tree_validation_cache;

/*
 * Standards-race-free structural admission for inherited Masstree.
 *
 * A reader publishes its tree in a cacheline-private core slot, then rechecks
 * the per-tree writer flag. The publication, flag store, recheck, and writer
 * scan are sequentially consistent: this deliberately closes the StoreLoad
 * (Dekker) outcome in which both sides could otherwise miss one another.
 * Therefore either the reader observes the writer and backs out before native
 * access, or the writer observes the reader and waits for its release store.
 */
class structure_read_guard final {
public:
  structure_read_guard(mt_tree &tree, const mt_thread &thread) noexcept
      : tree_(tree) {
    /*
     * A seal is release-published only after the last pre-seal structural
     * reader drains. No structural writer can pass the seal. An acquiring
     * reader may therefore retain only native RCU protection.
     */
    if (tree_.structure_sealed.sealed.load(std::memory_order_acquire)) {
      return;
    }

    const size_t core_id = static_cast<size_t>(thread.native_core_id);
    slot_ = &tree.runtime->structure_readers[core_id];
    const size_t word_index =
        core_id / mt_abi_detail::kStructureReaderMaskWordBits;
    const auto bit = mt_abi_detail::structure_reader_mask_word{1}
                     << (core_id % mt_abi_detail::kStructureReaderMaskWordBits);
    auto &reader_word = tree_.structure_readers.words[word_index];
    if ((reader_word.load(std::memory_order_acquire) & bit) == 0) {
      /*
       * First registration joins the same SC order as the publication/flag
       * handshake below. If a writer snapshots before this RMW, its preceding
       * flag store is visible to the reader's recheck. Otherwise the writer's
       * snapshot contains this core and it drains the published slot.
       */
      reader_word.fetch_or(bit, std::memory_order_seq_cst);
    }
    for (;;) {
      while (tree_.structure_writer.active.load(std::memory_order_seq_cst)) {
        nop_pause();
      }
      slot_->active_tree.store(&tree_, std::memory_order_seq_cst);
      if (!tree_.structure_writer.active.load(std::memory_order_seq_cst)) {
        break;
      }
      slot_->active_tree.store(nullptr, std::memory_order_release);
    }
  }

  structure_read_guard(const structure_read_guard &) = delete;
  structure_read_guard &operator=(const structure_read_guard &) = delete;

  ~structure_read_guard() noexcept {
    if (slot_ != nullptr) {
      slot_->active_tree.store(nullptr, std::memory_order_release);
    }
  }

private:
  mt_tree &tree_;
  mt_structure_reader_slot *slot_ = nullptr;
};

class structure_write_guard final {
public:
  explicit structure_write_guard(mt_tree &tree)
      : tree_(tree), writer_lock_(tree.structure_lock.mutex) {}

  bool try_admit() noexcept {
    /* The mutex orders this load after the one-way seal publication. */
    if (tree_.structure_sealed.sealed.load(std::memory_order_acquire)) {
      return false;
    }
    publish_and_drain();
    return true;
  }

  bool sealed() const noexcept {
    return tree_.structure_sealed.sealed.load(std::memory_order_acquire);
  }

  void synchronize_for_seal() noexcept {
    INVARIANT(!active_);
    publish_and_drain();
  }

  void publish_seal() noexcept {
    INVARIANT(active_);
    tree_.structure_sealed.sealed.store(true, std::memory_order_release);
  }

  structure_write_guard(const structure_write_guard &) = delete;
  structure_write_guard &operator=(const structure_write_guard &) = delete;

  ~structure_write_guard() noexcept {
    if (active_) {
      tree_.structure_writer.active.store(false, std::memory_order_release);
    }
  }

private:
  void publish_and_drain() noexcept {
    INVARIANT(!active_);
    tree_.structure_writer.active.store(true, std::memory_order_seq_cst);
    active_ = true;
    /*
     * Reader-mask registration, slot publication/recheck, and the writer
     * flag/snapshot form one sequentially consistent order. If registration is
     * absent from this snapshot, the reader must observe the preceding writer
     * flag and retract. If registration is present, this writer drains that
     * core's slot before entering inherited Masstree.
     */
    for (size_t word_index = 0;
         word_index != mt_abi_detail::kStructureReaderMaskWords;
         ++word_index) {
      auto readers = tree_.structure_readers.words[word_index].load(
          std::memory_order_seq_cst);
      const size_t first_core_id =
          word_index * mt_abi_detail::kStructureReaderMaskWordBits;
      while (readers != 0) {
        const size_t bit_index = static_cast<size_t>(std::countr_zero(readers));
        const size_t core_id = first_core_id + bit_index;
        INVARIANT(core_id < static_cast<size_t>(NMAXCORES));
        mt_structure_reader_slot &slot =
            tree_.runtime->structure_readers[core_id];
        while (slot.active_tree.load(std::memory_order_seq_cst) == &tree_) {
          nop_pause();
        }
        readers &= readers - 1;
      }
    }
  }

  mt_tree &tree_;
  std::unique_lock<std::mutex> writer_lock_;
  bool active_ = false;
};

/*
 * One private scope state per attached OS thread. The public token contains
 * this state's identity plus a nonrepeating generation; token validation
 * therefore rejects both cross-thread use and a stale token from an earlier
 * scope without dereferencing an attacker-controlled native handle.
 */
class read_scope_state final {
public:
  read_scope_state() = default;
  read_scope_state(const read_scope_state &) = delete;
  read_scope_state &operator=(const read_scope_state &) = delete;

  ~read_scope_state() noexcept { close(); }

  bool active() const noexcept { return active_; }

  bool can_advance_generation() const noexcept {
    return generation_ != std::numeric_limits<uint64_t>::max();
  }

  void admit(mt_tree &tree, mt_thread &thread) noexcept {
    tree_ = &tree;
    thread_ = &thread;
    runtime_ = tree.runtime;
    structure_.emplace(tree, thread);
  }

  void enter_rcu_and_activate() {
    rcu_.emplace();
    ++generation_;
    active_ = true;
  }

  void close() noexcept {
    active_ = false;
    /* Match mt_get: leave native RCU before releasing structural admission. */
    rcu_.reset();
    structure_.reset();
    runtime_ = nullptr;
    thread_ = nullptr;
    tree_ = nullptr;
  }

  bool matches(const mt_read_scope &token) const noexcept {
    return active_ && token.owner == owner_identity() &&
           token.generation == generation_;
  }

  uintptr_t owner_identity() const noexcept {
    return reinterpret_cast<uintptr_t>(this);
  }

  uint64_t generation() const noexcept { return generation_; }
  mt_tree *tree() const noexcept { return tree_; }
  mt_thread *thread() const noexcept { return thread_; }
  mt_runtime *runtime() const noexcept { return runtime_; }

private:
  mt_tree *tree_ = nullptr;
  mt_thread *thread_ = nullptr;
  mt_runtime *runtime_ = nullptr;
  uint64_t generation_ = 0;
  bool active_ = false;
  /* Declaration order guarantees RCU is destroyed before the read guard. */
  std::optional<structure_read_guard> structure_;
  std::optional<scoped_rcu_region> rcu_;
};

thread_local read_scope_state tls_read_scope;

/*
 * A controlled worker-wide RCU scope. Unlike read_scope_state this owns no
 * structural admission and is therefore valid across several trees and
 * get-or-insert calls. Ordinary operations retain their local structural
 * guards. Once checked_operation has validated this scope's runtime and worker
 * identity, they reuse its RCU protection instead of entering a nested region.
 */
class rcu_scope_state final {
public:
  rcu_scope_state() = default;
  rcu_scope_state(const rcu_scope_state &) = delete;
  rcu_scope_state &operator=(const rcu_scope_state &) = delete;

  ~rcu_scope_state() noexcept { close(); }

  bool active() const noexcept { return active_; }

  bool can_advance_generation() const noexcept {
    return generation_ != std::numeric_limits<uint64_t>::max();
  }

  void enter(mt_runtime &runtime, mt_thread &thread) {
    INVARIANT(!active_ && !thread.controlled_rcu_active);
    runtime_ = &runtime;
    thread_ = &thread;
    rcu_.emplace();
    ++generation_;
    active_ = true;
    thread.controlled_rcu_active = true;
  }

  void close() noexcept {
    if (thread_ != nullptr) {
      INVARIANT(thread_->controlled_rcu_active == active_);
      thread_->controlled_rcu_active = false;
    }
    active_ = false;
    rcu_.reset();
    thread_ = nullptr;
    runtime_ = nullptr;
  }

  bool matches(const mt_rcu_scope &token) const noexcept {
    return active_ && token.owner == owner_identity() &&
           token.generation == generation_;
  }

  bool matches(mt_runtime *runtime, mt_thread *thread) const noexcept {
    return active_ && runtime_ == runtime && thread_ == thread;
  }

  uintptr_t owner_identity() const noexcept {
    return reinterpret_cast<uintptr_t>(this);
  }

  uint64_t generation() const noexcept { return generation_; }

private:
  mt_runtime *runtime_ = nullptr;
  mt_thread *thread_ = nullptr;
  uint64_t generation_ = 0;
  bool active_ = false;
  std::optional<scoped_rcu_region> rcu_;
};

thread_local rcu_scope_state tls_rcu_scope;

/*
 * Preserves the established standalone-operation behavior while making the
 * controlled worker scope's hot path a single predictable branch. `covered`
 * is produced only after checked_operation has validated tree/runtime/worker
 * affinity. The invariant makes accidental elision without an active native
 * region fail loudly in invariant-enabled builds.
 */
class operation_rcu_guard final {
public:
  explicit operation_rcu_guard(bool covered) {
    if (covered) {
      INVARIANT(rcu::s_instance.in_rcu_region());
    } else {
      local_.emplace();
    }
  }

  operation_rcu_guard(const operation_rcu_guard &) = delete;
  operation_rcu_guard &operator=(const operation_rcu_guard &) = delete;

private:
  std::optional<scoped_rcu_region> local_;
};

mt_runtime &singleton_runtime() noexcept {
  static mt_runtime runtime;
  return runtime;
}

template <typename Function>
mt_status status_boundary(Function &&function) noexcept {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MT_ERR_CPP_EXCEPTION;
  }
}

struct normalized_config {
  uint32_t max_threads;
  uint32_t max_key_length;
};

mt_status normalize_config(const mt_runtime_config *config,
                           normalized_config *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }

  out->max_threads = kNativeMaxThreads;
  out->max_key_length = kNativeMaxKeyLength;
  if (config == nullptr) {
    return MT_OK;
  }
  if (config->struct_size != sizeof(mt_runtime_config) ||
      config->abi_version != MT_ABI_VERSION) {
    return MT_ERR_ABI_MISMATCH;
  }
  if ((config->required_features & ~kFeatures) != 0) {
    return MT_ERR_ABI_MISMATCH;
  }
  if (config->reserved[0] != 0 || config->reserved[1] != 0) {
    return MT_ERR_INVALID;
  }
  if (config->max_threads > kNativeMaxThreads ||
      config->max_key_length > kNativeMaxKeyLength) {
    return MT_ERR_INVALID;
  }
  if (config->max_threads != 0) {
    out->max_threads = config->max_threads;
  }
  if (config->max_key_length != 0) {
    out->max_key_length = config->max_key_length;
  }
  if (out->max_threads == 0 || out->max_key_length == 0) {
    return MT_ERR_INVALID;
  }
  return MT_OK;
}

mt_status checked_runtime(const mt_runtime *candidate, mt_runtime **out) {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = nullptr;
  mt_runtime &runtime = singleton_runtime();
  if (candidate == nullptr) {
    return MT_ERR_INVALID;
  }
  /* Compare first: an arbitrary foreign pointer is never dereferenced. */
  if (candidate != &runtime) {
    return MT_ERR_INVALID;
  }
  if (!runtime.acquired.load(std::memory_order_acquire)) {
    return MT_ERR_INVALID;
  }
  *out = &runtime;
  return MT_OK;
}

bool registered_thread(const mt_runtime &runtime,
                       const mt_thread *candidate) noexcept {
  /*
   * Handles are appended under runtime.mutex, release-published here, and
   * never removed or freed. Compare before dereferencing candidate so even an
   * arbitrary foreign address fails closed.
   */
  for (const mt_thread *current =
           runtime.thread_registry.load(std::memory_order_acquire);
       current != nullptr; current = current->registry_next) {
    if (current == candidate) {
      return true;
    }
  }
  return false;
}

bool registered_tree(const mt_runtime &runtime,
                     const mt_tree *candidate) noexcept {
  for (const mt_tree *current =
           runtime.tree_registry.load(std::memory_order_acquire);
       current != nullptr; current = current->registry_next) {
    if (current == candidate) {
      return true;
    }
  }
  return false;
}

mt_status checked_thread(mt_runtime *expected_runtime, mt_thread *candidate,
                         bool reject_active_rcu,
                         bool allow_controlled_rcu = false,
                         bool *controlled_rcu_out = nullptr) {
  if (controlled_rcu_out != nullptr) {
    *controlled_rcu_out = false;
  }
  if (candidate == nullptr) {
    return MT_ERR_NOT_ATTACHED;
  }
  /* The TLS equality is both the common path and a lifetime proof. */
  if (candidate != tls_thread &&
      !registered_thread(*expected_runtime, candidate)) {
    return MT_ERR_INVALID;
  }
  if (candidate != tls_thread &&
      candidate->owner != std::this_thread::get_id()) {
    return MT_ERR_WRONG_THREAD;
  }
  if (tls_thread == nullptr || candidate != tls_thread) {
    return MT_ERR_NOT_ATTACHED;
  }
  if (candidate->runtime != expected_runtime ||
      tl_silo_runtime != expected_runtime->native) {
    return MT_ERR_WRONG_RUNTIME;
  }
  if (candidate->native_core_id < 0 ||
      candidate->native_core_id >= static_cast<int>(NMAXCORES)) {
    return MT_ERR_NOT_ATTACHED;
  }
  if (!coreid::matches_current_assignment(expected_runtime->native->id(),
                                          candidate->native_core_id)) {
    return MT_ERR_NOT_ATTACHED;
  }
  /*
   * Full validation above proves this is the current worker's opaque handle,
   * so its owner-thread-only mirror avoids a dynamic TLS-wrapper lookup on
   * every operation. Native TLS still owns the guard, token, and cleanup.
   */
  INVARIANT(candidate->controlled_rcu_active ==
            tls_rcu_scope.matches(expected_runtime, candidate));
  INVARIANT(!candidate->controlled_rcu_active ||
            rcu::s_instance.in_rcu_region());
  const bool controlled_rcu =
      allow_controlled_rcu && candidate->controlled_rcu_active;
  if (controlled_rcu_out != nullptr) {
    *controlled_rcu_out = controlled_rcu;
  }
  if (reject_active_rcu && !controlled_rcu &&
      (tls_read_scope.active() || rcu::s_instance.in_rcu_region())) {
    return MT_ERR_ACTIVE_GUARDS;
  }
  return MT_OK;
}

mt_status checked_tree(mt_tree *candidate, mt_tree **out) {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = nullptr;
  if (candidate == nullptr) {
    return MT_ERR_INVALID;
  }
  mt_runtime &runtime = singleton_runtime();
  if (!runtime.acquired.load(std::memory_order_acquire)) {
    return MT_ERR_INVALID;
  }
  if (!tls_tree_validation_cache.contains(candidate)) {
    if (!registered_tree(runtime, candidate)) {
      return MT_ERR_INVALID;
    }
    tls_tree_validation_cache.remember(candidate);
  }
  /* Dereference only after registry membership proves this is our handle. */
  if (!candidate->open.load(std::memory_order_acquire)) {
    return MT_ERR_CLOSED;
  }
  *out = candidate;
  return MT_OK;
}

mt_status checked_operation(mt_tree *tree_candidate,
                            mt_thread *thread_candidate, mt_tree **tree_out,
                            mt_runtime **runtime_out,
                            bool allow_controlled_rcu,
                            bool &controlled_rcu_out) {
  if (tree_out == nullptr || runtime_out == nullptr) {
    return MT_ERR_INVALID;
  }
  *tree_out = nullptr;
  *runtime_out = nullptr;
  controlled_rcu_out = false;

  mt_tree *tree = nullptr;
  mt_status status = checked_tree(tree_candidate, &tree);
  if (status != MT_OK) {
    return status;
  }
  mt_runtime *runtime = tree->runtime;
  status = checked_thread(runtime, thread_candidate, true,
                          allow_controlled_rcu,
                          &controlled_rcu_out);
  if (status != MT_OK) {
    controlled_rcu_out = false;
    return status;
  }
  if (runtime->health.load(std::memory_order_acquire) != MT_RUNTIME_HEALTHY) {
    controlled_rcu_out = false;
    return MT_ERR_POISONED;
  }
  *tree_out = tree;
  *runtime_out = runtime;
  return MT_OK;
}

mt_status check_key(const mt_runtime &runtime, const void *key,
                    size_t key_length) noexcept {
  if (key == nullptr && key_length != 0) {
    return MT_ERR_INVALID;
  }
  if (key_length > runtime.max_key_length ||
      key_length > MT_CONFIGURED_MAX_KEY_LENGTH ||
      key_length > static_cast<size_t>(INT_MAX)) {
    return MT_ERR_KEY_TOO_LARGE;
  }
  return MT_OK;
}

varkey make_key(const void *key, size_t key_length) noexcept {
  static constexpr uint8_t kEmptyKeyStorage = 0;
  const auto *bytes =
      key_length == 0 ? &kEmptyKeyStorage : static_cast<const uint8_t *>(key);
  return varkey(bytes, key_length);
}

void poison(mt_runtime *runtime) noexcept;

mt_status initialize_strided_output(size_t key_count,
                                    mt_record_id *out) noexcept {
  /* Reject impossible spans before touching caller storage. */
  if (key_count > std::numeric_limits<size_t>::max() / sizeof(*out) ||
      (key_count != 0 && out == nullptr)) {
    return MT_ERR_INVALID;
  }
  const size_t output_bytes = key_count * sizeof(*out);
  if (out != nullptr && output_bytes > std::numeric_limits<uintptr_t>::max() -
                                           reinterpret_cast<uintptr_t>(out)) {
    return MT_ERR_INVALID;
  }
  if (key_count != 0) {
    std::fill_n(out, key_count, MT_RECORD_ID_NONE);
  }
  return MT_OK;
}

void clear_strided_output(size_t key_count, mt_record_id *out) noexcept {
  if (key_count != 0) {
    std::fill_n(out, key_count, MT_RECORD_ID_NONE);
  }
}

mt_status check_strided_keys(const mt_runtime &runtime, const void *keys,
                             size_t key_count, size_t key_length,
                             size_t key_stride,
                             const uint8_t **cursor_out) noexcept {
  static constexpr uint8_t kShapeProbe = 0;
  const void *shape_key =
      key_count == 0 ? static_cast<const void *>(&kShapeProbe) : keys;
  mt_status status = check_key(runtime, shape_key, key_length);
  if (status != MT_OK) {
    return status;
  }
  if (key_stride < key_length) {
    return MT_ERR_INVALID;
  }
  if (key_count == 0) {
    *cursor_out = nullptr;
    return MT_OK;
  }

  size_t last_key_offset = 0;
  if (key_count > 1) {
    const size_t intervals = key_count - 1;
    if (key_stride > std::numeric_limits<size_t>::max() / intervals) {
      return MT_ERR_INVALID;
    }
    last_key_offset = intervals * key_stride;
  }
  if (key_length > std::numeric_limits<size_t>::max() - last_key_offset) {
    return MT_ERR_INVALID;
  }
  const size_t byte_span = last_key_offset + key_length;
  if (keys != nullptr) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(keys);
    if (byte_span > std::numeric_limits<uintptr_t>::max() - base) {
      return MT_ERR_INVALID;
    }
  }
  *cursor_out = static_cast<const uint8_t *>(keys);
  return MT_OK;
}

mt_status search_strided(mt_tree &tree, mt_runtime *runtime,
                         const uint8_t *cursor, size_t key_count,
                         size_t key_length, size_t key_stride,
                         mt_record_id *out) {
  for (size_t index = 0; index != key_count; ++index) {
    const varkey native_key = make_key(cursor, key_length);
    mt_record_id value = MT_RECORD_ID_NONE;
    if (tree.native.search(native_key, value)) {
      if (value == MT_RECORD_ID_NONE) {
        poison(runtime);
        return MT_ERR_INTERNAL;
      }
      out[index] = value;
    }
    if (key_length != 0 && index + 1 != key_count) {
      cursor += key_stride;
    }
  }
  return MT_OK;
}

void poison(mt_runtime *runtime) noexcept {
  if (runtime != nullptr) {
    runtime->health.store(MT_RUNTIME_POISONED, std::memory_order_release);
  }
}

void initialize_insert_result(mt_get_or_insert_result *out) noexcept {
  out->winner = MT_RECORD_ID_NONE;
  out->publication = MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION;
  out->inserted = 0;
  out->reserved[0] = 0;
  out->reserved[1] = 0;
  out->reserved[2] = 0;
}

void initialize_insert_results(size_t count,
                               mt_get_or_insert_result *out) noexcept {
  for (size_t index = 0; index != count; ++index) {
    initialize_insert_result(&out[index]);
  }
}

/*
 * Preflight for the private Rust-owned-handle lane. References are formed by
 * the trusted entry points before reaching this function, so none of these
 * assertions is a hostile-pointer defense. They document and diagnose the
 * unsafe caller contract without retaining the release-build validation cost.
 */
mt_status trusted_operation(mt_tree &tree, mt_thread &thread,
                            mt_runtime **runtime_out,
                            bool &controlled_rcu_out) noexcept {
  mt_runtime *runtime = tree.runtime;
  INVARIANT(runtime != nullptr);
  INVARIANT(runtime == &singleton_runtime());
  INVARIANT(runtime->acquired.load(std::memory_order_acquire));
  INVARIANT(tree.open.load(std::memory_order_acquire));
  INVARIANT(&thread == tls_thread);
  INVARIANT(thread.runtime == runtime);
  INVARIANT(tl_silo_runtime == runtime->native);
  INVARIANT(thread.native_core_id >= 0);
  INVARIANT(thread.native_core_id < static_cast<int>(NMAXCORES));
  INVARIANT(coreid::matches_current_assignment(runtime->native->id(),
                                               thread.native_core_id));
  INVARIANT(thread.controlled_rcu_active ==
            tls_rcu_scope.matches(runtime, &thread));
  INVARIANT(!thread.controlled_rcu_active ||
            rcu::s_instance.in_rcu_region());

  controlled_rcu_out = thread.controlled_rcu_active;
  if (!controlled_rcu_out &&
      (tls_read_scope.active() || rcu::s_instance.in_rcu_region())) {
    return MT_ERR_ACTIVE_GUARDS;
  }
  if (runtime->health.load(std::memory_order_acquire) != MT_RUNTIME_HEALTHY) {
    controlled_rcu_out = false;
    return MT_ERR_POISONED;
  }
  *runtime_out = runtime;
  return MT_OK;
}

mt_status point_get_validated(mt_tree &tree, mt_thread &thread,
                              mt_runtime *runtime, bool controlled_rcu,
                              const void *key, size_t key_length,
                              mt_record_id *out) noexcept {
  try {
    mt_record_id found = MT_RECORD_ID_NONE;
    {
      structure_read_guard structure_guard(tree, thread);
      /* A writer may have poisoned the runtime while this reader waited. */
      if (runtime->health.load(std::memory_order_acquire) !=
          MT_RUNTIME_HEALTHY) {
        return MT_ERR_POISONED;
      }
      try {
        operation_rcu_guard region(controlled_rcu);
        const varkey native_key = make_key(key, key_length);
        mt_record_id value = MT_RECORD_ID_NONE;
        if (tree.native.search(native_key, value)) {
          if (value == MT_RECORD_ID_NONE) {
            poison(runtime);
            return MT_ERR_INTERNAL;
          }
          found = value;
        }
      } catch (const std::bad_alloc &) {
        throw;
      } catch (...) {
        /* Poison before a queued structural writer can pass admission. */
        poison(runtime);
        throw;
      }
    }
    *out = found;
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

mt_status point_get_strided_validated(mt_tree &tree, mt_thread &thread,
                                      mt_runtime *runtime, bool controlled_rcu,
                                      const uint8_t *cursor, size_t key_count,
                                      size_t key_length, size_t key_stride,
                                      mt_record_id *out) noexcept {
  try {
    {
      structure_read_guard structure_guard(tree, thread);
      /* A writer may have poisoned the runtime while this reader waited. */
      if (runtime->health.load(std::memory_order_acquire) !=
          MT_RUNTIME_HEALTHY) {
        return MT_ERR_POISONED;
      }
      try {
        operation_rcu_guard region(controlled_rcu);
        const mt_status status = search_strided(
            tree, runtime, cursor, key_count, key_length, key_stride, out);
        if (status != MT_OK) {
          clear_strided_output(key_count, out);
          return status;
        }
      } catch (const std::bad_alloc &) {
        throw;
      } catch (...) {
        /* Poison before a queued structural writer can pass admission. */
        poison(runtime);
        throw;
      }
    }
    return MT_OK;
  } catch (const std::bad_alloc &) {
    clear_strided_output(key_count, out);
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    clear_strided_output(key_count, out);
    poison(runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

mt_status get_or_insert_validated(mt_tree &tree, mt_thread &thread,
                                  mt_runtime *runtime, bool controlled_rcu,
                                  const void *key, size_t key_length,
                                  mt_record_id candidate,
                                  mt_get_or_insert_result *out) noexcept {
  bool publication_attempted = false;
  bool publication_classified = false;
  const auto classify_unfinished_publication = [&]() noexcept {
    if (!publication_classified) {
      out->winner = MT_RECORD_ID_NONE;
      out->inserted = 0;
      out->publication = publication_attempted
                             ? MT_PUBLICATION_UNKNOWN
                             : MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION;
    }
  };

  try {
    {
      structure_write_guard structure_guard(tree);
      if (!structure_guard.try_admit()) {
        return MT_ERR_STRUCTURE_SEALED;
      }
      /* A preceding queued writer may have poisoned before releasing. */
      if (runtime->health.load(std::memory_order_acquire) !=
          MT_RUNTIME_HEALTHY) {
        return MT_ERR_POISONED;
      }
      try {
        operation_rcu_guard region(controlled_rcu);
        const varkey native_key = make_key(key, key_length);
        mt_record_id winner = MT_RECORD_ID_NONE;

        /*
         * Resolve or publish with one locked cursor. Structural-writer
         * admission makes the cursor's observed old value the stable
         * append-only winner.
         */
        publication_attempted = true;
        const bool inserted =
            tree.native.insert_if_absent_with_old(native_key, candidate,
                                                  winner);
        if (inserted) {
          out->winner = candidate;
          out->inserted = 1;
          out->publication = MT_PUBLICATION_CANDIDATE_INSERTED;
          publication_classified = true;
        } else {
          /* A false return proves this candidate was not stored. */
          out->publication = MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED;
          publication_classified = true;
          if (winner == MT_RECORD_ID_NONE) {
            poison(runtime);
            return MT_ERR_INTERNAL;
          }
          out->winner = winner;
        }
      } catch (const std::bad_alloc &) {
        classify_unfinished_publication();
        if (out->publication == MT_PUBLICATION_UNKNOWN) {
          poison(runtime);
        }
        /* Rethrow only after readers will observe the poisoned runtime. */
        throw;
      } catch (...) {
        classify_unfinished_publication();
        poison(runtime);
        /* The structural writer guard remains held until this rethrow. */
        throw;
      }
    }
    return MT_OK;
  } catch (const std::bad_alloc &) {
    classify_unfinished_publication();
    if (out->publication == MT_PUBLICATION_UNKNOWN) {
      poison(runtime);
    }
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    classify_unfinished_publication();
    poison(runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

/*
 * Trusted fixed-shape batch counterpart to get_or_insert_validated. The safe
 * Rust facade has already proved all slice spans, key shape, handle affinity,
 * and pairwise-distinct nonzero candidates. Keeping the publication state of
 * the current row outside the Masstree call is intentional: every catch path
 * can classify that row before releasing structural-writer admission, while
 * the already completed prefix and untouched suffix remain exact.
 */
mt_status get_or_insert_strided_trusted_validated(
    mt_tree &tree, mt_thread &thread, mt_runtime *runtime,
    bool controlled_rcu, const uint8_t *cursor, size_t key_count,
    size_t key_length, size_t key_stride, const mt_record_id *candidates,
    mt_get_or_insert_result *out) noexcept {
  size_t current = key_count;
  bool publication_attempted = false;
  bool publication_classified = false;
  const auto classify_unfinished_publication = [&]() noexcept {
    if (current == key_count || publication_classified) {
      return;
    }
    mt_get_or_insert_result &result = out[current];
    result.winner = MT_RECORD_ID_NONE;
    result.inserted = 0;
    result.publication = publication_attempted
                             ? MT_PUBLICATION_UNKNOWN
                             : MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION;
  };

  try {
    {
      structure_write_guard structure_guard(tree);
      if (!structure_guard.try_admit()) {
        return MT_ERR_STRUCTURE_SEALED;
      }
      /* A preceding queued writer may have poisoned before releasing. */
      if (runtime->health.load(std::memory_order_acquire) !=
          MT_RUNTIME_HEALTHY) {
        return MT_ERR_POISONED;
      }
      try {
        operation_rcu_guard region(controlled_rcu);
        for (current = 0; current != key_count; ++current) {
          publication_attempted = false;
          publication_classified = false;
          mt_get_or_insert_result &result = out[current];
          const varkey native_key = make_key(cursor, key_length);
          mt_record_id winner = MT_RECORD_ID_NONE;

          publication_attempted = true;
          const bool inserted = tree.native.insert_if_absent_with_old(
              native_key, candidates[current], winner);
          if (inserted) {
            result.winner = candidates[current];
            result.inserted = 1;
            result.publication = MT_PUBLICATION_CANDIDATE_INSERTED;
            publication_classified = true;
          } else {
            /* A false return proves this exact candidate was not stored. */
            result.publication = MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED;
            publication_classified = true;
            if (winner == MT_RECORD_ID_NONE) {
              poison(runtime);
              return MT_ERR_INTERNAL;
            }
            result.winner = winner;
          }
          if (key_length != 0 && current + 1 != key_count) {
            cursor += key_stride;
          }
        }
        current = key_count;
      } catch (const std::bad_alloc &) {
        classify_unfinished_publication();
        if (current != key_count &&
            out[current].publication == MT_PUBLICATION_UNKNOWN) {
          poison(runtime);
        }
        throw;
      } catch (...) {
        classify_unfinished_publication();
        poison(runtime);
        throw;
      }
    }
    return MT_OK;
  } catch (const std::bad_alloc &) {
    classify_unfinished_publication();
    if (current != key_count &&
        out[current].publication == MT_PUBLICATION_UNKNOWN) {
      poison(runtime);
    }
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    classify_unfinished_publication();
    poison(runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

void initialize_scan_result(mt_scan_result *out) noexcept {
  *out = mt_scan_result{};
  out->stop_reason = MT_SCAN_STOP_END;
  out->resume = MT_SCAN_RESUME_NONE;
}

/* Private result used only by the Rust bounded RecordId scan. */
struct trusted_record_id_scan_result {
  size_t records_written;
  size_t continuation_bytes_used;
  size_t next_key_bytes_required;
  mt_scan_stop_reason stop_reason;
  mt_scan_resume_kind resume;
  uint64_t reserved[2];
};

constexpr mt_scan_resume_kind kTrustedScanResumeInclusiveNext = 3;

static_assert(std::is_standard_layout_v<trusted_record_id_scan_result>);
static_assert(sizeof(trusted_record_id_scan_result) == 48);
static_assert(alignof(trusted_record_id_scan_result) == alignof(uint64_t));

void initialize_trusted_record_id_scan_result(
    trusted_record_id_scan_result *out) noexcept {
  *out = trusted_record_id_scan_result{};
  out->stop_reason = MT_SCAN_STOP_END;
  out->resume = MT_SCAN_RESUME_NONE;
}

mt_status check_scan_bound(const mt_runtime &runtime,
                           const mt_scan_bound &bound) noexcept {
  if (bound.reserved != 0) {
    return MT_ERR_INVALID;
  }
  switch (bound.kind) {
  case MT_SCAN_BOUND_ABSENT:
    return bound.key == nullptr && bound.key_length == 0 ? MT_OK
                                                         : MT_ERR_INVALID;
  case MT_SCAN_BOUND_INCLUSIVE:
  case MT_SCAN_BOUND_EXCLUSIVE:
    return check_key(runtime, bound.key, bound.key_length);
  default:
    return MT_ERR_INVALID;
  }
}

bool bound_is_present(const mt_scan_bound &bound) noexcept {
  return bound.kind != MT_SCAN_BOUND_ABSENT;
}

int compare_byte_strings(const void *left, size_t left_length,
                         const void *right, size_t right_length) noexcept {
  const size_t common = std::min(left_length, right_length);
  if (common != 0) {
    const int prefix = std::memcmp(left, right, common);
    if (prefix != 0) {
      return prefix;
    }
  }
  if (left_length < right_length) {
    return -1;
  }
  return left_length > right_length ? 1 : 0;
}

int compare_key_to_bound(lcdf::Str key, const mt_scan_bound &bound) noexcept {
  return compare_byte_strings(key.s, static_cast<size_t>(key.len), bound.key,
                              bound.key_length);
}

class copied_scan_collector final
    : public mt_abi_detail::record_tree::low_level_search_range_callback {
public:
  using record_tree = mt_abi_detail::record_tree;

  copied_scan_collector(mt_scan_direction direction, const mt_scan_bound &lower,
                        const mt_scan_bound &upper, mt_scan_entry *entries,
                        size_t entry_capacity, uint8_t *key_arena,
                        size_t key_arena_capacity) noexcept
      : direction_(direction), lower_(lower), upper_(upper), entries_(entries),
        entry_capacity_(entry_capacity), key_arena_(key_arena),
        key_arena_capacity_(key_arena_capacity) {}

  ~copied_scan_collector() noexcept override = default;

  void on_resp_node(const record_tree::node_opaque_t *,
                    uint64_t) noexcept override {}

  bool invoke(const record_tree::string_type &key, mt_record_id record_id,
              const record_tree::node_opaque_t *, uint64_t) noexcept override {
    if (key.len < 0 ||
        static_cast<size_t>(key.len) > MT_CONFIGURED_MAX_KEY_LENGTH ||
        (key.len != 0 && key.s == nullptr) || record_id == MT_RECORD_ID_NONE) {
      invalid_native_entry_ = true;
      return false;
    }

    if (bound_is_present(lower_)) {
      const int comparison = compare_key_to_bound(key, lower_);
      const bool below = comparison < 0;
      const bool excluded_equal =
          comparison == 0 && lower_.kind == MT_SCAN_BOUND_EXCLUSIVE;
      if (below || excluded_equal) {
        return direction_ == MT_SCAN_FORWARD;
      }
    }
    if (bound_is_present(upper_)) {
      const int comparison = compare_key_to_bound(key, upper_);
      const bool above = comparison > 0;
      const bool excluded_equal =
          comparison == 0 && upper_.kind == MT_SCAN_BOUND_EXCLUSIVE;
      if (above || excluded_equal) {
        return direction_ == MT_SCAN_REVERSE;
      }
    }

    const size_t key_length = static_cast<size_t>(key.len);
    if (entries_written_ == entry_capacity_) {
      stop_reason_ = MT_SCAN_STOP_ENTRY_CAPACITY;
      next_key_bytes_required_ = key_length;
      return false;
    }
    if (key_length > key_arena_capacity_ - arena_bytes_used_) {
      stop_reason_ = MT_SCAN_STOP_KEY_ARENA_CAPACITY;
      next_key_bytes_required_ = key_length;
      return false;
    }

    if (key_length != 0) {
      std::memcpy(key_arena_ + arena_bytes_used_, key.s, key_length);
    }
    entries_[entries_written_] =
        mt_scan_entry{arena_bytes_used_, key_length, record_id};
    resume_key_offset_ = arena_bytes_used_;
    resume_key_length_ = key_length;
    arena_bytes_used_ += key_length;
    ++entries_written_;
    return true;
  }

  bool invalid_native_entry() const noexcept { return invalid_native_entry_; }

  void publish(mt_scan_result *out) const noexcept {
    out->entries_written = entries_written_;
    out->arena_bytes_used = arena_bytes_used_;
    out->stop_reason = stop_reason_;
    if (stop_reason_ == MT_SCAN_STOP_END) {
      return;
    }

    out->next_key_bytes_required = next_key_bytes_required_;
    if (entries_written_ == 0) {
      out->resume = MT_SCAN_RESUME_UNCHANGED_INPUT;
      return;
    }
    out->resume = MT_SCAN_RESUME_EXCLUSIVE_LAST;
    out->resume_key_offset = resume_key_offset_;
    out->resume_key_length = resume_key_length_;
  }

private:
  mt_scan_direction direction_;
  mt_scan_bound lower_;
  mt_scan_bound upper_;
  mt_scan_entry *entries_;
  size_t entry_capacity_;
  uint8_t *key_arena_;
  size_t key_arena_capacity_;
  size_t entries_written_ = 0;
  size_t arena_bytes_used_ = 0;
  size_t next_key_bytes_required_ = 0;
  size_t resume_key_offset_ = 0;
  size_t resume_key_length_ = 0;
  mt_scan_stop_reason stop_reason_ = MT_SCAN_STOP_END;
  bool invalid_native_entry_ = false;
};

mt_status copied_scan_validated(mt_tree &tree, mt_thread &thread,
                                mt_runtime *runtime, bool controlled_rcu,
                                mt_scan_direction direction,
                                const mt_scan_bound &lower,
                                const mt_scan_bound &upper,
                                mt_scan_entry *entries, size_t entry_capacity,
                                uint8_t *key_arena,
                                size_t key_arena_capacity,
                                mt_scan_result *out) noexcept {
  try {
    if (bound_is_present(lower) && bound_is_present(upper)) {
      const int ordering = compare_byte_strings(
          lower.key, lower.key_length, upper.key, upper.key_length);
      if (ordering > 0 ||
          (ordering == 0 && (lower.kind == MT_SCAN_BOUND_EXCLUSIVE ||
                             upper.kind == MT_SCAN_BOUND_EXCLUSIVE))) {
        return MT_OK;
      }
    }

    copied_scan_collector collector(direction, lower, upper, entries,
                                    entry_capacity, key_arena,
                                    key_arena_capacity);
    {
      structure_read_guard structure_guard(tree, thread);
      /* A writer may have poisoned the runtime while this reader waited. */
      if (runtime->health.load(std::memory_order_acquire) !=
          MT_RUNTIME_HEALTHY) {
        return MT_ERR_POISONED;
      }
      try {
        operation_rcu_guard region(controlled_rcu);
        if (direction == MT_SCAN_FORWARD) {
          const varkey native_lower =
              bound_is_present(lower) ? make_key(lower.key, lower.key_length)
                                      : make_key(nullptr, 0);
          if (upper.kind == MT_SCAN_BOUND_EXCLUSIVE) {
            const varkey native_upper = make_key(upper.key, upper.key_length);
            tree.native.search_range_call_bounded(native_lower, native_upper,
                                                  collector);
          } else {
            tree.native.search_range_call_unbounded(native_lower, collector);
          }
        } else {
          std::array<uint8_t, MT_CONFIGURED_MAX_KEY_LENGTH> maximum_key{};
          maximum_key.fill(UINT8_MAX);
          const varkey native_upper =
              bound_is_present(upper)
                  ? make_key(upper.key, upper.key_length)
                  : make_key(maximum_key.data(), maximum_key.size());
          if (lower.kind == MT_SCAN_BOUND_EXCLUSIVE) {
            const varkey native_lower = make_key(lower.key, lower.key_length);
            tree.native.rsearch_range_call_bounded(native_upper, native_lower,
                                                   collector);
          } else {
            tree.native.rsearch_range_call_unbounded(native_upper, collector);
          }
        }
      } catch (const std::bad_alloc &) {
        throw;
      } catch (...) {
        /* Poison before a queued structural writer can pass admission. */
        poison(runtime);
        throw;
      }

      if (collector.invalid_native_entry()) {
        poison(runtime);
        return MT_ERR_INTERNAL;
      }
    }

    collector.publish(out);
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

class trusted_record_id_scan_collector final
    : public mt_abi_detail::record_tree::low_level_search_range_callback {
public:
  using record_tree = mt_abi_detail::record_tree;

  trusted_record_id_scan_collector(mt_record_id *records,
                                   size_t record_capacity,
                                   uint8_t *continuation,
                                   size_t continuation_capacity) noexcept
      : records_(records), record_capacity_(record_capacity),
        continuation_(continuation),
        continuation_capacity_(continuation_capacity) {}

  ~trusted_record_id_scan_collector() noexcept override = default;

  void on_resp_node(const record_tree::node_opaque_t *,
                    uint64_t) noexcept override {}

  bool invoke(const record_tree::string_type &key, mt_record_id record_id,
              const record_tree::node_opaque_t *, uint64_t) noexcept override {
    if (key.len < 0 ||
        static_cast<size_t>(key.len) > MT_CONFIGURED_MAX_KEY_LENGTH ||
        (key.len != 0 && key.s == nullptr) || record_id == MT_RECORD_ID_NONE) {
      invalid_native_entry_ = true;
      return false;
    }

    if (records_written_ != record_capacity_) {
      records_[records_written_++] = record_id;
      return true;
    }

    const size_t key_length = static_cast<size_t>(key.len);
    next_key_bytes_required_ = key_length;
    if (key_length > continuation_capacity_) {
      stop_reason_ = MT_SCAN_STOP_KEY_ARENA_CAPACITY;
      return false;
    }
    if (key_length != 0) {
      std::memcpy(continuation_, key.s, key_length);
    }
    continuation_bytes_used_ = key_length;
    stop_reason_ = MT_SCAN_STOP_ENTRY_CAPACITY;
    return false;
  }

  bool invalid_native_entry() const noexcept { return invalid_native_entry_; }

  void publish(trusted_record_id_scan_result *out) const noexcept {
    out->stop_reason = stop_reason_;
    out->next_key_bytes_required = next_key_bytes_required_;
    if (stop_reason_ == MT_SCAN_STOP_END) {
      out->records_written = records_written_;
      return;
    }
    if (stop_reason_ == MT_SCAN_STOP_ENTRY_CAPACITY) {
      out->records_written = records_written_;
      out->continuation_bytes_used = continuation_bytes_used_;
      out->resume = kTrustedScanResumeInclusiveNext;
      return;
    }

    /* Rust grows the one-key buffer and retries the unchanged lower bound. */
    out->resume = MT_SCAN_RESUME_UNCHANGED_INPUT;
  }

private:
  mt_record_id *records_;
  size_t record_capacity_;
  uint8_t *continuation_;
  size_t continuation_capacity_;
  size_t records_written_ = 0;
  size_t continuation_bytes_used_ = 0;
  size_t next_key_bytes_required_ = 0;
  mt_scan_stop_reason stop_reason_ = MT_SCAN_STOP_END;
  bool invalid_native_entry_ = false;
};

mt_status trusted_record_id_scan_validated(
    mt_tree &tree, mt_thread &thread, mt_runtime *runtime, bool controlled_rcu,
    const void *lower, size_t lower_length, const void *upper,
    size_t upper_length, mt_record_id *records, size_t record_capacity,
    uint8_t *continuation, size_t continuation_capacity,
    trusted_record_id_scan_result *out) noexcept {
  try {
    if (compare_byte_strings(lower, lower_length, upper, upper_length) >= 0) {
      return MT_OK;
    }

    trusted_record_id_scan_collector collector(
        records, record_capacity, continuation, continuation_capacity);
    {
      structure_read_guard structure_guard(tree, thread);
      if (runtime->health.load(std::memory_order_acquire) !=
          MT_RUNTIME_HEALTHY) {
        return MT_ERR_POISONED;
      }
      try {
        operation_rcu_guard region(controlled_rcu);
        const varkey native_lower = make_key(lower, lower_length);
        const varkey native_upper = make_key(upper, upper_length);
        tree.native.search_range_call_bounded(native_lower, native_upper,
                                              collector);
      } catch (const std::bad_alloc &) {
        throw;
      } catch (...) {
        poison(runtime);
        throw;
      }

      if (collector.invalid_native_entry()) {
        poison(runtime);
        return MT_ERR_INTERNAL;
      }
    }

    collector.publish(out);
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

constexpr uint64_t fnv1a_append(uint64_t hash, const char *text) noexcept {
  while (*text != '\0') {
    hash ^= static_cast<unsigned char>(*text++);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

constexpr uint64_t fnv1a(const char *text) noexcept {
  return fnv1a_append(UINT64_C(14695981039346656037), text);
}

#define MT_STRINGIFY_INNER(value) #value
#define MT_STRINGIFY(value) MT_STRINGIFY_INNER(value)

constexpr char kExportedSymbols[] =
    "mt_abi_version;mt_feature_bits;mt_endianness;mt_pointer_width;"
    "mt_max_key_length;mt_max_threads;mt_record_id_limit;"
    "mt_runtime_config_size;mt_runtime_config_alignment;mt_build_id_size;"
    "mt_build_id_alignment;mt_read_scope_size;mt_read_scope_alignment;"
    "mt_get_or_insert_result_size;"
    "mt_get_or_insert_result_alignment;mt_scan_bound_size;"
    "mt_scan_bound_alignment;mt_scan_entry_size;mt_scan_entry_alignment;"
    "mt_scan_result_size;mt_scan_result_alignment;"
    "mt_exported_symbols_fingerprint;"
    "mt_get_build_fingerprint;mt_runtime_config_init;mt_runtime_acquire;"
    "mt_runtime_health;mt_runtime_max_key_length;mt_runtime_max_threads;"
    "mt_runtime_shutdown;mt_thread_attach;mt_thread_quiesce;mt_tree_create;"
    "mt_tree_release;mt_tree_seal_structure;mt_get;mt_get_strided;"
    "mt_read_scope_begin;mt_read_scope_get;"
    "mt_read_scope_get_strided;mt_read_scope_end;mt_rcu_scope_begin;"
    "mt_rcu_scope_end;mt_get_or_insert;mt_scan";

constexpr char kBuildDescription[] =
    "mtree-abi=" MT_STRINGIFY(MT_ABI_VERSION) ";cxx=" MT_STRINGIFY(
        __cplusplus) ";compiler=" __VERSION__
                     ";max-key=" MT_STRINGIFY(
                         MASSTREE_MAXKEYLEN) ";max-cores=" MT_STRINGIFY(NMAXCORES);

#undef MT_STRINGIFY
#undef MT_STRINGIFY_INNER

constexpr uint64_t kExportedSymbolsFingerprint = fnv1a(kExportedSymbols);
constexpr uint64_t kBuildDescriptionFingerprint = fnv1a(kBuildDescription);

uint64_t mix_build_number(uint64_t hash, uint64_t value) noexcept {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    hash ^= static_cast<uint8_t>(value >> shift);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

mt_build_id build_id() noexcept {
  mt_build_id result{};
  result.low = kBuildDescriptionFingerprint;
  uint64_t high = kExportedSymbolsFingerprint;
  high = mix_build_number(high, sizeof(void *));
  high = mix_build_number(high, alignof(void *));
  high = mix_build_number(high, sizeof(mt_runtime_config));
  high = mix_build_number(high, alignof(mt_runtime_config));
  high = mix_build_number(high, sizeof(mt_build_id));
  high = mix_build_number(high, alignof(mt_build_id));
  high = mix_build_number(high, sizeof(mt_read_scope));
  high = mix_build_number(high, alignof(mt_read_scope));
  high = mix_build_number(high, sizeof(mt_get_or_insert_result));
  high = mix_build_number(high, alignof(mt_get_or_insert_result));
  high = mix_build_number(high, sizeof(mt_scan_bound));
  high = mix_build_number(high, alignof(mt_scan_bound));
  high = mix_build_number(high, sizeof(mt_scan_entry));
  high = mix_build_number(high, alignof(mt_scan_entry));
  high = mix_build_number(high, sizeof(mt_scan_result));
  high = mix_build_number(high, alignof(mt_scan_result));
  high = mix_build_number(high, sizeof(mt_abi_detail::record_tree));
  high = mix_build_number(high, kNativeMaxKeyLength);
  high = mix_build_number(high, kNativeMaxThreads);
  high = mix_build_number(high, kFeatures);
  high = mix_build_number(high, static_cast<uint64_t>(mt_endianness()));
  result.high = high;
  return result;
}

} // namespace

extern "C" uint32_t mt_abi_version(void) noexcept { return MT_ABI_VERSION; }

extern "C" mt_feature_set mt_feature_bits(void) noexcept { return kFeatures; }

extern "C" mt_byte_order mt_endianness(void) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return MT_BYTE_ORDER_LITTLE_ENDIAN;
  }
  if constexpr (std::endian::native == std::endian::big) {
    return MT_BYTE_ORDER_BIG_ENDIAN;
  }
  return MT_BYTE_ORDER_UNKNOWN;
}

extern "C" uint32_t mt_pointer_width(void) noexcept {
  return static_cast<uint32_t>(sizeof(void *) * CHAR_BIT);
}

extern "C" size_t mt_max_key_length(void) noexcept {
  return kNativeMaxKeyLength;
}

extern "C" uint32_t mt_max_threads(void) noexcept { return kNativeMaxThreads; }

extern "C" mt_record_id mt_record_id_limit(void) noexcept {
  return std::numeric_limits<mt_record_id>::max();
}

extern "C" size_t mt_runtime_config_size(void) noexcept {
  return sizeof(mt_runtime_config);
}

extern "C" size_t mt_runtime_config_alignment(void) noexcept {
  return alignof(mt_runtime_config);
}

extern "C" size_t mt_build_id_size(void) noexcept {
  return sizeof(mt_build_id);
}

extern "C" size_t mt_build_id_alignment(void) noexcept {
  return alignof(mt_build_id);
}

extern "C" size_t mt_read_scope_size(void) noexcept {
  return sizeof(mt_read_scope);
}

extern "C" size_t mt_read_scope_alignment(void) noexcept {
  return alignof(mt_read_scope);
}

extern "C" size_t mt_get_or_insert_result_size(void) noexcept {
  return sizeof(mt_get_or_insert_result);
}

extern "C" size_t mt_get_or_insert_result_alignment(void) noexcept {
  return alignof(mt_get_or_insert_result);
}

extern "C" size_t mt_scan_bound_size(void) noexcept {
  return sizeof(mt_scan_bound);
}

extern "C" size_t mt_scan_bound_alignment(void) noexcept {
  return alignof(mt_scan_bound);
}

extern "C" size_t mt_scan_entry_size(void) noexcept {
  return sizeof(mt_scan_entry);
}

extern "C" size_t mt_scan_entry_alignment(void) noexcept {
  return alignof(mt_scan_entry);
}

extern "C" size_t mt_scan_result_size(void) noexcept {
  return sizeof(mt_scan_result);
}

extern "C" size_t mt_scan_result_alignment(void) noexcept {
  return alignof(mt_scan_result);
}

extern "C" uint64_t mt_exported_symbols_fingerprint(void) noexcept {
  return kExportedSymbolsFingerprint;
}

extern "C" mt_status mt_get_build_fingerprint(mt_build_id *out) noexcept {
  return status_boundary([&]() -> mt_status {
    if (out == nullptr) {
      return MT_ERR_INVALID;
    }
    *out = build_id();
    return MT_OK;
  });
}

extern "C" mt_status mt_runtime_config_init(mt_runtime_config *out) noexcept {
  return status_boundary([&]() -> mt_status {
    if (out == nullptr) {
      return MT_ERR_INVALID;
    }
    *out = mt_runtime_config{};
    out->struct_size = sizeof(mt_runtime_config);
    out->abi_version = MT_ABI_VERSION;
    return MT_OK;
  });
}

extern "C" mt_status mt_runtime_acquire(const mt_runtime_config *config,
                                        mt_runtime **out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = nullptr;
  return status_boundary([&]() -> mt_status {
    normalized_config requested{};
    mt_status status = normalize_config(config, &requested);
    if (status != MT_OK) {
      return status;
    }

    mt_runtime &runtime = singleton_runtime();
    std::lock_guard<std::mutex> guard(runtime.mutex);
    if (runtime.acquired.load(std::memory_order_acquire)) {
      if (runtime.max_threads != requested.max_threads ||
          runtime.max_key_length != requested.max_key_length) {
        return MT_ERR_INCOMPATIBLE_RUNTIME;
      }
      *out = &runtime;
      return MT_OK;
    }

    /* Reserve before publishing the singleton configuration. */
    runtime.threads.reserve(requested.max_threads);
    SiloRuntime *native = SiloRuntime::GlobalDefault();
    if (native == nullptr) {
      return MT_ERR_INTERNAL;
    }
    runtime.native = native;
    runtime.max_threads = requested.max_threads;
    runtime.max_key_length = requested.max_key_length;
    runtime.health.store(MT_RUNTIME_HEALTHY, std::memory_order_release);
    runtime.acquired.store(true, std::memory_order_release);
    *out = &runtime;
    return MT_OK;
  });
}

extern "C" mt_status mt_runtime_health(const mt_runtime *runtime_candidate,
                                       mt_runtime_health_state *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = 0;
  return status_boundary([&]() -> mt_status {
    mt_runtime *runtime = nullptr;
    mt_status status = checked_runtime(runtime_candidate, &runtime);
    if (status != MT_OK) {
      return status;
    }
    *out = runtime->health.load(std::memory_order_acquire);
    return MT_OK;
  });
}

extern "C" mt_status
mt_runtime_max_key_length(const mt_runtime *runtime_candidate,
                          size_t *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = 0;
  return status_boundary([&]() -> mt_status {
    mt_runtime *runtime = nullptr;
    mt_status status = checked_runtime(runtime_candidate, &runtime);
    if (status != MT_OK) {
      return status;
    }
    *out = runtime->max_key_length;
    return MT_OK;
  });
}

extern "C" mt_status mt_runtime_max_threads(const mt_runtime *runtime_candidate,
                                            uint32_t *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = 0;
  return status_boundary([&]() -> mt_status {
    mt_runtime *runtime = nullptr;
    mt_status status = checked_runtime(runtime_candidate, &runtime);
    if (status != MT_OK) {
      return status;
    }
    *out = runtime->max_threads;
    return MT_OK;
  });
}

extern "C" mt_status mt_thread_attach(mt_runtime *runtime_candidate,
                                      mt_thread **out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = nullptr;
  return status_boundary([&]() -> mt_status {
    mt_runtime *runtime = nullptr;
    mt_status status = checked_runtime(runtime_candidate, &runtime);
    if (status != MT_OK) {
      return status;
    }
    if (runtime->health.load(std::memory_order_acquire) != MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }
    if (tls_read_scope.active() || tls_rcu_scope.active()) {
      return MT_ERR_ACTIVE_GUARDS;
    }
    if (SiloRuntime::Current() != runtime->native) {
      return MT_ERR_WRONG_RUNTIME;
    }
    if (tls_thread != nullptr) {
      if (tls_thread->runtime != runtime) {
        return MT_ERR_WRONG_RUNTIME;
      }
      if (tls_thread->owner != std::this_thread::get_id()) {
        return MT_ERR_WRONG_THREAD;
      }
      *out = tls_thread;
      return MT_OK;
    }

    auto pending =
        std::make_unique<mt_thread>(runtime, std::this_thread::get_id(), -1);
    std::lock_guard<std::mutex> guard(runtime->mutex);
    if (runtime->threads.size() >= runtime->max_threads) {
      return MT_ERR_THREAD_LIMIT;
    }
    /*
     * Current() falls back to GlobalDefault() on an unbound thread, so the
     * equality check above cannot distinguish "fresh" from "already bound".
     * Bind explicitly before registration to select this runtime's Masstree
     * context. A genuinely foreign binding was rejected above.
     */
    runtime->native->BindToCurrentThread();
    if (!runtime->native->try_register_current_thread()) {
      return MT_ERR_THREAD_LIMIT;
    }
    const int native_core_id = coreid::try_current_core_id();
    if (native_core_id < 0 || native_core_id >= static_cast<int>(NMAXCORES)) {
      return MT_ERR_INTERNAL;
    }
    pending->native_core_id = native_core_id;
    runtime->threads.push_back(pending.get());
    pending->registry_next =
        runtime->thread_registry.load(std::memory_order_relaxed);
    /* Publish the initialized handle and immutable registry link for lifetime
     * validation of untrusted pointers. */
    runtime->thread_registry.store(pending.get(), std::memory_order_release);
    tls_thread = pending.release();
    *out = tls_thread;
    return MT_OK;
  });
}

extern "C" mt_status mt_thread_quiesce(mt_thread *thread) noexcept {
  return status_boundary([&]() -> mt_status {
    if (thread == nullptr) {
      return MT_ERR_NOT_ATTACHED;
    }
    mt_runtime &singleton = singleton_runtime();
    if (!singleton.acquired.load(std::memory_order_acquire) ||
        (thread != tls_thread && !registered_thread(singleton, thread))) {
      return MT_ERR_INVALID;
    }
    /* Dereference only after registry membership proves this is our handle. */
    mt_runtime *runtime = thread->runtime;
    mt_status status = checked_thread(runtime, thread, true);
    if (status != MT_OK) {
      return status;
    }
    {
      scoped_rcu_region region;
    }
    return MT_OK;
  });
}

extern "C" mt_status mt_tree_create(mt_runtime *runtime_candidate,
                                    mt_thread *thread, mt_tree **out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = nullptr;
  return status_boundary([&]() -> mt_status {
    mt_runtime *runtime = nullptr;
    mt_status status = checked_runtime(runtime_candidate, &runtime);
    if (status != MT_OK) {
      return status;
    }
    status = checked_thread(runtime, thread, true);
    if (status != MT_OK) {
      return status;
    }
    if (runtime->health.load(std::memory_order_acquire) != MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }

    std::unique_ptr<mt_tree> pending;
    {
      scoped_rcu_region region;
      pending = std::make_unique<mt_tree>(runtime);
    }
    try {
      std::lock_guard<std::mutex> guard(runtime->mutex);
      runtime->trees.push_back(pending.get());
      pending->registry_next =
          runtime->tree_registry.load(std::memory_order_relaxed);
      runtime->tree_registry.store(pending.get(), std::memory_order_release);
    } catch (...) {
      /* The unpublished tree is destroyed on this attached worker. */
      scoped_rcu_region region;
      pending.reset();
      throw;
    }
    *out = pending.release();
    return MT_OK;
  });
}

extern "C" mt_status mt_tree_release(mt_tree *tree_candidate) noexcept {
  return status_boundary([&]() -> mt_status {
    mt_tree *tree = nullptr;
    mt_status status = checked_tree(tree_candidate, &tree);
    if (status != MT_OK) {
      return status;
    }
    if (tls_read_scope.active() && tls_read_scope.tree() == tree) {
      return MT_ERR_ACTIVE_GUARDS;
    }
    bool expected = true;
    if (!tree->open.compare_exchange_strong(expected, false,
                                            std::memory_order_acq_rel)) {
      return MT_ERR_CLOSED;
    }
    return MT_OK;
  });
}

extern "C" mt_status
mt_tree_seal_structure(mt_tree *tree_candidate) noexcept {
  mt_runtime *operation_runtime = nullptr;
  try {
    mt_tree *tree = nullptr;
    mt_status status = checked_tree(tree_candidate, &tree);
    if (status != MT_OK) {
      return status;
    }
    operation_runtime = tree->runtime;
    if (operation_runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }
    if (tls_read_scope.active() && tls_read_scope.tree() == tree) {
      return MT_ERR_ACTIVE_GUARDS;
    }

    structure_write_guard structure_guard(*tree);
    if (operation_runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }
    if (structure_guard.sealed()) {
      return MT_OK;
    }

    structure_guard.synchronize_for_seal();
    /* A draining reader may have poisoned this runtime before it left. */
    if (operation_runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }
    structure_guard.publish_seal();
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status mt_runtime_shutdown(mt_runtime *runtime_candidate,
                                         mt_thread *shutdown_thread) noexcept {
  return status_boundary([&]() -> mt_status {
    mt_runtime *runtime = nullptr;
    mt_status status = checked_runtime(runtime_candidate, &runtime);
    if (status != MT_OK) {
      return status;
    }
    status = checked_thread(runtime, shutdown_thread, true);
    if (status != MT_OK) {
      return status;
    }
    return MT_ERR_UNSUPPORTED;
  });
}

extern "C" mt_status mt_get(mt_tree *tree_candidate, mt_thread *thread,
                            const void *key, size_t key_length,
                            mt_record_id *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = MT_RECORD_ID_NONE;
  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  try {
    mt_tree *tree = nullptr;
    mt_status status = checked_operation(tree_candidate, thread, &tree,
                                         &operation_runtime, true,
                                         controlled_rcu);
    if (status != MT_OK) {
      return status;
    }
    status = check_key(*operation_runtime, key, key_length);
    if (status != MT_OK) {
      return status;
    }
    return point_get_validated(*tree, *thread, operation_runtime,
                               controlled_rcu, key, key_length, out);
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status mako_mtree_get_trusted(mt_tree *tree, mt_thread *thread,
                                            const void *key,
                                            size_t key_length,
                                            mt_record_id *out) noexcept {
  *out = MT_RECORD_ID_NONE;
  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  const mt_status status = trusted_operation(*tree, *thread,
                                             &operation_runtime,
                                             controlled_rcu);
  if (status != MT_OK) {
    return status;
  }
  return point_get_validated(*tree, *thread, operation_runtime,
                             controlled_rcu, key, key_length, out);
}

extern "C" mt_status
mako_mtree_get_strided_trusted(mt_tree *tree, mt_thread *thread,
                               const void *keys, size_t key_count,
                               size_t key_length, mt_record_id *out) noexcept {
  INVARIANT(key_count == 0 || out != nullptr);
  INVARIANT(key_count <=
            std::numeric_limits<size_t>::max() / sizeof(mt_record_id));
  clear_strided_output(key_count, out);

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  const mt_status status =
      trusted_operation(*tree, *thread, &operation_runtime, controlled_rcu);
  if (status != MT_OK || key_count == 0) {
    return status;
  }

  INVARIANT(key_length <= operation_runtime->max_key_length);
  INVARIANT(key_length <= MT_CONFIGURED_MAX_KEY_LENGTH);
  INVARIANT(key_length <= static_cast<size_t>(INT_MAX));
  INVARIANT(key_length == 0 || keys != nullptr);
  INVARIANT(key_length == 0 ||
            key_count <= std::numeric_limits<size_t>::max() / key_length);
  return point_get_strided_validated(*tree, *thread, operation_runtime,
                                     controlled_rcu,
                                     static_cast<const uint8_t *>(keys),
                                     key_count, key_length, key_length, out);
}

extern "C" mt_status mt_get_strided(mt_tree *tree_candidate, mt_thread *thread,
                                    const void *keys, size_t key_count,
                                    size_t key_length, size_t key_stride,
                                    mt_record_id *out) noexcept {
  mt_status status = initialize_strided_output(key_count, out);
  if (status != MT_OK) {
    return status;
  }

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  try {
    mt_tree *tree = nullptr;
    status = checked_operation(tree_candidate, thread, &tree,
                               &operation_runtime, true, controlled_rcu);
    if (status != MT_OK) {
      return status;
    }

    const uint8_t *cursor = nullptr;
    status = check_strided_keys(*operation_runtime, keys, key_count, key_length,
                                key_stride, &cursor);
    if (status != MT_OK || key_count == 0) {
      return status;
    }
    return point_get_strided_validated(*tree, *thread, operation_runtime,
                                       controlled_rcu, cursor, key_count,
                                       key_length, key_stride, out);
  } catch (const std::bad_alloc &) {
    clear_strided_output(key_count, out);
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    clear_strided_output(key_count, out);
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status mt_read_scope_begin(mt_tree *tree_candidate,
                                         mt_thread *thread,
                                         mt_read_scope *token) noexcept {
  if (token == nullptr) {
    return MT_ERR_INVALID;
  }
  *token = mt_read_scope{};
  if (tls_read_scope.active() || tls_rcu_scope.active()) {
    return MT_ERR_ACTIVE_GUARDS;
  }

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  try {
    mt_tree *tree = nullptr;
    mt_status status = checked_operation(tree_candidate, thread, &tree,
                                         &operation_runtime, false,
                                         controlled_rcu);
    if (status != MT_OK) {
      return status;
    }
    INVARIANT(!controlled_rcu);
    if (!tls_read_scope.can_advance_generation()) {
      return MT_ERR_INTERNAL;
    }

    tls_read_scope.admit(*tree, *thread);
    /* A writer may have poisoned the runtime while this scope waited. */
    if (operation_runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      tls_read_scope.close();
      return MT_ERR_POISONED;
    }
    tls_read_scope.enter_rcu_and_activate();
    token->owner = tls_read_scope.owner_identity();
    token->generation = tls_read_scope.generation();
    return MT_OK;
  } catch (const std::bad_alloc &) {
    tls_read_scope.close();
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    /* Publish poison before releasing this scope's structural admission. */
    poison(operation_runtime);
    tls_read_scope.close();
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status mt_read_scope_get(const mt_read_scope *token,
                                       const void *key, size_t key_length,
                                       mt_record_id *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  *out = MT_RECORD_ID_NONE;
  if (token == nullptr || !tls_read_scope.matches(*token)) {
    return MT_ERR_INVALID;
  }

  mt_runtime *operation_runtime = tls_read_scope.runtime();
  try {
    mt_tree *tree = tls_read_scope.tree();
    mt_thread *thread = tls_read_scope.thread();
    if (tree == nullptr || thread == nullptr || operation_runtime == nullptr) {
      poison(operation_runtime);
      return MT_ERR_INTERNAL;
    }
    /* The token's TLS identity proves affinity; retain a cheap local sanity
     * check without repeating registry/runtime/core validation. */
    if (thread != tls_thread) {
      return MT_ERR_NOT_ATTACHED;
    }
    if (!tree->open.load(std::memory_order_acquire)) {
      return MT_ERR_CLOSED;
    }
    if (operation_runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }
    mt_status status = check_key(*operation_runtime, key, key_length);
    if (status != MT_OK) {
      return status;
    }

    const varkey native_key = make_key(key, key_length);
    mt_record_id value = MT_RECORD_ID_NONE;
    if (tree->native.search(native_key, value)) {
      if (value == MT_RECORD_ID_NONE) {
        /* A queued structural writer cannot pass until end observes this. */
        poison(operation_runtime);
        return MT_ERR_INTERNAL;
      }
      *out = value;
    }
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    /* Keep structural admission until the caller ends the poisoned scope. */
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status
mt_read_scope_get_strided(const mt_read_scope *token, const void *keys,
                          size_t key_count, size_t key_length,
                          size_t key_stride, mt_record_id *out) noexcept {
  mt_status status = initialize_strided_output(key_count, out);
  if (status != MT_OK) {
    return status;
  }
  const auto clear_output = [&]() noexcept {
    clear_strided_output(key_count, out);
  };

  if (token == nullptr || !tls_read_scope.matches(*token)) {
    return MT_ERR_INVALID;
  }

  mt_runtime *operation_runtime = tls_read_scope.runtime();
  try {
    mt_tree *tree = tls_read_scope.tree();
    mt_thread *thread = tls_read_scope.thread();
    if (tree == nullptr || thread == nullptr || operation_runtime == nullptr) {
      poison(operation_runtime);
      return MT_ERR_INTERNAL;
    }
    /* Match mt_read_scope_get, but pay these capability and health checks once
     * for the entire fixed-shape batch. */
    if (thread != tls_thread) {
      return MT_ERR_NOT_ATTACHED;
    }
    if (!tree->open.load(std::memory_order_acquire)) {
      return MT_ERR_CLOSED;
    }
    if (operation_runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }

    const uint8_t *cursor = nullptr;
    status = check_strided_keys(*operation_runtime, keys, key_count, key_length,
                                key_stride, &cursor);
    if (status != MT_OK) {
      return status;
    }
    if (key_count == 0) {
      return MT_OK;
    }

    status = search_strided(*tree, operation_runtime, cursor, key_count,
                            key_length, key_stride, out);
    if (status != MT_OK) {
      clear_output();
      return status;
    }
    return MT_OK;
  } catch (const std::bad_alloc &) {
    clear_output();
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    clear_output();
    /* Keep structural admission until the caller ends the poisoned scope. */
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status mt_read_scope_end(mt_read_scope *token) noexcept {
  if (token == nullptr || !tls_read_scope.matches(*token)) {
    return MT_ERR_INVALID;
  }
  tls_read_scope.close();
  *token = mt_read_scope{};
  return MT_OK;
}

extern "C" mt_status mt_rcu_scope_begin(mt_thread *thread,
                                         mt_rcu_scope *token) noexcept {
  if (token == nullptr) {
    return MT_ERR_INVALID;
  }
  *token = mt_rcu_scope{};
  if (tls_read_scope.active() || tls_rcu_scope.active()) {
    return MT_ERR_ACTIVE_GUARDS;
  }

  return status_boundary([&]() -> mt_status {
    if (thread == nullptr) {
      return MT_ERR_NOT_ATTACHED;
    }
    mt_runtime &singleton = singleton_runtime();
    if (!singleton.acquired.load(std::memory_order_acquire) ||
        (thread != tls_thread && !registered_thread(singleton, thread))) {
      return MT_ERR_INVALID;
    }
    /* Dereference only after registry membership proves this handle. */
    mt_runtime *runtime = thread->runtime;
    mt_status status = checked_thread(runtime, thread, true);
    if (status != MT_OK) {
      return status;
    }
    if (runtime->health.load(std::memory_order_acquire) !=
        MT_RUNTIME_HEALTHY) {
      return MT_ERR_POISONED;
    }
    if (!tls_rcu_scope.can_advance_generation()) {
      return MT_ERR_INTERNAL;
    }

    try {
      tls_rcu_scope.enter(*runtime, *thread);
    } catch (...) {
      tls_rcu_scope.close();
      throw;
    }
    token->owner = tls_rcu_scope.owner_identity();
    token->generation = tls_rcu_scope.generation();
    return MT_OK;
  });
}

extern "C" mt_status mt_rcu_scope_end(mt_rcu_scope *token) noexcept {
  if (token == nullptr || !tls_rcu_scope.matches(*token)) {
    return MT_ERR_INVALID;
  }
  tls_rcu_scope.close();
  *token = mt_rcu_scope{};
  return MT_OK;
}

extern "C" mt_status mt_get_or_insert(mt_tree *tree_candidate,
                                      mt_thread *thread, const void *key,
                                      size_t key_length, mt_record_id candidate,
                                      mt_get_or_insert_result *out) noexcept {
  if (out == nullptr) {
    return MT_ERR_INVALID;
  }
  initialize_insert_result(out);

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  try {
    mt_tree *tree = nullptr;
    mt_status status = checked_operation(tree_candidate, thread, &tree,
                                         &operation_runtime, true,
                                         controlled_rcu);
    if (status != MT_OK) {
      return status;
    }
    status = check_key(*operation_runtime, key, key_length);
    if (status != MT_OK) {
      return status;
    }
    if (candidate == MT_RECORD_ID_NONE) {
      return MT_ERR_INVALID;
    }
    return get_or_insert_validated(*tree, *thread, operation_runtime,
                                   controlled_rcu, key, key_length,
                                   candidate, out);
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

extern "C" mt_status mako_mtree_get_or_insert_trusted(
    mt_tree *tree, mt_thread *thread, const void *key, size_t key_length,
    mt_record_id candidate, mt_get_or_insert_result *out) noexcept {
  initialize_insert_result(out);
  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  const mt_status status = trusted_operation(*tree, *thread,
                                             &operation_runtime,
                                             controlled_rcu);
  if (status != MT_OK) {
    return status;
  }
  return get_or_insert_validated(*tree, *thread, operation_runtime,
                                 controlled_rcu, key, key_length, candidate,
                                 out);
}

extern "C" mt_status mako_mtree_get_or_insert_strided_trusted(
    mt_tree *tree, mt_thread *thread, const void *keys, size_t key_count,
    size_t key_length, size_t key_stride, const mt_record_id *candidates,
    mt_get_or_insert_result *out) noexcept {
  initialize_insert_results(key_count, out);
  if (key_count == 0) {
    return MT_OK;
  }

  /* These checks diagnose violations of the private unsafe-call contract. */
  INVARIANT(keys != nullptr);
  INVARIANT(key_stride >= key_length);
  INVARIANT(candidates != nullptr);
  INVARIANT(out != nullptr);
  for (size_t index = 0; index != key_count; ++index) {
    INVARIANT(candidates[index] != MT_RECORD_ID_NONE);
  }

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  const mt_status status = trusted_operation(*tree, *thread,
                                             &operation_runtime,
                                             controlled_rcu);
  if (status != MT_OK) {
    return status;
  }
  return get_or_insert_strided_trusted_validated(
      *tree, *thread, operation_runtime, controlled_rcu,
      static_cast<const uint8_t *>(keys), key_count, key_length, key_stride,
      candidates, out);
}

extern "C" mt_status
mt_scan(mt_tree *tree_candidate, mt_thread *thread, mt_scan_direction direction,
        const mt_scan_bound *lower, const mt_scan_bound *upper,
        mt_scan_entry *entries, size_t entry_capacity, void *key_arena,
        size_t key_arena_capacity, mt_scan_result *out) noexcept {
  if (out == nullptr || (entries == nullptr && entry_capacity != 0) ||
      (key_arena == nullptr && key_arena_capacity != 0)) {
    return MT_ERR_INVALID;
  }
  initialize_scan_result(out);

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  try {
    mt_tree *tree = nullptr;
    mt_status status = checked_operation(tree_candidate, thread, &tree,
                                         &operation_runtime, true,
                                         controlled_rcu);
    if (status != MT_OK) {
      return status;
    }
    if (direction != MT_SCAN_FORWARD && direction != MT_SCAN_REVERSE) {
      return MT_ERR_INVALID;
    }
    if (lower == nullptr || upper == nullptr) {
      return MT_ERR_INVALID;
    }
    status = check_scan_bound(*operation_runtime, *lower);
    if (status != MT_OK) {
      return status;
    }
    status = check_scan_bound(*operation_runtime, *upper);
    if (status != MT_OK) {
      return status;
    }
    return copied_scan_validated(
        *tree, *thread, operation_runtime, controlled_rcu, direction, *lower,
        *upper, entries, entry_capacity, static_cast<uint8_t *>(key_arena),
        key_arena_capacity, out);
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}

#if defined(__GNUC__) || defined(__clang__)
#define MAKO_MTREE_PRIVATE_HIDDEN __attribute__((visibility("hidden")))
#else
#define MAKO_MTREE_PRIVATE_HIDDEN
#endif
extern "C" MAKO_MTREE_PRIVATE_HIDDEN mt_status mako_mtree_scan_trusted(
    mt_tree *tree, mt_thread *thread, mt_scan_direction direction,
    const mt_scan_bound *lower, const mt_scan_bound *upper,
    mt_scan_entry *entries, size_t entry_capacity, void *key_arena,
    size_t key_arena_capacity, mt_scan_result *out) noexcept {
  initialize_scan_result(out);
  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  const mt_status status = trusted_operation(*tree, *thread,
                                             &operation_runtime,
                                             controlled_rcu);
  if (status != MT_OK) {
    return status;
  }
  return copied_scan_validated(
      *tree, *thread, operation_runtime, controlled_rcu, direction, *lower,
      *upper, entries, entry_capacity, static_cast<uint8_t *>(key_arena),
      key_arena_capacity, out);
}

extern "C" MAKO_MTREE_PRIVATE_HIDDEN mt_status
mako_mtree_scan_record_ids_bounded_trusted(
    mt_tree *tree, mt_thread *thread, const void *lower, size_t lower_length,
    const void *upper, size_t upper_length, mt_record_id *records,
    size_t record_capacity, void *continuation,
    size_t continuation_capacity,
    trusted_record_id_scan_result *out) noexcept {
  initialize_trusted_record_id_scan_result(out);

  INVARIANT(lower != nullptr || lower_length == 0);
  INVARIANT(upper != nullptr || upper_length == 0);
  INVARIANT(records != nullptr || record_capacity == 0);
  INVARIANT(continuation != nullptr || continuation_capacity == 0);

  mt_runtime *operation_runtime = nullptr;
  bool controlled_rcu = false;
  const mt_status status = trusted_operation(*tree, *thread,
                                             &operation_runtime,
                                             controlled_rcu);
  if (status != MT_OK) {
    return status;
  }
  INVARIANT(lower_length <= operation_runtime->max_key_length);
  INVARIANT(upper_length <= operation_runtime->max_key_length);
  return trusted_record_id_scan_validated(
      *tree, *thread, operation_runtime, controlled_rcu, lower, lower_length,
      upper, upper_length, records, record_capacity,
      static_cast<uint8_t *>(continuation), continuation_capacity, out);
}
#undef MAKO_MTREE_PRIVATE_HIDDEN
