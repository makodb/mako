// mtree_abi.cc - hardened C boundary for an integral-valued Masstree.

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
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
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
static_assert(MASSTREE_MAXKEYLEN >= MT_CONFIGURED_MAX_KEY_LENGTH,
              "the native Masstree key limit is below the ABI contract");
static_assert(NMAXCORES <= std::numeric_limits<uint32_t>::max());

} // namespace mt_abi_detail

/* These definitions complete the opaque C tags from mtree_abi.h. */
struct mt_runtime {
  mutable std::mutex mutex;
  bool acquired = false;
  SiloRuntime *native = nullptr;
  uint32_t max_threads = 0;
  uint32_t max_key_length = 0;
  std::atomic<mt_runtime_health_state> health{MT_RUNTIME_HEALTHY};
  std::vector<mt_thread *> threads;
  std::vector<mt_tree *> trees;
};

struct mt_thread {
  mt_runtime *runtime;
  std::thread::id owner;
  int native_core_id;

  mt_thread(mt_runtime *runtime_arg, std::thread::id owner_arg,
            int native_core_id_arg) noexcept
      : runtime(runtime_arg), owner(owner_arg),
        native_core_id(native_core_id_arg) {}
};

struct mt_tree {
  mt_runtime *runtime;
  std::atomic<bool> open;
  /*
   * The inherited leaf-link implementation mixes raw reads with custom CAS
   * during splits. Keep that implementation behind a C++ synchronization
   * boundary: point lookups and scans share structural access, while a
   * possible insertion owns it exclusively.
   */
  mutable std::shared_mutex structure_mutex;
  mt_abi_detail::record_tree native;

  explicit mt_tree(mt_runtime *runtime_arg)
      : runtime(runtime_arg), open(true), native() {}
};

namespace {

constexpr uint32_t kNativeMaxThreads = static_cast<uint32_t>(NMAXCORES);
constexpr uint32_t kNativeMaxKeyLength =
    static_cast<uint32_t>(MT_CONFIGURED_MAX_KEY_LENGTH);
constexpr mt_feature_set kFeatures =
    MT_FEATURE_POINT_GET | MT_FEATURE_ATOMIC_GET_OR_INSERT |
    MT_FEATURE_EXPLICIT_HANDLES | MT_FEATURE_BINARY_KEYS |
    MT_FEATURE_INTEGRAL_RECORD_IDS | MT_FEATURE_RUNTIME_HEALTH |
    MT_FEATURE_SINGLETON_RUNTIME | MT_FEATURE_COPIED_RANGE_SCANS;

/* Deliberately excludes MT_FEATURE_GRACEFUL_SHUTDOWN. */
static_assert((kFeatures & MT_FEATURE_GRACEFUL_SHUTDOWN) == 0);

thread_local mt_thread *tls_thread = nullptr;

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
  std::lock_guard<std::mutex> guard(runtime.mutex);
  if (!runtime.acquired) {
    return MT_ERR_INVALID;
  }
  *out = &runtime;
  return MT_OK;
}

bool registered_thread_locked(const mt_runtime &runtime,
                              const mt_thread *candidate) noexcept {
  return std::find(runtime.threads.begin(), runtime.threads.end(), candidate) !=
         runtime.threads.end();
}

bool registered_tree_locked(const mt_runtime &runtime,
                            const mt_tree *candidate) noexcept {
  return std::find(runtime.trees.begin(), runtime.trees.end(), candidate) !=
         runtime.trees.end();
}

mt_status checked_thread(mt_runtime *expected_runtime, mt_thread *candidate,
                         bool reject_active_rcu) {
  if (candidate == nullptr) {
    return MT_ERR_NOT_ATTACHED;
  }
  {
    std::lock_guard<std::mutex> guard(expected_runtime->mutex);
    if (!registered_thread_locked(*expected_runtime, candidate)) {
      return MT_ERR_INVALID;
    }
  }
  if (candidate->owner != std::this_thread::get_id()) {
    return MT_ERR_WRONG_THREAD;
  }
  if (tls_thread == nullptr || candidate != tls_thread) {
    return MT_ERR_NOT_ATTACHED;
  }
  if (candidate->runtime != expected_runtime ||
      SiloRuntime::Current() != expected_runtime->native) {
    return MT_ERR_WRONG_RUNTIME;
  }
  if (coreid::try_current_core_id() != candidate->native_core_id) {
    return MT_ERR_NOT_ATTACHED;
  }
  if (reject_active_rcu && rcu::s_instance.in_rcu_region()) {
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
  {
    std::lock_guard<std::mutex> guard(runtime.mutex);
    if (!runtime.acquired || !registered_tree_locked(runtime, candidate)) {
      return MT_ERR_INVALID;
    }
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
                            mt_runtime **runtime_out) {
  if (tree_out == nullptr || runtime_out == nullptr) {
    return MT_ERR_INVALID;
  }
  *tree_out = nullptr;
  *runtime_out = nullptr;

  mt_tree *tree = nullptr;
  mt_status status = checked_tree(tree_candidate, &tree);
  if (status != MT_OK) {
    return status;
  }
  mt_runtime *runtime = tree->runtime;
  status = checked_thread(runtime, thread_candidate, true);
  if (status != MT_OK) {
    return status;
  }
  if (runtime->health.load(std::memory_order_acquire) != MT_RUNTIME_HEALTHY) {
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

void initialize_scan_result(mt_scan_result *out) noexcept {
  *out = mt_scan_result{};
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
    "mt_build_id_alignment;mt_get_or_insert_result_size;"
    "mt_get_or_insert_result_alignment;mt_scan_bound_size;"
    "mt_scan_bound_alignment;mt_scan_entry_size;mt_scan_entry_alignment;"
    "mt_scan_result_size;mt_scan_result_alignment;"
    "mt_exported_symbols_fingerprint;"
    "mt_get_build_fingerprint;mt_runtime_config_init;mt_runtime_acquire;"
    "mt_runtime_health;mt_runtime_max_key_length;mt_runtime_max_threads;"
    "mt_runtime_shutdown;mt_thread_attach;mt_thread_quiesce;mt_tree_create;"
    "mt_tree_release;mt_get;mt_get_or_insert;mt_scan";

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
    if (runtime.acquired) {
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
    runtime.acquired = true;
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
    if (native_core_id < 0) {
      return MT_ERR_INTERNAL;
    }
    pending->native_core_id = native_core_id;
    runtime->threads.push_back(pending.get());
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
    {
      std::lock_guard<std::mutex> guard(singleton.mutex);
      if (!singleton.acquired || !registered_thread_locked(singleton, thread)) {
        return MT_ERR_INVALID;
      }
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
    bool expected = true;
    if (!tree->open.compare_exchange_strong(expected, false,
                                            std::memory_order_acq_rel)) {
      return MT_ERR_CLOSED;
    }
    return MT_OK;
  });
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
  try {
    mt_tree *tree = nullptr;
    mt_status status =
        checked_operation(tree_candidate, thread, &tree, &operation_runtime);
    if (status != MT_OK) {
      return status;
    }
    status = check_key(*operation_runtime, key, key_length);
    if (status != MT_OK) {
      return status;
    }

    mt_record_id found = MT_RECORD_ID_NONE;
    {
      std::shared_lock<std::shared_mutex> structure_guard(
          tree->structure_mutex);
      scoped_rcu_region region;
      const varkey native_key = make_key(key, key_length);
      mt_record_id value = MT_RECORD_ID_NONE;
      if (tree->native.search(native_key, value)) {
        if (value == MT_RECORD_ID_NONE) {
          poison(operation_runtime);
          return MT_ERR_INTERNAL;
        }
        found = value;
      }
    }
    *out = found;
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
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
  bool publication_attempted = false;
  bool publication_classified = false;
  try {
    mt_tree *tree = nullptr;
    mt_status status =
        checked_operation(tree_candidate, thread, &tree, &operation_runtime);
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

    {
      std::unique_lock<std::shared_mutex> structure_guard(
          tree->structure_mutex);
      scoped_rcu_region region;
      const varkey native_key = make_key(key, key_length);
      mt_record_id winner = MT_RECORD_ID_NONE;

      /* Existing keys dominate this workload, so avoid taking a write lock. */
      if (tree->native.search(native_key, winner)) {
        out->publication = MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED;
        publication_classified = true;
        if (winner == MT_RECORD_ID_NONE) {
          poison(operation_runtime);
          return MT_ERR_INTERNAL;
        }
        out->winner = winner;
      } else {
        publication_attempted = true;
        const bool inserted =
            tree->native.insert_if_absent(native_key, candidate);
        if (inserted) {
          out->winner = candidate;
          out->inserted = 1;
          out->publication = MT_PUBLICATION_CANDIDATE_INSERTED;
          publication_classified = true;
        } else {
          /* A false return positively proves this candidate was not stored. */
          out->publication = MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED;
          publication_classified = true;
          if (!tree->native.search(native_key, winner) ||
              winner == MT_RECORD_ID_NONE) {
            poison(operation_runtime);
            return MT_ERR_INTERNAL;
          }
          out->winner = winner;
        }
      }
    }
    return MT_OK;
  } catch (const std::bad_alloc &) {
    if (!publication_classified) {
      out->winner = MT_RECORD_ID_NONE;
      out->inserted = 0;
      out->publication = publication_attempted
                             ? MT_PUBLICATION_UNKNOWN
                             : MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION;
    }
    if (out->publication == MT_PUBLICATION_UNKNOWN) {
      poison(operation_runtime);
    }
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    if (!publication_classified) {
      out->winner = MT_RECORD_ID_NONE;
      out->inserted = 0;
      out->publication = publication_attempted
                             ? MT_PUBLICATION_UNKNOWN
                             : MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION;
    }
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
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
  try {
    mt_tree *tree = nullptr;
    mt_status status =
        checked_operation(tree_candidate, thread, &tree, &operation_runtime);
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
    if (bound_is_present(*lower) && bound_is_present(*upper)) {
      const int ordering = compare_byte_strings(lower->key, lower->key_length,
                                                upper->key, upper->key_length);
      if (ordering > 0 ||
          (ordering == 0 && (lower->kind == MT_SCAN_BOUND_EXCLUSIVE ||
                             upper->kind == MT_SCAN_BOUND_EXCLUSIVE))) {
        return MT_OK;
      }
    }

    copied_scan_collector collector(
        direction, *lower, *upper, entries, entry_capacity,
        static_cast<uint8_t *>(key_arena), key_arena_capacity);
    {
      std::shared_lock<std::shared_mutex> structure_guard(
          tree->structure_mutex);
      scoped_rcu_region region;
      if (direction == MT_SCAN_FORWARD) {
        const varkey native_lower =
            bound_is_present(*lower) ? make_key(lower->key, lower->key_length)
                                     : make_key(nullptr, 0);
        if (upper->kind == MT_SCAN_BOUND_EXCLUSIVE) {
          const varkey native_upper = make_key(upper->key, upper->key_length);
          tree->native.search_range_call_bounded(native_lower, native_upper,
                                                 collector);
        } else {
          tree->native.search_range_call_unbounded(native_lower, collector);
        }
      } else {
        std::array<uint8_t, MT_CONFIGURED_MAX_KEY_LENGTH> maximum_key{};
        maximum_key.fill(UINT8_MAX);
        const varkey native_upper =
            bound_is_present(*upper)
                ? make_key(upper->key, upper->key_length)
                : make_key(maximum_key.data(), maximum_key.size());
        if (lower->kind == MT_SCAN_BOUND_EXCLUSIVE) {
          const varkey native_lower = make_key(lower->key, lower->key_length);
          tree->native.rsearch_range_call_bounded(native_upper, native_lower,
                                                  collector);
        } else {
          tree->native.rsearch_range_call_unbounded(native_upper, collector);
        }
      }
    }

    if (collector.invalid_native_entry()) {
      poison(operation_runtime);
      return MT_ERR_INTERNAL;
    }
    collector.publish(out);
    return MT_OK;
  } catch (const std::bad_alloc &) {
    return MT_ERR_OUT_OF_MEMORY;
  } catch (...) {
    poison(operation_runtime);
    return MT_ERR_CPP_EXCEPTION;
  }
}
