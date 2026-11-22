#include "rust_sto_tpcc_wrapper.hh"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmarks/benchmark_config.h"
#include "benchmarks/benchmark_output.h"
#include "lib/common.h"
#include "rcu.h"
#include "mbta_wrapper.hh"
#include "mtree_abi.h"
#include <rusty/mutex.hpp>
#include "deptran/replication_helper.h"
#include "rocksdb_persistence_fwd.h"

// Wrapper-private fast lane implemented by sto-tpcc-ffi. These symbols stay
// out of the installed C header: this wrapper establishes their live-handle,
// affinity, active-transaction, table-ownership, and range preconditions.
extern "C" sto_tpcc_status mako_sto_tpcc_get_trusted(
    sto_tpcc_thread *thread, const sto_tpcc_table *table, const uint8_t *key,
    size_t key_length, uint8_t *out_value, size_t value_capacity,
    size_t *out_actual) noexcept;
extern "C" sto_tpcc_status
mako_sto_tpcc_txn_begin_trusted(sto_tpcc_thread *thread) noexcept;
extern "C" sto_tpcc_status
mako_sto_tpcc_txn_commit_trusted(sto_tpcc_thread *thread) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_put_trusted(
    sto_tpcc_thread *thread, const sto_tpcc_table *table, const uint8_t *key,
    size_t key_length, const uint8_t *value, size_t value_length) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_insert_trusted(
    sto_tpcc_thread *thread, const sto_tpcc_table *table, const uint8_t *key,
    size_t key_length, const uint8_t *value, size_t value_length) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_put_borrowed_trusted(
    sto_tpcc_thread *thread, const sto_tpcc_table *table, const uint8_t *key,
    size_t key_length, const uint8_t *value, size_t value_length) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_insert_borrowed_trusted(
    sto_tpcc_thread *thread, const sto_tpcc_table *table, const uint8_t *key,
    size_t key_length, const uint8_t *value, size_t value_length) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_scan_trusted(
    sto_tpcc_thread *thread, const sto_tpcc_table *table,
    sto_tpcc_scan_direction direction, sto_tpcc_bound_kind lower_kind,
    const uint8_t *lower_key, size_t lower_key_length,
    sto_tpcc_bound_kind upper_kind, const uint8_t *upper_key,
    size_t upper_key_length, size_t limit, sto_tpcc_scan_callback callback,
    void *callback_context, size_t *out_visited) noexcept;

// Private ABI for the callback-free TPC-C Payment prefix. The public FFI
// remains workload-neutral; this bridge is linked only into the matched
// comparison binary.
struct mako_sto_tpcc_payment_prefix_request {
  const sto_tpcc_table *warehouse;
  const sto_tpcc_table *district;
  const sto_tpcc_table *customer;
  const sto_tpcc_table *customer_name;
  const uint8_t *warehouse_key;
  const uint8_t *district_key;
  const uint8_t *customer_key_prefix;
  const uint8_t *customer_name_lower;
  const uint8_t *customer_name_upper;
  int32_t customer_id;
  float payment_amount;
  uint32_t customer_by_name;
  uint8_t *warehouse_value;
  uint8_t *district_value;
  uint8_t *customer_value;
  size_t output_capacity;
};

struct mako_sto_tpcc_payment_prefix_result {
  size_t warehouse_value_length;
  size_t district_value_length;
  size_t customer_value_length;
  int32_t customer_id;
};

struct mako_sto_tpcc_payment_full_request {
  const sto_tpcc_table *warehouse;
  const sto_tpcc_table *district;
  const sto_tpcc_table *customer;
  const sto_tpcc_table *customer_name;
  const sto_tpcc_table *history;
  const uint8_t *warehouse_key;
  const uint8_t *district_key;
  const uint8_t *customer_key_prefix;
  const uint8_t *customer_name_lower;
  const uint8_t *customer_name_upper;
  int32_t customer_id;
  float payment_amount;
  uint32_t timestamp;
  int32_t warehouse_id;
  int32_t district_id;
  int32_t customer_warehouse_id;
  int32_t customer_district_id;
  uint32_t customer_by_name;
};

struct mako_sto_tpcc_payment_full_result {
  size_t history_value_length;
  int32_t customer_id;
};

struct mako_sto_tpcc_new_order_full_request {
  const sto_tpcc_table *warehouse;
  const sto_tpcc_table *district;
  const sto_tpcc_table *customer;
  const sto_tpcc_table *item;
  const sto_tpcc_table *stock;
  const sto_tpcc_table *new_order;
  const sto_tpcc_table *oorder;
  const sto_tpcc_table *oorder_c_id_idx;
  const sto_tpcc_table *order_line;
  const uint32_t *item_ids;
  const uint32_t *quantities;
  int32_t warehouse_id;
  int32_t district_id;
  int32_t customer_id;
  int32_t order_id;
  uint32_t entry_date;
  uint32_t line_count;
};

struct mako_sto_tpcc_new_order_full_result {
  size_t reported_value_bytes;
};

struct mako_sto_tpcc_delivery_full_request {
  const sto_tpcc_table *new_order;
  const sto_tpcc_table *oorder;
  const sto_tpcc_table *order_line;
  const sto_tpcc_table *customer;
  int32_t *last_no_o_ids;
  int32_t warehouse_id;
  int32_t carrier_id;
  uint32_t timestamp;
};

struct mako_sto_tpcc_delivery_full_result {
  size_t reported_value_bytes;
  uint32_t delivered_districts;
  uint32_t updated_order_lines;
};

struct mako_sto_tpcc_stock_level_full_request {
  const sto_tpcc_table *order_line;
  const sto_tpcc_table *stock;
  uint64_t current_next_order_id;
  int32_t warehouse_id;
  int32_t district_id;
  uint32_t threshold;
};

struct mako_sto_tpcc_stock_level_full_result {
  size_t reported_value_bytes;
  uint32_t scanned_order_line_rows;
  uint32_t distinct_item_ids;
  uint32_t low_stock_count;
};

#if UINTPTR_MAX == UINT64_MAX
#define MAKO_PAYMENT_OFFSET(type, field, expected)                            \
  static_assert(offsetof(type, field) == expected)
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, warehouse, 0);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, district, 8);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, customer, 16);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, customer_name, 24);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, warehouse_key, 32);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, district_key, 40);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request,
                    customer_key_prefix, 48);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request,
                    customer_name_lower, 56);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request,
                    customer_name_upper, 64);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, customer_id, 72);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, payment_amount, 76);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request,
                    customer_by_name, 80);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, warehouse_value, 88);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, district_value, 96);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request, customer_value, 104);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_request,
                    output_capacity, 112);
static_assert(sizeof(mako_sto_tpcc_payment_prefix_request) == 120);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_result,
                    warehouse_value_length, 0);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_result,
                    district_value_length, 8);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_result,
                    customer_value_length, 16);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_prefix_result, customer_id, 24);
static_assert(sizeof(mako_sto_tpcc_payment_prefix_result) == 32);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, warehouse, 0);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, district, 8);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, customer, 16);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, customer_name, 24);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, history, 32);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, warehouse_key, 40);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, district_key, 48);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request,
                    customer_key_prefix, 56);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request,
                    customer_name_lower, 64);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request,
                    customer_name_upper, 72);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, customer_id, 80);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, payment_amount, 84);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, timestamp, 88);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, warehouse_id, 92);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request, district_id, 96);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request,
                    customer_warehouse_id, 100);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request,
                    customer_district_id, 104);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_request,
                    customer_by_name, 108);
static_assert(sizeof(mako_sto_tpcc_payment_full_request) == 112);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_result,
                    history_value_length, 0);
MAKO_PAYMENT_OFFSET(mako_sto_tpcc_payment_full_result, customer_id, 8);
static_assert(sizeof(mako_sto_tpcc_payment_full_result) == 16);
#undef MAKO_PAYMENT_OFFSET

#define MAKO_NEW_ORDER_OFFSET(type, field, expected)                          \
  static_assert(offsetof(type, field) == expected)
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, warehouse, 0);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, district, 8);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, customer, 16);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, item, 24);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, stock, 32);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, new_order, 40);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, oorder, 48);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request,
                      oorder_c_id_idx, 56);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, order_line, 64);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, item_ids, 72);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, quantities, 80);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, warehouse_id, 88);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, district_id, 92);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, customer_id, 96);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, order_id, 100);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, entry_date, 104);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_request, line_count, 108);
static_assert(sizeof(mako_sto_tpcc_new_order_full_request) == 112);
MAKO_NEW_ORDER_OFFSET(mako_sto_tpcc_new_order_full_result,
                      reported_value_bytes, 0);
static_assert(sizeof(mako_sto_tpcc_new_order_full_result) == 8);
#undef MAKO_NEW_ORDER_OFFSET

#define MAKO_DELIVERY_OFFSET(type, field, expected)                           \
  static_assert(offsetof(type, field) == expected)
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, new_order, 0);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, oorder, 8);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, order_line, 16);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, customer, 24);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, last_no_o_ids, 32);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, warehouse_id, 40);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, carrier_id, 44);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_request, timestamp, 48);
static_assert(sizeof(mako_sto_tpcc_delivery_full_request) == 56);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_result,
                     reported_value_bytes, 0);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_result,
                     delivered_districts, 8);
MAKO_DELIVERY_OFFSET(mako_sto_tpcc_delivery_full_result,
                     updated_order_lines, 12);
static_assert(sizeof(mako_sto_tpcc_delivery_full_result) == 16);
#undef MAKO_DELIVERY_OFFSET

#define MAKO_STOCK_LEVEL_OFFSET(type, field, expected)                        \
  static_assert(offsetof(type, field) == expected)
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_request,
                        order_line, 0);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_request, stock, 8);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_request,
                        current_next_order_id, 16);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_request,
                        warehouse_id, 24);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_request,
                        district_id, 28);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_request, threshold, 32);
static_assert(sizeof(mako_sto_tpcc_stock_level_full_request) == 40);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_result,
                        reported_value_bytes, 0);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_result,
                        scanned_order_line_rows, 8);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_result,
                        distinct_item_ids, 12);
MAKO_STOCK_LEVEL_OFFSET(mako_sto_tpcc_stock_level_full_result,
                        low_stock_count, 16);
static_assert(sizeof(mako_sto_tpcc_stock_level_full_result) == 24);
#undef MAKO_STOCK_LEVEL_OFFSET
#endif

extern "C" sto_tpcc_status mako_sto_tpcc_payment_prefix_trusted(
    sto_tpcc_thread *thread,
    const mako_sto_tpcc_payment_prefix_request *request,
    mako_sto_tpcc_payment_prefix_result *result) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_payment_full_trusted(
    sto_tpcc_thread *thread,
    const mako_sto_tpcc_payment_full_request *request,
    mako_sto_tpcc_payment_full_result *result) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_new_order_full_trusted(
    sto_tpcc_thread *thread,
    const mako_sto_tpcc_new_order_full_request *request,
    mako_sto_tpcc_new_order_full_result *result) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_delivery_full_trusted(
    sto_tpcc_thread *thread,
    const mako_sto_tpcc_delivery_full_request *request,
    mako_sto_tpcc_delivery_full_result *result) noexcept;
extern "C" sto_tpcc_status mako_sto_tpcc_stock_level_full_trusted(
    sto_tpcc_thread *thread,
    const mako_sto_tpcc_stock_level_full_request *request,
    mako_sto_tpcc_stock_level_full_result *result) noexcept;

namespace {

// @unsafe - The C ABI owner is shared only at process startup/teardown.
// All wrappers therefore open tables in the same native runtime; same-process
// cross-shard facade wiring never hands a worker a foreign runtime resource.
struct process_database {
  sto_tpcc_db *handle = nullptr;
  size_t owners = 0;
};
rusty::Mutex<process_database> shared_database(process_database{});
#ifdef MAKO_RUST_STO_TEST_HOOKS
thread_local rust_sto_tpcc_detail::open_index_fault_stage
    pending_open_index_fault =
        rust_sto_tpcc_detail::open_index_fault_stage::none;
thread_local size_t destroyed_table_count = 0;

void inject_open_index_fault_if_requested(
    rust_sto_tpcc_detail::open_index_fault_stage stage) {
  if (pending_open_index_fault != stage)
    return;
  pending_open_index_fault =
      rust_sto_tpcc_detail::open_index_fault_stage::none;
  throw std::bad_alloc();
}
#endif

void destroy_ffi_table_noexcept(sto_tpcc_table *table) noexcept {
  if (table == nullptr)
    return;
  (void)sto_tpcc_table_destroy(table);
#ifdef MAKO_RUST_STO_TEST_HOOKS
  ++destroyed_table_count;
#endif
}

// @unsafe - Opaque Rust ownership must be returned through its C destructor,
// not C++ delete. This stack guard adds no allocation.
struct ffi_table_owner {
  sto_tpcc_table *table;
  ~ffi_table_owner() noexcept { destroy_ffi_table_noexcept(table); }
  void release() noexcept { table = nullptr; }
};

constexpr size_t kTpccMinimumAttachmentSlots = 32;
constexpr size_t kTpccAttachmentSlotsPerWorker = 4;
constexpr size_t kTpccReservedAttachmentSlots = 16;
static_assert(MAX_THREADS > 0);
static_assert(std::numeric_limits<size_t>::max() >=
              std::numeric_limits<uint32_t>::max());

// A process-start diagnostic switch permits same-binary scalar/fused A/B
// measurements without adding a getenv call to the transaction hot path.
const bool kDisablePaymentPrefix =
    std::getenv("MAKO_STO_TPCC_DISABLE_PAYMENT_PREFIX") != nullptr;
// Keep the established prefix path available in the same optimized binary for
// diagnostic A/B runs while making the commit-owning lane the default.
const bool kDisablePaymentFull =
    std::getenv("MAKO_STO_TPCC_DISABLE_PAYMENT_FULL") != nullptr;
const bool kDisableNewOrderFull =
    std::getenv("MAKO_STO_TPCC_DISABLE_NEW_ORDER_FULL") != nullptr;
const bool kDisableDeliveryFull =
    std::getenv("MAKO_STO_TPCC_DISABLE_DELIVERY_FULL") != nullptr;
const bool kDisableStockLevelFull =
    std::getenv("MAKO_STO_TPCC_DISABLE_STOCK_LEVEL_FULL") != nullptr;

constexpr std::string_view
tpcc_tablespace_name(std::string_view index_name) {
  // TPC-C's separate-tree mode names a partition `<tablespace>_<warehouse>`.
  // Strip only a numeric suffix: names such as oorder_c_id_idx must otherwise
  // retain their underscores for the capacity classification below.
  const size_t separator = index_name.rfind('_');
  if (separator == std::string_view::npos || separator + 1 == index_name.size())
    return index_name;
  for (size_t index = separator + 1; index < index_name.size(); ++index) {
    if (index_name[index] < '0' || index_name[index] > '9')
      return index_name;
  }
  return index_name.substr(0, separator);
}

constexpr bool
tpcc_table_has_static_directory(std::string_view index_name) {
  // These seven TPC-C tables finish their key set during loading. The same
  // decision selects their smaller registry bounds and the post-load seal.
  const std::string_view tablespace = tpcc_tablespace_name(index_name);
  return tablespace == "customer" || tablespace == "customer_name_idx" ||
         tablespace == "district" || tablespace == "item" ||
         tablespace == "stock" || tablespace == "stock_data" ||
         tablespace == "warehouse";
}

static_assert(tpcc_table_has_static_directory("customer"));
static_assert(tpcc_table_has_static_directory("customer_name_idx_17"));
static_assert(tpcc_table_has_static_directory("district_0"));
static_assert(tpcc_table_has_static_directory("item_42"));
static_assert(tpcc_table_has_static_directory("stock_3"));
static_assert(tpcc_table_has_static_directory("stock_data_3"));
static_assert(tpcc_table_has_static_directory("warehouse_3"));
static_assert(!tpcc_table_has_static_directory("history_3"));
static_assert(!tpcc_table_has_static_directory("new_order_3"));
static_assert(!tpcc_table_has_static_directory("oorder_3"));
static_assert(!tpcc_table_has_static_directory("oorder_c_id_idx_3"));
static_assert(!tpcc_table_has_static_directory("order_line_3"));
static_assert(!tpcc_table_has_static_directory("warehouse_remote"));

constexpr sto_tpcc_resolved_cache_policy
tpcc_resolved_cache_policy_for(std::string_view index_name) {
  const std::string_view tablespace = tpcc_tablespace_name(index_name);

  // Item is global and Stock is warehouse-local. Their dense table-wide
  // caches converge on the complete loaded key sets across all workers while
  // every resolved access still observes the current record version.
  if (tablespace == "item")
    return STO_TPCC_RESOLVED_CACHE_DENSE_ITEM;
  if (tablespace == "stock")
    return STO_TPCC_RESOLVED_CACHE_DENSE_STOCK;

  // One warehouse has only 3,000 customer rows. The worker-local Full cache
  // avoids most customer-tree traversals after warmup and still preserves the
  // scalar get->put handoff through its last-hit lane.
  if (tablespace == "customer")
    return STO_TPCC_RESOLVED_CACHE_FULL;

  // These tables are point-read-only, insert-only, or scanned without a later
  // row update. Retaining their resolutions cannot avoid a native lookup.
  if (tablespace == "customer_name_idx" ||
      tablespace == "oorder_c_id_idx" || tablespace == "history" ||
      tablespace == "stock_data" || tablespace == "order_line")
    return STO_TPCC_RESOLVED_CACHE_NONE;

  // warehouse and district benefit from cross-transaction reuse. Delivery
  // reuses small-scan resolutions for new_order and oorder. Its order-line
  // updates use the fixed-put lane, which does not consume resolved-cache
  // entries, so retaining order-line scan rows would only add hash and copy
  // work. Keep Full as the conservative fallback for unexpected table names.
  return STO_TPCC_RESOLVED_CACHE_FULL;
}

static_assert(tpcc_resolved_cache_policy_for("customer_1") ==
              STO_TPCC_RESOLVED_CACHE_FULL);
static_assert(tpcc_resolved_cache_policy_for("stock_1") ==
              STO_TPCC_RESOLVED_CACHE_DENSE_STOCK);
static_assert(tpcc_resolved_cache_policy_for("item") ==
              STO_TPCC_RESOLVED_CACHE_DENSE_ITEM);
static_assert(tpcc_resolved_cache_policy_for("history_1") ==
              STO_TPCC_RESOLVED_CACHE_NONE);
static_assert(tpcc_resolved_cache_policy_for("order_line_1") ==
              STO_TPCC_RESOLVED_CACHE_NONE);

// @safe - Match native record-local OCC without ordered scan generations.
constexpr sto_tpcc_table_config
tpcc_table_config_for(std::string_view index_name) {
  const std::string_view tablespace = tpcc_tablespace_name(index_name);
  sto_tpcc_table_config config{};
  // Sparse directory nodes and record chunks grow on demand within the
  // database's shared registry-byte budget. These independent logical quotas
  // impose no smaller default ceiling and allocate no metadata in advance.
  config.max_retained_records = std::numeric_limits<uint64_t>::max();
  config.max_consumed_record_ids = std::numeric_limits<uint64_t>::max();
  config.max_retained_key_bytes = std::numeric_limits<uint64_t>::max();
  config.scan_chunk_records = 128;
  config.scan_initial_key_arena_bytes = 16 * 1024;
  config.scan_max_key_arena_bytes = 64 * 1024;
  config.max_scan_chunks = 32'768;
  config.max_scan_physical_records = 4'000'000;
  // Payment rewrites warehouse, district, and customer rows on its critical
  // path, while NewOrder also rewrites district. Keep the 160-byte atomic cell
  // scoped to these frequently updated point tables. District has only ten
  // logical rows per warehouse, so this avoids another shared-value
  // publication without materially expanding the resident data set.
  config.bounded_atomic_values = tablespace == "customer" ||
                                 tablespace == "district" ||
                                 tablespace == "warehouse";
  return config;
}

std::string last_rust_error() {
  size_t required = sto_tpcc_last_error_length();
  if (required == 0)
    return "no Rust diagnostic";
  std::vector<char> bytes(required + 1, '\0');
  size_t actual = 0;
  const sto_tpcc_status status =
      sto_tpcc_last_error_copy(bytes.data(), bytes.size(), &actual);
  if (status != STO_TPCC_OK)
    return "could not copy Rust diagnostic";
  return std::string(bytes.data(), std::min(actual, required));
}

constexpr size_t kMakoValueMetadataBytes =
    static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);

// @unsafe - Decode legacy byte framing; Rust owns all record/history metadata.
// Mutation trailers describe the previously read value, not a new decision.
std::string_view canonical_local_mako_payload(std::string_view encoded_value) {
  if (encoded_value.size() < kMakoValueMetadataBytes)
    throw std::invalid_argument(
        "Rust STO received a truncated Mako metadata suffix");

  const size_t payload_size = encoded_value.size() - kMakoValueMetadataBytes;
  const uint32_t time_term =
      mako::load_value_time_term(encoded_value.data(), encoded_value.size());
  if (time_term != 0 && time_term / 10 == 0)
    throw std::invalid_argument("invalid Mako distributed metadata");
  return encoded_value.substr(0, payload_size);
}

// @unsafe - Preserve the native wire layout without exporting a history pointer.
void append_mako_metadata(std::string &payload, uint32_t time_term) {
  const size_t offset = payload.size();
  payload.append(kMakoValueMetadataBytes, '\0');
  mako::store_unaligned(payload.data() + offset, time_term);
  mako::store_unaligned(payload.data() + offset + sizeof(uint32_t),
                        time_term / 10);
}

struct scan_bridge {
  oi_scan_callback *callback;
  bool append_metadata;
  std::string canonical_value;
  std::exception_ptr failure;
};

// @unsafe - Callback borrows synchronous Rust row bytes and catches exceptions.
int32_t invoke_versioned_scan_bridge(void *context, const uint8_t *key,
                                    size_t key_length, const uint8_t *value,
                                    size_t value_length, uint32_t time_term) {
  auto *bridge = static_cast<scan_bridge *>(context);
  try {
    const char *callback_value = reinterpret_cast<const char *>(value);
    size_t callback_value_length = value_length;
    if (bridge->append_metadata) {
      bridge->canonical_value.clear();
      if (value_length != 0)
        bridge->canonical_value.assign(callback_value, value_length);
      append_mako_metadata(bridge->canonical_value, time_term);
      callback_value = bridge->canonical_value.data();
      callback_value_length = bridge->canonical_value.size();
    }
    return bridge->callback->invoke_bytes(
               reinterpret_cast<const char *>(key), key_length,
               callback_value, callback_value_length)
               ? 0
               : 1;
  } catch (...) {
    // Never unwind C++ through a Rust frame. Stop the scan, then rethrow from
    // scan_raw after the extern "C" call has returned.
    bridge->failure = std::current_exception();
    return 1;
  }
}

// @unsafe - Local-only bootstrap reads have no distributed metadata.
int32_t invoke_scan_bridge(void *context, const uint8_t *key,
                          size_t key_length, const uint8_t *value,
                          size_t value_length) {
  return invoke_versioned_scan_bridge(context, key, key_length, value,
                                      value_length, 0);
}

struct fixed_read_bridge {
  oi_fixed_read_callback *callback;
  size_t max_bytes_read;
  std::exception_ptr failure;
};

int32_t invoke_fixed_read_bridge(void *context, size_t index,
                                 const uint8_t *current_value,
                                 size_t current_value_length) {
  auto *bridge = static_cast<fixed_read_bridge *>(context);
  try {
    const char *logical_value = nullptr;
    size_t logical_value_length = 0;
    if (current_value != nullptr) {
      logical_value = reinterpret_cast<const char *>(current_value);
      logical_value_length =
          std::min(current_value_length, bridge->max_bytes_read);
    } else if (current_value_length != 0) {
      throw std::runtime_error("Rust STO returned an invalid missing value");
    }
    bridge->callback->invoke(index, logical_value, logical_value_length);
    return 0;
  } catch (...) {
    // Rust sees a plain failure code and aborts the attempt. Rethrowing waits
    // until no C++ frame or borrowed value remains inside the Rust call.
    bridge->failure = std::current_exception();
    return 1;
  }
}

struct fixed_modify_bridge {
  oi_fixed_modify_callback *callback;
  std::exception_ptr failure;
};

int32_t invoke_fixed_modify_bridge(
    void *context, size_t index, const uint8_t *current_value,
    size_t current_value_length, const uint8_t **out_replacement,
    size_t *out_replacement_length) {
  auto *bridge = static_cast<fixed_modify_bridge *>(context);
  *out_replacement = nullptr;
  *out_replacement_length = 0;
  try {
    const char *logical_value = nullptr;
    size_t logical_value_length = 0;
    if (current_value != nullptr) {
      logical_value = reinterpret_cast<const char *>(current_value);
      logical_value_length = current_value_length;
    } else if (current_value_length != 0) {
      throw std::runtime_error("Rust STO returned an invalid missing value");
    }

    const oi_fixed_mutation_result result =
        bridge->callback->invoke(index, logical_value, logical_value_length);
    switch (result.action()) {
    case oi_fixed_mutation_action::keep:
      return STO_TPCC_FIXED_MODIFY_KEEP;
    case oi_fixed_mutation_action::put: {
      const std::string *replacement = result.replacement();
      if (replacement == nullptr)
        throw std::runtime_error(
            "fixed-mutation put returned no encoded replacement");
      const std::string_view payload =
          canonical_local_mako_payload(*replacement);
      *out_replacement =
          reinterpret_cast<const uint8_t *>(payload.data());
      *out_replacement_length = payload.size();
      return STO_TPCC_FIXED_MODIFY_PUT;
    }
    case oi_fixed_mutation_action::remove:
      return STO_TPCC_FIXED_MODIFY_REMOVE;
    }
    throw std::runtime_error("fixed-mutation callback returned invalid action");
  } catch (...) {
    // Rust aborts the active attempt on FAILED. The original exception is
    // rethrown only after no C++ frame remains inside the Rust callback.
    bridge->failure = std::current_exception();
    return STO_TPCC_FIXED_MODIFY_FAILED;
  }
}

} // namespace

namespace {

uint64_t capacity_environment(const char *name, uint64_t fallback,
                              bool allow_size_suffix) {
  const char *raw = std::getenv(name);
  if (!raw)
    return fallback;
  const std::string_view spec(raw);
  const auto invalid = [&]() -> std::invalid_argument {
    return std::invalid_argument(std::string("invalid ") + name + ": " +
                                 std::string(spec));
  };
  if (spec.empty() || spec.front() < '1' || spec.front() > '9')
    throw invalid();
  uint64_t multiplier = 1;
  size_t digits = spec.size();
  if (allow_size_suffix) {
    switch (spec.back()) {
    case 'K': multiplier = uint64_t{1} << 10; --digits; break;
    case 'M': multiplier = uint64_t{1} << 20; --digits; break;
    case 'G': multiplier = uint64_t{1} << 30; --digits; break;
    default: break;
    }
  }
  uint64_t amount = 0;
  for (size_t index = 0; index < digits; ++index) {
    const char digit = spec[index];
    if (digit < '0' || digit > '9')
      throw invalid();
    const uint64_t value = static_cast<uint64_t>(digit - '0');
    if (amount > (std::numeric_limits<uint64_t>::max() - value) / 10)
      throw invalid();
    amount = amount * 10 + value;
  }
  if (amount > std::numeric_limits<uint64_t>::max() / multiplier)
    throw invalid();
  amount *= multiplier;
  if (allow_size_suffix && amount > std::numeric_limits<size_t>::max())
    throw invalid();
  return amount;
}

} // namespace

sto_tpcc_db_config rust_sto_tpcc_detail::db_config_for_worker_count(
    size_t configured_workers) {
  // A Rust worker also initializes the legacy C++ TThread state. Size the
  // process-wide budget to the smaller registry, not just Masstree's larger
  // native-core pool.
  const size_t attachment_limit =
      std::min<size_t>(mt_max_threads(), static_cast<size_t>(MAX_THREADS));
  size_t max_supported_workers = 0;
  if (attachment_limit >= kTpccMinimumAttachmentSlots &&
      attachment_limit >= kTpccReservedAttachmentSlots) {
    // Division is intentional: admission is proved before the multiply/add
    // below, so even SIZE_MAX input follows the rejection path without
    // wrapping.
    max_supported_workers =
        (attachment_limit - kTpccReservedAttachmentSlots) /
        kTpccAttachmentSlotsPerWorker;
  }
  if (configured_workers == 0 ||
      configured_workers > max_supported_workers) {
    throw std::invalid_argument(
        "Rust STO TPC-C worker count must be in [1, " +
        std::to_string(max_supported_workers) +
        "]; max(32, 4 * workers + 16) must fit " +
        std::to_string(attachment_limit) +
        " shared C++/native attachment slots");
  }

  const size_t requested_slots =
      configured_workers * kTpccAttachmentSlotsPerWorker +
      kTpccReservedAttachmentSlots;
  sto_tpcc_db_config config{};
  config.max_threads = static_cast<uint32_t>(
      std::max(kTpccMinimumAttachmentSlots, requested_slots));
  config.max_key_length = 1024;
  config.max_items_per_txn = 1024;
  config.max_locks_per_txn = 2048;
  config.max_registry_bytes = capacity_environment(
      "MAKO_STO_TPCC_REGISTRY_MEMORY", uint64_t{8} << 30, true);
  return config;
}

sto_tpcc_table_config
rust_sto_tpcc_detail::table_config_for(std::string_view index_name) {
  sto_tpcc_table_config config = tpcc_table_config_for(index_name);
  config.max_retained_records = capacity_environment(
      "MAKO_STO_TPCC_MAX_RETAINED_RECORDS", config.max_retained_records, false);
  config.max_consumed_record_ids = capacity_environment(
      "MAKO_STO_TPCC_MAX_CONSUMED_RECORD_IDS", config.max_consumed_record_ids,
      false);
  config.max_retained_key_bytes = capacity_environment(
      "MAKO_STO_TPCC_MAX_RETAINED_KEY_BYTES", config.max_retained_key_bytes,
      false);
  return config;
}

void rust_sto_tpcc_detail::validate_capacity_environment() {
  (void)table_config_for("order_line");
  (void)capacity_environment("MAKO_STO_TPCC_REGISTRY_MEMORY",
                             uint64_t{8} << 30, true);
}

bool rust_sto_tpcc_detail::table_has_static_directory(
    std::string_view index_name) {
  return tpcc_table_has_static_directory(index_name);
}

#ifdef MAKO_RUST_STO_TEST_HOOKS
void rust_sto_tpcc_detail::fail_next_open_index_at(
    open_index_fault_stage stage) noexcept {
  pending_open_index_fault = stage;
}

size_t rust_sto_tpcc_detail::destroyed_table_count_for_testing() noexcept {
  return destroyed_table_count;
}
#endif

thread_local sto_tpcc_thread *rust_sto_tpcc_wrapper::tls_thread_ = nullptr;
thread_local bool rust_sto_tpcc_wrapper::tls_transaction_active_ = false;
thread_local bool rust_sto_tpcc_wrapper::tls_benchmark_thread_initialized_ =
    false;
thread_local rust_sto_tpcc_wrapper *rust_sto_tpcc_wrapper::tls_owner_ = nullptr;
thread_local uint32_t rust_sto_tpcc_wrapper::tls_term_ = 0;
thread_local bool rust_sto_tpcc_wrapper::tls_protocol_ = false;
thread_local bool rust_sto_tpcc_wrapper::tls_suppress_log_ = false;
thread_local uint32_t rust_sto_tpcc_wrapper::tls_last_read_time_term_ = 0;
thread_local std::vector<sto_tpcc_fixed_value>
    rust_sto_tpcc_wrapper::tls_fixed_values_;
thread_local std::vector<sto_tpcc_insert_operation>
    rust_sto_tpcc_wrapper::tls_insert_operations_;

rust_sto_tpcc_ordered_index::rust_sto_tpcc_ordered_index(
    rust_sto_tpcc_wrapper *owner, sto_tpcc_table *table, int32_t table_id,
    std::string name, bool is_remote)
    : owner_(owner), table_(table), table_id_(table_id),
      name_(std::move(name)), is_remote_(is_remote) {}

rust_sto_tpcc_ordered_index::~rust_sto_tpcc_ordered_index() noexcept {
  if (table_) {
    destroy_ffi_table_noexcept(table_);
    table_ = nullptr;
  }
}

// @unsafe - One-operation legacy facade; only definite aborts are retried.
bool rust_sto_tpcc_ordered_index::get(lcdf::Str key, std::string &value,
                                      size_t max_bytes_read) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      const bool found = tx_get(owner_->tls_thread_, key, value, max_bytes_read);
      if (!owner_->commit_txn(owner_->tls_thread_))
        continue;
      return found;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_txn(owner_->tls_thread_);
    } catch (...) {
      owner_->abort_txn(owner_->tls_thread_);
      throw;
    }
  }
}

// @unsafe - Encode legacy bytes and drive the native scalar terminal.
bool rust_sto_tpcc_ordered_index::put(lcdf::Str key,
                                      const std::string &value) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      std::string ignored;
      const bool existed = tx_get(owner_->tls_thread_, key, ignored, 0);
      const std::string encoded = mako::Encode(value);
      if (owner_->tls_protocol_)
        owner_->mutate_protocol(*this, key, encoded, 0, false);
      else
        owner_->put_raw(table_, key, encoded, false);
      owner_->commit_txn(owner_->tls_thread_);
      return !existed;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_txn(owner_->tls_thread_);
    } catch (...) {
      owner_->abort_txn(owner_->tls_thread_);
      throw;
    }
  }
}

// @unsafe - Preserve conditional-insert results across the legacy boundary.
bool rust_sto_tpcc_ordered_index::insert(lcdf::Str key,
                                         const std::string &value) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      if (owner_->tls_protocol_) {
        std::string ignored;
        if (tx_get(owner_->tls_thread_, key, ignored, 0)) {
          owner_->commit_txn(owner_->tls_thread_);
          return false;
        }
        owner_->mutate_protocol(*this, key, mako::Encode(value), 1, false);
        owner_->commit_txn(owner_->tls_thread_);
        return true;
      }
      const std::string encoded = mako::Encode(value);
      const std::string_view payload = canonical_local_mako_payload(encoded);
      const sto_tpcc_status status = mako_sto_tpcc_insert_trusted(
          owner_->tls_thread_, table_,
          reinterpret_cast<const uint8_t *>(key.data()), key.length(),
          reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
      if (status == STO_TPCC_DUPLICATE) {
        owner_->abort_current_transaction_noexcept();
        return false;
      }
      owner_->require_ok("insert", status);
      owner_->commit_txn(owner_->tls_thread_);
      return true;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_txn(owner_->tls_thread_);
    } catch (...) {
      owner_->abort_txn(owner_->tls_thread_);
      throw;
    }
  }
}

// @unsafe - Preserve remove presence while Rust owns the mutation and decision.
bool rust_sto_tpcc_ordered_index::remove(lcdf::Str key) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      std::string ignored;
      const bool existed = owner_->tls_protocol_
          ? tx_get(owner_->tls_thread_, key, ignored, 0)
          : owner_->remove_raw(table_, key);
      if (owner_->tls_protocol_ && existed)
        owner_->mutate_protocol(*this, key, {}, 2, false);
      owner_->commit_txn(owner_->tls_thread_);
      return existed;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_txn(owner_->tls_thread_);
    } catch (...) {
      owner_->abort_txn(owner_->tls_thread_);
      throw;
    }
  }
}

void rust_sto_tpcc_ordered_index::scan(const std::string &start_key,
                                       const std::string *end_key,
                                       oi_scan_callback &callback,
                                       str_arena *) {
  if (routes_remotely())
    throw std::invalid_argument("remote ranges use tx_scan_remote_one");
  while (true) {
    owner_->begin_current_transaction();
    try {
      owner_->scan_raw(table_, false, start_key, end_key, callback, true);
      owner_->commit_txn(owner_->tls_thread_);
      return;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_txn(owner_->tls_thread_);
    } catch (...) {
      owner_->abort_txn(owner_->tls_thread_);
      throw;
    }
  }
}

void rust_sto_tpcc_ordered_index::rscan(const std::string &start_key,
                                        const std::string *end_key,
                                        oi_scan_callback &callback,
                                        str_arena *) {
  if (routes_remotely())
    throw std::invalid_argument("remote ranges use tx_scan_remote_one");
  while (true) {
    owner_->begin_current_transaction();
    try {
      owner_->scan_raw(table_, true, start_key, end_key, callback, true);
      owner_->commit_txn(owner_->tls_thread_);
      return;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_txn(owner_->tls_thread_);
    } catch (...) {
      owner_->abort_txn(owner_->tls_thread_);
      throw;
    }
  }
}

size_t rust_sto_tpcc_ordered_index::size() const {
  uint64_t rows = 0;
  if (is_remote_)
    return 0; // A proxy owns no local rows; remote cardinality is not cached.
  owner_->require_ok("table_size", sto_tpcc_table_size(table_, &rows));
  return static_cast<size_t>(rows);
}

oi_stats_map rust_sto_tpcc_ordered_index::clear() { return {}; }
int32_t rust_sto_tpcc_ordered_index::get_table_id() { return table_id_; }
// @unsafe - Live colocated replicas must use the owning affine participant.
bool rust_sto_tpcc_ordered_index::routes_remotely() const noexcept {
  return is_remote_ || (owner_->replicated_ && owner_->tls_thread_ &&
      (table_id_ - 1) / mako::NUM_TABLES_PER_SHARD !=
          TThread::get_shard_index());
}

// @unsafe - Legacy workload dispatch observes the same routing as the verbs.
bool rust_sto_tpcc_ordered_index::get_is_remote() { return routes_remotely(); }

// @unsafe - Dispatch the affine native read or the legacy remote proxy.
bool rust_sto_tpcc_ordered_index::tx_get(c_void *txn, lcdf::Str key,
                                         std::string &value,
                                         size_t max_bytes_read) {
  owner_->require_txn_handle(txn);
  if (routes_remotely())
    return owner_->remote_get(*this, key, value, max_bytes_read);
  return owner_->get_active_raw(table_, key, value, max_bytes_read);
}

void rust_sto_tpcc_ordered_index::tx_visit_fixed(
    c_void *txn, const char *keys, size_t key_width, size_t key_count,
    size_t max_bytes_read, oi_fixed_read_callback &callback) {
  owner_->require_txn_handle(txn);
  fixed_read_bridge bridge{&callback, max_bytes_read, nullptr};
  size_t visited = 0;
  const sto_tpcc_status status = sto_tpcc_visit_fixed(
      owner_->tls_thread_, table_, reinterpret_cast<const uint8_t *>(keys),
      key_count, key_width, invoke_fixed_read_bridge, &bridge, &visited);
  if (bridge.failure) {
    owner_->tls_transaction_active_ = false;
    std::rethrow_exception(bridge.failure);
  }
  owner_->require_ok("fixed read", status);
  if (visited != key_count) {
    owner_->abort_current_transaction_noexcept();
    throw std::runtime_error("Rust STO fixed read returned a short batch");
  }
}

void rust_sto_tpcc_ordered_index::tx_modify_fixed(
    c_void *txn, const char *keys, size_t key_width, size_t key_count,
    oi_fixed_modify_callback &callback) {
  owner_->require_txn_handle(txn);
  fixed_modify_bridge bridge{&callback, nullptr};
  size_t visited = 0;
  const sto_tpcc_status status = sto_tpcc_modify_fixed(
      owner_->tls_thread_, table_, reinterpret_cast<const uint8_t *>(keys),
      key_count, key_width, invoke_fixed_modify_bridge, &bridge, &visited);
  if (bridge.failure) {
    owner_->tls_transaction_active_ = false;
    std::rethrow_exception(bridge.failure);
  }
  owner_->require_ok("fixed mutation", status);
  if (visited != key_count) {
    owner_->abort_current_transaction_noexcept();
    throw std::runtime_error("Rust STO fixed mutation returned a short batch");
  }
}

oi_fixed_put_result rust_sto_tpcc_ordered_index::tx_put_fixed(
    c_void *txn, const char *keys, size_t key_width, size_t key_count,
    const std::string_view *encoded_values, oi_fixed_put_mode mode) {
  owner_->require_txn_handle(txn);
  if (key_count != 0 && (keys == nullptr || encoded_values == nullptr))
    throw std::invalid_argument(
        "fixed put requires keys and values for a nonempty batch");

  auto &descriptors = owner_->tls_fixed_values_;
  descriptors.resize(key_count);
  for (size_t index = 0; index < key_count; ++index) {
    const std::string_view value =
        canonical_local_mako_payload(encoded_values[index]);
    descriptors[index] = {
        reinterpret_cast<const uint8_t *>(value.data()), value.size()};
  }

  sto_tpcc_fixed_put_mode raw_mode;
  switch (mode) {
  case oi_fixed_put_mode::upsert:
    raw_mode = STO_TPCC_FIXED_PUT_UPSERT;
    break;
  case oi_fixed_put_mode::insert:
    raw_mode = STO_TPCC_FIXED_PUT_INSERT;
    break;
  default:
    throw std::invalid_argument("invalid fixed put mode");
  }
  sto_tpcc_fixed_put_result raw_result{};
  const sto_tpcc_status status = sto_tpcc_put_fixed(
      owner_->tls_thread_, table_, reinterpret_cast<const uint8_t *>(keys),
      key_count, key_width,
      descriptors.empty() ? nullptr : descriptors.data(), raw_mode,
      &raw_result);
  if (status != STO_TPCC_OK && status != STO_TPCC_DUPLICATE)
    owner_->require_ok("fixed put", status);

  const bool has_duplicate =
      raw_result.first_duplicate != std::numeric_limits<size_t>::max();
  if (raw_result.inserted > key_count ||
      (has_duplicate && raw_result.first_duplicate >= key_count) ||
      (mode == oi_fixed_put_mode::upsert &&
       (status != STO_TPCC_OK || has_duplicate)) ||
      (mode == oi_fixed_put_mode::insert &&
       ((status == STO_TPCC_DUPLICATE) != has_duplicate))) {
    owner_->abort_current_transaction_noexcept();
    throw std::runtime_error("Rust STO fixed put returned invalid metadata");
  }
  return {raw_result.inserted, raw_result.first_duplicate};
}

// @unsafe - Protocol intents copy bytes; the local fast lane borrows its arena.
void rust_sto_tpcc_ordered_index::tx_put(c_void *txn, lcdf::Str key,
                                         const std::string &value) {
  owner_->require_txn_handle(txn);
  if (owner_->tls_protocol_)
    owner_->mutate_protocol(*this, key, value, 0, false);
  else
    owner_->put_active_raw(table_, key, value, false);
}

// @unsafe - Inserts copy loader/arena bytes before the caller reuses them.
void rust_sto_tpcc_ordered_index::tx_insert(c_void *txn, lcdf::Str key,
                                            const std::string &value) {
  owner_->require_txn_handle(txn);
  if (routes_remotely()) {
    std::string ignored;
    if (owner_->remote_get(*this, key, ignored, 0))
      return;
  }
  if (owner_->tls_protocol_)
    owner_->mutate_protocol(*this, key, value, 1, false);
  else
    owner_->put_active_raw(table_, key, value, true);
}

// @unsafe - Route delete semantics without synthesizing an ordinary PUT.
void rust_sto_tpcc_ordered_index::tx_remove(c_void *txn, lcdf::Str key) {
  owner_->require_txn_handle(txn);
  if (owner_->tls_protocol_)
    owner_->mutate_protocol(*this, key, {}, 2, false);
  else
    (void)owner_->remove_active_raw(table_, key);
}

void rust_sto_tpcc_ordered_index::tx_scan(c_void *txn,
                                          const std::string &start_key,
                                          const std::string *end_key,
                                          oi_scan_callback &callback,
                                          str_arena *) {
  owner_->require_txn_handle(txn);
  if (routes_remotely())
    throw std::invalid_argument("remote ranges use tx_scan_remote_one");
  owner_->scan_active_raw(table_, false, start_key, end_key, callback, true);
}

void rust_sto_tpcc_ordered_index::tx_rscan(c_void *txn,
                                           const std::string &start_key,
                                           const std::string *end_key,
                                           oi_scan_callback &callback,
                                           str_arena *) {
  owner_->require_txn_handle(txn);
  if (routes_remotely())
    throw std::invalid_argument("remote ranges use tx_scan_remote_one");
  owner_->scan_active_raw(table_, true, start_key, end_key, callback, true);
}

// @unsafe - Existing remote scan contract returns the lower median payload.
void rust_sto_tpcc_ordered_index::tx_scan_remote_one(
    c_void *txn, const std::string &start_key, const std::string &end_key,
    std::string &value) {
  owner_->require_txn_handle(txn);
  if (routes_remotely()) {
    if (!TThread::sclient)
      throw std::logic_error("Rust STO remote scan has no shard client");
    owner_->require_ok("flush remote scan", sto_tpcc_protocol_flush_remote(
        owner_->tls_thread_, owner_->protocol_callback, owner_));
    const int status = TThread::sclient->remoteScan(
        table_id_, start_key, end_key, value);
    if (status != mako::ErrorCode::SUCCESS &&
        status != mako::ErrorCode::NOT_FOUND)
      throw abstract_db::abstract_abort_exception();
    uint32_t time_term = 0;
    if (status == mako::ErrorCode::SUCCESS) {
      const auto payload = canonical_local_mako_payload(value);
      time_term = mako::load_value_time_term(value.data(), value.size());
      value.resize(payload.size());
    } else {
      value.clear();
    }
    owner_->require_ok("remote scan dependency", sto_tpcc_protocol_observe_read(
        owner_->tls_thread_, (table_id_ - 1) / mako::NUM_TABLES_PER_SHARD,
        time_term));
    return;
  }
  class first_value_callback final : public oi_scan_callback {
  public:
    explicit first_value_callback(std::string &target) : target_(target) {}
    bool invoke(const char *, size_t, const std::string &v) override {
      target_ = v;
      return false;
    }
    size_t max_records_hint() const override { return 1; }

  private:
    std::string &target_;
  } callback(value);
  owner_->scan_active_raw(table_, false, start_key, &end_key, callback, true);
}

// @unsafe - Materialize metadata captured with the selected native payload.
bool rust_sto_tpcc_ordered_index::shard_get(lcdf::Str key,
                                            std::string &value,
                                            size_t max_bytes_read) {
  const bool found = owner_->get_raw(table_, key, value);
  if (found) {
    append_mako_metadata(value, owner_->tls_last_read_time_term_);
    if (value.size() > max_bytes_read)
      value.resize(max_bytes_read);
  }
  return found;
}

const c_char *rust_sto_tpcc_ordered_index::shard_put(
    lcdf::Str key, const std::string &value) {
  owner_->mutate_protocol(*this, key, value, 0, true);
  return nullptr;
}

bool rust_sto_tpcc_ordered_index::shard_scan(
    const std::string &start_key, const std::string *end_key,
    oi_scan_callback &callback, str_arena *) {
  owner_->scan_raw(table_, false, start_key, end_key, callback, false);
  return true;
}

// @unsafe - Owns one reference to the process native runtime.
rust_sto_tpcc_wrapper::rust_sto_tpcc_wrapper()
    : db_(nullptr), tables_by_id_(static_cast<size_t>(kTableIdLimit)) {}

rust_sto_tpcc_wrapper::~rust_sto_tpcc_wrapper() noexcept {
  tables_by_name_.clear();
  for (const auto &slot : tables_by_id_)
    slot.store(nullptr, rusty::sync::atomic::Ordering::Release);
  tables_.clear();
  if (db_) {
    auto shared = shared_database.lock().unwrap();
    if (--shared->owners == 0) {
      (void)sto_tpcc_db_destroy(shared->handle);
      shared->handle = nullptr;
    }
    db_ = nullptr;
  }
}

// @unsafe - Configuration and native C ABI startup boundary.
void rust_sto_tpcc_wrapper::init() {
  auto &config = BenchmarkConfig::getInstance();
  protocol_mode_ = config.getNshards() != 1 || config.getIsReplicated();
  replicated_ = config.getIsReplicated();
  const size_t shards = config.getNshards();
  const size_t workers = config.getNthreads();
  if (shards == 0 || workers == 0 ||
      shards > std::numeric_limits<size_t>::max() / workers)
    throw std::invalid_argument("invalid Rust STO process worker topology");
  // Four slots per shard/warehouse cover foreground, loader, participant and
  // replay lanes; the fixed reserve covers service/maintenance attachment.
  sto_tpcc_db_config ffi_config =
      rust_sto_tpcc_detail::db_config_for_worker_count(shards * workers);
  const auto *transport = config.getConfig();
  const size_t local_shards = transport && !transport->local_shard_indices.empty()
      ? transport->local_shard_indices.size() : 1;
  const size_t warehouses = transport
      ? static_cast<size_t>(transport->warehouses) : workers;
  const size_t limit =
      std::min<size_t>(mt_max_threads(), static_cast<size_t>(MAX_THREADS));
  // Helpers remain resident during loading. The parallel loader has three
  // warehouse-sized groups plus warehouse/item/district lanes; foreground
  // workers replace loaders, while replay attachments can coexist with either.
  if (warehouses == 0 || shards > limit / warehouses ||
      local_shards > limit || workers > limit / 2 ||
      warehouses > (limit - 3) / 3)
    throw std::invalid_argument("Rust STO process attachment budget exhausted");
  const size_t loaders = config.getEnableParallelLoading()
      ? 3 * warehouses + 3 : 6;
  const size_t per_shard =
      shards * warehouses + workers + std::max(workers, loaders);
  if (per_shard > (limit - kTpccReservedAttachmentSlots) / local_shards)
    throw std::invalid_argument("Rust STO helpers/replay exceed worker capacity");
  ffi_config.max_threads = static_cast<uint32_t>(std::max<size_t>(
      ffi_config.max_threads,
      local_shards * per_shard + kTpccReservedAttachmentSlots));
  auto shared = shared_database.lock().unwrap();
  if (db_ != nullptr)
    throw std::logic_error("Rust STO database initialized twice");
  if (shared->handle == nullptr)
    require_ok("db_create", sto_tpcc_db_create(&ffi_config, &shared->handle));
  db_ = shared->handle;
  ++shared->owners;
  if (config.getIsReplicated() && !config.getLeaderConfig()) {
    // Followers receive loading logs before a workload constructs named
    // tables. Native tree creation needs a quiescent startup thread.
    const int shard = static_cast<int>(config.getShardIndex());
    for (int ordinal = 1; ordinal <= mako::NUM_TABLES_PER_SHARD; ++ordinal) {
      const int id = shard * mako::NUM_TABLES_PER_SHARD + ordinal;
      if (id >= kTableIdLimit)
        throw std::length_error("Rust STO shard table ID capacity exhausted");
      sto_tpcc_table *table = nullptr;
      const auto table_config = rust_sto_tpcc_detail::table_config_for("replay");
      require_ok("replay table create", sto_tpcc_table_create_with_cache_policy(
          db_, &table_config, STO_TPCC_RESOLVED_CACHE_NONE, &table));
      ffi_table_owner pending{table};
      auto index = rusty::make_box<rust_sto_tpcc_ordered_index>(
          this, table, id, std::string{}, false);
      pending.release();
      auto *raw = index.get();
      tables_.push_back(std::move(index));
      tables_by_id_[static_cast<size_t>(id)].store(
          raw, rusty::sync::atomic::Ordering::Release);
      mako::setup_update_table(id, raw);
    }
  }
}

void rust_sto_tpcc_wrapper::on_load_complete() {
  for (const auto &table : tables_) {
    if (table->is_remote_ || !tpcc_table_has_static_directory(table->name_))
      continue;
    require_ok("table_seal_directory_structure",
               sto_tpcc_table_seal_directory_structure(table->table_));
  }
}

void rust_sto_tpcc_wrapper::report_capacity_usage(const char *phase) const {
  sto_tpcc_db_usage_info db_usage{};
  require_ok("db_usage", sto_tpcc_db_usage(db_, &db_usage));
  auto output = mako::benchmark_cerr();
  output << "STO_TPCC_CAPACITY phase=" << phase
         << " scope=database allocated_registry_bytes="
         << db_usage.allocated_registry_bytes
         << " max_registry_bytes=" << db_usage.max_registry_bytes
         << " registry_headroom_bytes="
         << (db_usage.max_registry_bytes - db_usage.allocated_registry_bytes)
         << '\n';
  for (const auto &table : tables_) {
    if (table->is_remote_)
      continue;
    sto_tpcc_table_usage_info usage{};
    require_ok("table_usage", sto_tpcc_table_usage(table->table_, &usage));
    output << "STO_TPCC_CAPACITY phase=" << phase << " table=" << table->name_
           << " retained_records=" << usage.retained_records
           << " consumed_record_ids=" << usage.consumed_record_ids
           << " retained_key_bytes=" << usage.retained_key_bytes
           << " allocated_registry_bytes=" << usage.allocated_registry_bytes
           << " max_retained_records=" << usage.max_retained_records
           << " max_consumed_record_ids=" << usage.max_consumed_record_ids
           << " max_retained_key_bytes=" << usage.max_retained_key_bytes
           << '\n';
  }
  output << std::flush;
}

void rust_sto_tpcc_wrapper::preallocate_open_index() {}
ssize_t rust_sto_tpcc_wrapper::txn_max_batch_size() const { return 100; }
size_t rust_sto_tpcc_wrapper::sizeof_txn_object(uint64_t txn_flags) const {
  ALWAYS_ASSERT(txn_flags == 0);
  return 1;
}

// @unsafe - One native worker and one legacy attachment on this OS thread.
void rust_sto_tpcc_wrapper::thread_init(bool loader, int source) {
  if (tls_thread_)
    throw std::runtime_error("Rust STO thread was initialized twice");
  (void)source;
  mbta_wrapper::benchmark_thread_init(loader);
  tls_benchmark_thread_initialized_ = true;
  const sto_tpcc_status status = sto_tpcc_thread_create(db_, &tls_thread_);
  if (status != STO_TPCC_OK) {
    mbta_wrapper::benchmark_thread_end();
    tls_benchmark_thread_initialized_ = false;
    require_ok("thread_create", status);
  } else {
    tls_owner_ = this;
  }
}

// @unsafe - Resolve native state before destroying its worker/RCU attachment.
void rust_sto_tpcc_wrapper::thread_end() {
  if (tls_transaction_active_)
    abort_txn(tls_thread_);
  sto_tpcc_status destroy_status = STO_TPCC_OK;
  if (tls_thread_) {
    destroy_status = sto_tpcc_thread_destroy(tls_thread_);
    tls_thread_ = nullptr;
    tls_owner_ = nullptr;
  }
  if (tls_benchmark_thread_initialized_) {
    mbta_wrapper::benchmark_thread_end();
    tls_benchmark_thread_initialized_ = false;
  }
  require_ok("thread_destroy", destroy_status);
}

void *rust_sto_tpcc_wrapper::new_txn(uint64_t txn_flags, str_arena &, void *,
                                     TxnProfileHint) {
  ALWAYS_ASSERT(txn_flags == 0);
  begin_current_transaction();
  return tls_thread_;
}

// @unsafe - Rust owns the decision; only a definite predecision abort retries.
bool rust_sto_tpcc_wrapper::commit_txn(void *txn) {
  require_txn_handle(txn);
  sto_tpcc_protocol_report report{};
  const sto_tpcc_status status = tls_protocol_
      ? sto_tpcc_protocol_commit(tls_thread_, mako::getCurrentTimeMillis(),
                                 protocol_callback, this, &report)
      : mako_sto_tpcc_txn_commit_trusted(tls_thread_);
  tls_transaction_active_ = false;
  if (tls_protocol_ && status == STO_TPCC_RETRY &&
      report.disposition != STO_TPCC_ABORTED)
    throw_fatal("non-aborted retry disposition", STO_TPCC_UNKNOWN);
  require_ok("txn_commit", status);
  return true;
}

// @unsafe - Same terminal protocol, without resubmitting a local legacy log.
bool rust_sto_tpcc_wrapper::commit_txn_no_paxos(void *txn) {
  const bool saved = std::exchange(tls_suppress_log_, true);
  try {
    const bool committed = commit_txn(txn);
    tls_suppress_log_ = saved;
    return committed;
  } catch (...) {
    tls_suppress_log_ = saved;
    throw;
  }
}

// @unsafe - Resolve every touched helper before releasing coordinator state.
void rust_sto_tpcc_wrapper::abort_txn(void *txn) {
  if (txn && txn != tls_thread_)
    throw std::runtime_error("foreign Rust STO transaction handle");
  if (tls_protocol_ && TThread::mode() == 0 && TThread::sclient &&
      TThread::sclient->hasTouchedParticipants()) {
    const int result = TThread::sclient->remoteAbort();
    if (result != mako::ErrorCode::SUCCESS)
      throw_fatal("remote abort cleanup", STO_TPCC_FATAL);
  }
  abort_current_transaction_noexcept();
}

// @unsafe - Local abort never sends a remote participant terminal.
void rust_sto_tpcc_wrapper::abort_txn_local(void *) {
  abort_current_transaction_noexcept();
}

TxnFixedReadCapability *rust_sto_tpcc_wrapper::txn_fixed_read_capability(
    abstract_ordered_index *index) noexcept {
  if (protocol_mode_ || index == nullptr)
    return nullptr;
#ifndef NDEBUG
  const auto owned = std::find_if(
      tables_.begin(), tables_.end(),
      [index](const auto &candidate) { return candidate.get() == index; });
  assert(owned != tables_.end() &&
         "fixed-read capability requested for a foreign index");
#endif
  // The abstract_db contract admits only an index opened by this instance.
  // Every such index has this concrete type, so release builds pay no RTTI or
  // ownership lookup on the hot capability check.
  return static_cast<rust_sto_tpcc_ordered_index *>(index);
}

TxnFixedModifyCapability *rust_sto_tpcc_wrapper::txn_fixed_modify_capability(
    abstract_ordered_index *index) noexcept {
  if (protocol_mode_ || index == nullptr)
    return nullptr;
#ifndef NDEBUG
  const auto owned = std::find_if(
      tables_.begin(), tables_.end(),
      [index](const auto &candidate) { return candidate.get() == index; });
  assert(owned != tables_.end() &&
         "fixed-mutation capability requested for a foreign index");
#endif
  return static_cast<rust_sto_tpcc_ordered_index *>(index);
}

TxnFixedPutCapability *rust_sto_tpcc_wrapper::txn_fixed_put_capability(
    abstract_ordered_index *index) noexcept {
  if (protocol_mode_ || index == nullptr)
    return nullptr;
#ifndef NDEBUG
  const auto owned = std::find_if(
      tables_.begin(), tables_.end(),
      [index](const auto &candidate) { return candidate.get() == index; });
  assert(owned != tables_.end() &&
         "fixed-put capability requested for a foreign index");
#endif
  return static_cast<rust_sto_tpcc_ordered_index *>(index);
}

TxnInsertBatchCapability *
rust_sto_tpcc_wrapper::txn_insert_batch_capability() noexcept {
  return protocol_mode_ ? nullptr : this;
}

TxnTpccPaymentCapability *
rust_sto_tpcc_wrapper::txn_tpcc_payment_capability() noexcept {
  return protocol_mode_ || kDisablePaymentPrefix ? nullptr : this;
}

TxnTpccNewOrderCapability *
rust_sto_tpcc_wrapper::txn_tpcc_new_order_capability() noexcept {
  return protocol_mode_ || kDisableNewOrderFull ? nullptr : this;
}

TxnTpccDeliveryCapability *
rust_sto_tpcc_wrapper::txn_tpcc_delivery_capability() noexcept {
  return protocol_mode_ || kDisableDeliveryFull ? nullptr : this;
}

TxnTpccStockLevelCapability *
rust_sto_tpcc_wrapper::txn_tpcc_stock_level_capability() noexcept {
  return protocol_mode_ || kDisableStockLevelFull ? nullptr : this;
}

bool rust_sto_tpcc_wrapper::payment_full_enabled() const noexcept {
  return !protocol_mode_ && !kDisablePaymentFull;
}

tpcc_fixed_batch::payment_full_result
rust_sto_tpcc_wrapper::tx_payment_full(
    const tpcc_fixed_batch::payment_full_request &request) {
  require_txn_handle(request.txn);
  if (request.warehouse == nullptr || request.district == nullptr ||
      request.customer == nullptr || request.history == nullptr ||
      request.warehouse_key == nullptr || request.district_key == nullptr ||
      request.customer_key_prefix == nullptr ||
      (request.customer_by_name &&
       (request.customer_name == nullptr ||
        request.customer_name_lower == nullptr ||
        request.customer_name_upper == nullptr)) ||
      (!request.customer_by_name && request.customer_id <= 0) ||
      request.warehouse_id <= 0 || request.district_id <= 0 ||
      request.customer_warehouse_id <= 0 ||
      request.customer_district_id <= 0) {
    throw std::invalid_argument("invalid full TPC-C Payment request");
  }

  const auto unwrap = [&](abstract_ordered_index *index,
                          const char *description)
      -> rust_sto_tpcc_ordered_index * {
    if (index == nullptr)
      return nullptr;
#ifndef NDEBUG
    const auto owned = std::find_if(
        tables_.begin(), tables_.end(),
        [index](const auto &candidate) { return candidate.get() == index; });
    assert(owned != tables_.end() &&
           "full Payment contains an index from another database");
#else
    (void)description;
#endif
    auto *table = static_cast<rust_sto_tpcc_ordered_index *>(index);
#ifndef NDEBUG
    assert(table->owner_ == this && !table->is_remote_ && description);
#endif
    return table;
  };

  auto *warehouse = unwrap(request.warehouse, "warehouse");
  auto *district = unwrap(request.district, "district");
  auto *customer = unwrap(request.customer, "customer");
  auto *customer_name = unwrap(request.customer_name, "customer name");
  auto *history = unwrap(request.history, "history");
  const mako_sto_tpcc_payment_full_request raw_request{
      warehouse->table_,
      district->table_,
      customer->table_,
      customer_name == nullptr ? nullptr : customer_name->table_,
      history->table_,
      reinterpret_cast<const uint8_t *>(request.warehouse_key),
      reinterpret_cast<const uint8_t *>(request.district_key),
      reinterpret_cast<const uint8_t *>(request.customer_key_prefix),
      reinterpret_cast<const uint8_t *>(request.customer_name_lower),
      reinterpret_cast<const uint8_t *>(request.customer_name_upper),
      request.customer_id,
      request.payment_amount,
      request.timestamp,
      request.warehouse_id,
      request.district_id,
      request.customer_warehouse_id,
      request.customer_district_id,
      request.customer_by_name ? uint32_t{1} : uint32_t{0}};
  mako_sto_tpcc_payment_full_result raw_result{};
  const sto_tpcc_status status = mako_sto_tpcc_payment_full_trusted(
      tls_thread_, &raw_request, &raw_result);

  // The full call resolves the attempt on every return: success commits it;
  // retry and fatal paths leave the guard to abort it.
  tls_transaction_active_ = false;
  if (status == STO_TPCC_RETRY)
    throw abstract_abort_exception();
  require_ok("full TPC-C Payment", status);
  if (raw_result.history_value_length !=
          tpcc_fixed_batch::payment_history_value_length ||
      raw_result.customer_id <= 0) {
    throw std::runtime_error("Rust STO full Payment returned invalid metadata");
  }
  return {raw_result.history_value_length, raw_result.customer_id};
}

tpcc_fixed_batch::payment_prefix_result
rust_sto_tpcc_wrapper::tx_payment_prefix(
    const tpcc_fixed_batch::payment_prefix_request &request) {
  require_txn_handle(request.txn);
  if (request.warehouse == nullptr || request.district == nullptr ||
      request.customer == nullptr || request.warehouse_key == nullptr ||
      request.district_key == nullptr ||
      request.customer_key_prefix == nullptr ||
      request.warehouse_value == nullptr || request.district_value == nullptr ||
      request.customer_value == nullptr ||
      (request.customer_by_name &&
       (request.customer_name == nullptr ||
        request.customer_name_lower == nullptr ||
        request.customer_name_upper == nullptr)) ||
      (!request.customer_by_name && request.customer_id <= 0)) {
    throw std::invalid_argument("invalid TPC-C Payment prefix request");
  }

  const auto unwrap = [&](abstract_ordered_index *index,
                          const char *description)
      -> rust_sto_tpcc_ordered_index * {
    if (index == nullptr)
      return nullptr;
#ifndef NDEBUG
    const auto owned = std::find_if(
        tables_.begin(), tables_.end(),
        [index](const auto &candidate) { return candidate.get() == index; });
    assert(owned != tables_.end() &&
           "Payment prefix contains an index from another database");
#else
    (void)description;
#endif
    auto *table = static_cast<rust_sto_tpcc_ordered_index *>(index);
#ifndef NDEBUG
    assert(table->owner_ == this && !table->is_remote_ && description);
#endif
    return table;
  };

  auto *warehouse = unwrap(request.warehouse, "warehouse");
  auto *district = unwrap(request.district, "district");
  auto *customer = unwrap(request.customer, "customer");
  auto *customer_name = unwrap(request.customer_name, "customer name");
  mako_sto_tpcc_payment_prefix_request raw_request{
      warehouse->table_,
      district->table_,
      customer->table_,
      customer_name == nullptr ? nullptr : customer_name->table_,
      reinterpret_cast<const uint8_t *>(request.warehouse_key),
      reinterpret_cast<const uint8_t *>(request.district_key),
      reinterpret_cast<const uint8_t *>(request.customer_key_prefix),
      reinterpret_cast<const uint8_t *>(request.customer_name_lower),
      reinterpret_cast<const uint8_t *>(request.customer_name_upper),
      request.customer_id,
      request.payment_amount,
      request.customer_by_name ? uint32_t{1} : uint32_t{0},
      reinterpret_cast<uint8_t *>(request.warehouse_value),
      reinterpret_cast<uint8_t *>(request.district_value),
      reinterpret_cast<uint8_t *>(request.customer_value),
      tpcc_fixed_batch::payment_value_capacity};
  mako_sto_tpcc_payment_prefix_result raw_result{};
  const sto_tpcc_status status = mako_sto_tpcc_payment_prefix_trusted(
      tls_thread_, &raw_request, &raw_result);
  if (status != STO_TPCC_OK) {
    // The fused Rust operation aborts every failed attempt so partially
    // staged borrowed values can never survive an error return.
    tls_transaction_active_ = false;
    require_ok("TPC-C Payment prefix", status);
  }
  if (raw_result.warehouse_value_length == 0 ||
      raw_result.warehouse_value_length >
          tpcc_fixed_batch::payment_value_capacity ||
      raw_result.district_value_length == 0 ||
      raw_result.district_value_length >
          tpcc_fixed_batch::payment_value_capacity ||
      raw_result.customer_value_length == 0 ||
      raw_result.customer_value_length >
          tpcc_fixed_batch::payment_value_capacity ||
      raw_result.customer_id <= 0) {
    abort_current_transaction_noexcept();
    throw std::runtime_error(
        "Rust STO Payment prefix returned invalid metadata");
  }
  return {raw_result.warehouse_value_length,
          raw_result.district_value_length,
          raw_result.customer_value_length, raw_result.customer_id};
}

tpcc_fixed_batch::new_order_full_result
rust_sto_tpcc_wrapper::tx_new_order_full(
    const tpcc_fixed_batch::new_order_full_request &request) {
  require_txn_handle(request.txn);
  if (request.warehouse == nullptr || request.district == nullptr ||
      request.customer == nullptr || request.item == nullptr ||
      request.stock == nullptr || request.new_order == nullptr ||
      request.oorder == nullptr || request.oorder_c_id_idx == nullptr ||
      request.order_line == nullptr || request.item_ids == nullptr ||
      request.quantities == nullptr || request.warehouse_id <= 0 ||
      request.district_id <= 0 || request.customer_id <= 0 ||
      request.order_id <= 0 || request.line_count < 5 ||
      request.line_count > tpcc_fixed_batch::new_order_max_lines) {
    throw std::invalid_argument("invalid full TPC-C NewOrder request");
  }

  const auto unwrap = [&](abstract_ordered_index *index,
                          const char *description)
      -> rust_sto_tpcc_ordered_index * {
#ifndef NDEBUG
    const auto owned = std::find_if(
        tables_.begin(), tables_.end(),
        [index](const auto &candidate) { return candidate.get() == index; });
    assert(owned != tables_.end() &&
           "full NewOrder contains an index from another database");
#else
    (void)description;
#endif
    auto *table = static_cast<rust_sto_tpcc_ordered_index *>(index);
#ifndef NDEBUG
    assert(table->owner_ == this && !table->is_remote_ && description);
#endif
    return table;
  };

  auto *warehouse = unwrap(request.warehouse, "warehouse");
  auto *district = unwrap(request.district, "district");
  auto *customer = unwrap(request.customer, "customer");
  auto *item = unwrap(request.item, "item");
  auto *stock = unwrap(request.stock, "stock");
  auto *new_order = unwrap(request.new_order, "new order");
  auto *oorder = unwrap(request.oorder, "order");
  auto *oorder_c_id_idx = unwrap(request.oorder_c_id_idx, "order index");
  auto *order_line = unwrap(request.order_line, "order line");
  const mako_sto_tpcc_new_order_full_request raw_request{
      warehouse->table_,
      district->table_,
      customer->table_,
      item->table_,
      stock->table_,
      new_order->table_,
      oorder->table_,
      oorder_c_id_idx->table_,
      order_line->table_,
      request.item_ids,
      request.quantities,
      request.warehouse_id,
      request.district_id,
      request.customer_id,
      request.order_id,
      request.entry_date,
      request.line_count};
  mako_sto_tpcc_new_order_full_result raw_result{};
  const sto_tpcc_status status = mako_sto_tpcc_new_order_full_trusted(
      tls_thread_, &raw_request, &raw_result);

  // The Rust guard commits or aborts the attempt before every return.
  tls_transaction_active_ = false;
  if (status == STO_TPCC_RETRY)
    throw abstract_abort_exception();
  require_ok("full TPC-C NewOrder", status);
  if (raw_result.reported_value_bytes == 0) {
    throw std::runtime_error(
        "Rust STO full NewOrder returned invalid metadata");
  }
  return {raw_result.reported_value_bytes};
}

tpcc_fixed_batch::delivery_full_result
rust_sto_tpcc_wrapper::tx_delivery_full(
    const tpcc_fixed_batch::delivery_full_request &request) {
  require_txn_handle(request.txn);
  if (request.new_order == nullptr || request.oorder == nullptr ||
      request.order_line == nullptr || request.customer == nullptr ||
      request.last_no_o_ids == nullptr || request.warehouse_id <= 0 ||
      request.carrier_id <= 0 ||
      request.carrier_id >
          static_cast<int32_t>(tpcc_fixed_batch::delivery_district_count)) {
    throw std::invalid_argument("invalid full TPC-C Delivery request");
  }

  const auto unwrap = [&](abstract_ordered_index *index,
                          const char *description)
      -> rust_sto_tpcc_ordered_index * {
#ifndef NDEBUG
    const auto owned = std::find_if(
        tables_.begin(), tables_.end(),
        [index](const auto &candidate) { return candidate.get() == index; });
    assert(owned != tables_.end() &&
           "full Delivery contains an index from another database");
#else
    (void)description;
#endif
    auto *table = static_cast<rust_sto_tpcc_ordered_index *>(index);
#ifndef NDEBUG
    assert(table->owner_ == this && !table->is_remote_ && description);
#endif
    return table;
  };

  auto *new_order = unwrap(request.new_order, "new order");
  auto *oorder = unwrap(request.oorder, "order");
  auto *order_line = unwrap(request.order_line, "order line");
  auto *customer = unwrap(request.customer, "customer balance");
  const mako_sto_tpcc_delivery_full_request raw_request{
      new_order->table_,
      oorder->table_,
      order_line->table_,
      customer->table_,
      request.last_no_o_ids,
      request.warehouse_id,
      request.carrier_id,
      request.timestamp};
  mako_sto_tpcc_delivery_full_result raw_result{};
  const sto_tpcc_status status = mako_sto_tpcc_delivery_full_trusted(
      tls_thread_, &raw_request, &raw_result);

  // The Rust guard commits or aborts the attempt before every return. Cursor
  // writes are worker-local and intentionally remain visible after an abort.
  tls_transaction_active_ = false;
  if (status == STO_TPCC_RETRY)
    throw abstract_abort_exception();
  require_ok("full TPC-C Delivery", status);
  if (raw_result.reported_value_bytes != 0 ||
      raw_result.delivered_districts >
          tpcc_fixed_batch::delivery_district_count ||
      raw_result.updated_order_lines >
          raw_result.delivered_districts *
              tpcc_fixed_batch::delivery_max_lines_per_district) {
    throw std::runtime_error(
        "Rust STO full Delivery returned invalid metadata");
  }
  return {raw_result.reported_value_bytes,
          raw_result.delivered_districts,
          raw_result.updated_order_lines};
}

tpcc_fixed_batch::stock_level_full_result
rust_sto_tpcc_wrapper::tx_stock_level_full(
    const tpcc_fixed_batch::stock_level_full_request &request) {
  require_txn_handle(request.txn);
  if (request.order_line == nullptr || request.stock == nullptr ||
      request.warehouse_id <= 0 || request.district_id <= 0 ||
      request.district_id > 10 ||
      request.threshold > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("invalid full TPC-C StockLevel request");
  }

  const auto unwrap = [&](abstract_ordered_index *index,
                          const char *description)
      -> rust_sto_tpcc_ordered_index * {
#ifndef NDEBUG
    const auto owned = std::find_if(
        tables_.begin(), tables_.end(),
        [index](const auto &candidate) { return candidate.get() == index; });
    assert(owned != tables_.end() &&
           "full StockLevel contains an index from another database");
#else
    (void)description;
#endif
    auto *table = static_cast<rust_sto_tpcc_ordered_index *>(index);
#ifndef NDEBUG
    assert(table->owner_ == this && !table->is_remote_ && description);
#endif
    return table;
  };

  auto *order_line = unwrap(request.order_line, "order line");
  auto *stock = unwrap(request.stock, "stock");
  const mako_sto_tpcc_stock_level_full_request raw_request{
      order_line->table_,
      stock->table_,
      request.current_next_order_id,
      request.warehouse_id,
      request.district_id,
      request.threshold};
  mako_sto_tpcc_stock_level_full_result raw_result{};
  const sto_tpcc_status status = mako_sto_tpcc_stock_level_full_trusted(
      tls_thread_, &raw_request, &raw_result);

  // The Rust guard commits or aborts the attempt before every return.
  tls_transaction_active_ = false;
  if (status == STO_TPCC_RETRY)
    throw abstract_abort_exception();
  require_ok("full TPC-C StockLevel", status);
  if (raw_result.reported_value_bytes != 0 ||
      raw_result.scanned_order_line_rows >
          tpcc_fixed_batch::stock_level_max_order_line_rows ||
      raw_result.distinct_item_ids > raw_result.scanned_order_line_rows ||
      raw_result.low_stock_count > raw_result.distinct_item_ids) {
    throw std::runtime_error(
        "Rust STO full StockLevel returned invalid metadata");
  }
  return {raw_result.reported_value_bytes,
          raw_result.scanned_order_line_rows,
          raw_result.distinct_item_ids,
          raw_result.low_stock_count};
}

oi_fixed_put_result rust_sto_tpcc_wrapper::tx_insert_many(
    c_void *txn, const oi_insert_operation *operations,
    size_t operation_count) {
  require_txn_handle(txn);
  if (operation_count != 0 && operations == nullptr)
    throw std::invalid_argument(
        "insert batch requires operations for a nonempty batch");

  auto &raw_operations = tls_insert_operations_;
  raw_operations.resize(operation_count);
  for (size_t index = 0; index < operation_count; ++index) {
    const oi_insert_operation &operation = operations[index];
    if (operation.table == nullptr)
      throw std::invalid_argument("insert batch contains a null table");
#ifndef NDEBUG
    const auto owned = std::find_if(
        tables_.begin(), tables_.end(), [&](const auto &candidate) {
          return candidate.get() == operation.table;
        });
    assert(owned != tables_.end() &&
           "insert batch contains an index from another database");
#endif
    auto *table =
        static_cast<rust_sto_tpcc_ordered_index *>(operation.table);
    const std::string_view payload =
        canonical_local_mako_payload(operation.encoded_value);
    raw_operations[index] = {
        table->table_,
        reinterpret_cast<const uint8_t *>(operation.key.data()),
        operation.key.size(),
        reinterpret_cast<const uint8_t *>(payload.data()), payload.size()};
  }

  sto_tpcc_fixed_put_result raw_result{};
  const sto_tpcc_status status = sto_tpcc_insert_many(
      tls_thread_, raw_operations.empty() ? nullptr : raw_operations.data(),
      raw_operations.size(), &raw_result);
  if (status != STO_TPCC_OK && status != STO_TPCC_DUPLICATE)
    require_ok("insert batch", status);
  const bool has_duplicate =
      raw_result.first_duplicate != std::numeric_limits<size_t>::max();
  if (raw_result.inserted > operation_count ||
      (has_duplicate && raw_result.first_duplicate >= operation_count) ||
      ((status == STO_TPCC_DUPLICATE) != has_duplicate)) {
    abort_current_transaction_noexcept();
    throw std::runtime_error("Rust STO insert batch returned invalid metadata");
  }
  return {raw_result.inserted, raw_result.first_duplicate};
}

// @unsafe - Safe against a concurrent open_index: see tables_by_id_.
rust_sto_tpcc_ordered_index *rust_sto_tpcc_wrapper::table_by_id(
    int32_t table_id) const noexcept {
  if (table_id < 0 || table_id >= kTableIdLimit)
    return nullptr;
  return tables_by_id_[static_cast<size_t>(table_id)].load(
      rusty::sync::atomic::Ordering::Acquire);
}

abstract_ordered_index *rust_sto_tpcc_wrapper::get_index_by_table_id(
    unsigned short table_id) {
  return table_by_id(table_id);
}

abstract_ordered_index *rust_sto_tpcc_wrapper::open_index(
    const std::string &name, size_t, bool, bool) {
  return open_index(name, -1);
}

// @unsafe - Startup-only catalog publication and native C ABI table creation.
abstract_ordered_index *rust_sto_tpcc_wrapper::open_index(
    const std::string &name, int shard_index) {
  auto &config = BenchmarkConfig::getInstance();
  if (shard_index == -1)
    shard_index = static_cast<int>(config.getShardIndex());
  if (shard_index < 0 ||
      static_cast<size_t>(shard_index) >= config.getNshards())
    throw std::invalid_argument("Rust STO table shard is out of range");
  const bool remote = shard_index != static_cast<int>(config.getShardIndex());

  const auto key = std::make_tuple(name, shard_index);
  if (const auto found = tables_by_name_.find(key);
      found != tables_by_name_.end())
    return found->second;

  auto [next, inserted] = next_table_ids_.try_emplace(
      shard_index, shard_index * mako::NUM_TABLES_PER_SHARD + 1);
  int32_t table_id = next->second;
  if (remote) {
    std::string local_name = name;
    const size_t marker = name.rfind("_remote_");
    if (marker != std::string::npos) {
      size_t warehouse = 0;
      const char *first = name.data() + marker + sizeof("_remote_") - 1;
      const char *last = name.data() + name.size();
      const auto parsed = std::from_chars(first, last, warehouse);
      const size_t warehouses = static_cast<size_t>(config.getScaleFactor());
      if (parsed.ec != std::errc{} || parsed.ptr != last || warehouse == 0 ||
          warehouses == 0 || (warehouse - 1) / warehouses !=
              static_cast<size_t>(shard_index))
        throw std::invalid_argument("invalid Rust STO remote warehouse name");
      local_name.resize(marker);
      const size_t local_warehouse = (warehouse - 1) % warehouses;
      const size_t workers = config.getNthreads();
      const size_t partition = warehouses <= workers ? local_warehouse
          : std::min(local_warehouse / (warehouses / workers), workers - 1);
      const std::string partitioned =
          local_name + "_" + std::to_string(partition);
      if (tables_by_name_.find(std::make_tuple(
              partitioned, static_cast<int>(config.getShardIndex()))) !=
          tables_by_name_.end())
        local_name = partitioned;
    }
    const auto local = tables_by_name_.find(std::make_tuple(
        local_name, static_cast<int>(config.getShardIndex())));
    if (local != tables_by_name_.end())
      table_id = shard_index * mako::NUM_TABLES_PER_SHARD +
          (local->second->table_id_ - 1) % mako::NUM_TABLES_PER_SHARD + 1;
  }
  if (table_id > (shard_index + 1) * mako::NUM_TABLES_PER_SHARD ||
      table_id >= kTableIdLimit)
    throw std::length_error("Rust STO shard table ID capacity exhausted");
  if (auto *index = table_by_id(table_id)) {
    if (remote && index->is_remote_) {
      // Several warehouses can share one local physical tree. Their remote
      // aliases must name that same participant table, not consume extra IDs.
      tables_by_name_.emplace(key, index);
      return index;
    }
    if (!index->name_.empty())
      throw std::logic_error("Rust STO table ID is already present");
    tables_by_name_.emplace(key, index);
    index->name_ = name;
    // No engine handle is published: native placement resolves MBTA indexes.
    mako::get_table_registry().register_table(
        table_id, name, shard_index, !index->is_remote_);
    ++next->second;
    return index;
  }
  if (tables_.size() == tables_.max_size() ||
      tables_by_name_.size() == tables_by_name_.max_size())
    throw std::length_error("Rust STO table catalog capacity exhausted");

  // Allocate every container slot before asking Rust for a table. Once the
  // FFI handle exists, publication uses reserved vector capacity, the
  // preallocated ID slot and a detached map node. The catch block can restore
  // the exact catalog contents without allocating.
  tables_.reserve(tables_.size() + 1);
  const auto &id_slot = tables_by_id_[static_cast<size_t>(table_id)];

  decltype(tables_by_name_) staged_by_name;
  const auto staged_name_position = staged_by_name.emplace(key, nullptr);
  if (!staged_name_position.second)
    throw std::logic_error("could not stage Rust STO table name");
  auto staged_name = staged_by_name.extract(staged_name_position.first);

  const sto_tpcc_table_config table_config =
      rust_sto_tpcc_detail::table_config_for(name);
  const sto_tpcc_resolved_cache_policy cache_policy =
      tpcc_resolved_cache_policy_for(name);
  sto_tpcc_table *table = nullptr;
  if (!remote)
    require_ok("table_create_with_cache_policy",
               sto_tpcc_table_create_with_cache_policy(
                   db_, &table_config, cache_policy, &table));

  ffi_table_owner table_owner{table};
#ifdef MAKO_RUST_STO_TEST_HOOKS
  inject_open_index_fault_if_requested(
      rust_sto_tpcc_detail::open_index_fault_stage::after_table_create);
#endif

  auto index = rusty::make_box<rust_sto_tpcc_ordered_index>(
      this, table, table_id, name, remote);
  table_owner.release();
#ifdef MAKO_RUST_STO_TEST_HOOKS
  inject_open_index_fault_if_requested(
      rust_sto_tpcc_detail::open_index_fault_stage::after_facade_create);
#endif

  auto *raw = index.get();
  staged_name.mapped() = raw;

  bool table_inserted = false;
  bool id_inserted = false;
  bool name_inserted = false;
  decltype(tables_by_name_)::iterator name_position = tables_by_name_.end();
  try {
    tables_.push_back(std::move(index));
    table_inserted = true;
#ifdef MAKO_RUST_STO_TEST_HOOKS
    inject_open_index_fault_if_requested(
        rust_sto_tpcc_detail::open_index_fault_stage::after_tables_insert);
#endif

    if (id_slot.compare_exchange(nullptr, raw,
                                 rusty::sync::atomic::Ordering::AcqRel,
                                 rusty::sync::atomic::Ordering::Acquire)
            .is_err())
      throw std::logic_error("Rust STO table ID insertion conflicted");
    id_inserted = true;
#ifdef MAKO_RUST_STO_TEST_HOOKS
    inject_open_index_fault_if_requested(
        rust_sto_tpcc_detail::open_index_fault_stage::after_id_insert);
#endif

    auto name_result = tables_by_name_.insert(std::move(staged_name));
    if (!name_result.inserted)
      throw std::logic_error("Rust STO table name insertion conflicted");
    name_position = name_result.position;
    name_inserted = true;
#ifdef MAKO_RUST_STO_TEST_HOOKS
    inject_open_index_fault_if_requested(
        rust_sto_tpcc_detail::open_index_fault_stage::after_name_insert);
#endif
  } catch (...) {
    if (name_inserted)
      tables_by_name_.erase(name_position);
    if (id_inserted)
      id_slot.store(nullptr, rusty::sync::atomic::Ordering::Release);
    if (table_inserted) {
      assert(tables_.back().get() == raw);
      tables_.pop_back();
    }
    throw;
  }

  // No engine handle is published: native placement resolves MBTA indexes.
  mako::get_table_registry().register_table(
      table_id, name, shard_index, !remote);
  if (!remote)
    mako::setup_update_table(table_id, raw);
  next->second = std::max(next->second, table_id + 1);
  return raw;
}

void rust_sto_tpcc_wrapper::close_index(abstract_ordered_index *idx) {
  if (idx == nullptr)
    return;

  const auto owned = std::find_if(
      tables_.begin(), tables_.end(), [&](const auto &candidate) {
        return candidate.get() == idx;
      });
  if (owned == tables_.end())
    throw std::invalid_argument(
        "Rust STO close_index received an index from another database");

  auto *table = owned->get();
  tables_by_id_[static_cast<size_t>(table->table_id_)].store(
      nullptr, rusty::sync::atomic::Ordering::Release);
  for (auto by_name = tables_by_name_.begin();
       by_name != tables_by_name_.end();) {
    if (by_name->second == table)
      by_name = tables_by_name_.erase(by_name);
    else
      ++by_name;
  }
  tables_.erase(owned);
}

// @unsafe - Participant cleanup never sends coordinator RPCs.
void rust_sto_tpcc_wrapper::shard_abort_txn(void *) {
  abort_current_transaction_noexcept();
}

// @unsafe - Native participant validation seals its operation phase.
int rust_sto_tpcc_wrapper::shard_validate() {
  const auto status = sto_tpcc_protocol_validate(tls_thread_);
  if (status == STO_TPCC_RETRY)
    return mako::ErrorCode::ABORT;
  require_ok("participant validate", status);
  return mako::ErrorCode::SUCCESS;
}

// @unsafe - Irreversible installation; failures may never become retry.
void rust_sto_tpcc_wrapper::shard_install(uint32_t timestamp) {
  require_ok("participant install",
             sto_tpcc_protocol_install(tls_thread_, timestamp));
}

// @unsafe - Rust serializes its own final write ledger, not C++ TransItems.
void rust_sto_tpcc_wrapper::shard_serialize_util(uint32_t) {
  require_ok("participant log", sto_tpcc_protocol_log(
      tls_thread_, mako::getCurrentTimeMillis(), protocol_callback, this));
}

// @unsafe - Consumes the retained participant and resolves committed cleanup.
void rust_sto_tpcc_wrapper::shard_unlock(bool committed) {
  sto_tpcc_protocol_report report{};
  const auto status = sto_tpcc_protocol_finish(tls_thread_, committed, &report);
  tls_transaction_active_ = false;
  require_ok("participant finish", status);
}

// @unsafe - Resolve the preceding affine attempt before admitting another.
void rust_sto_tpcc_wrapper::shard_reset() {
  abort_current_transaction_noexcept();
  begin_current_transaction();
}

// @unsafe - RPC buffers are copied by Rust before returning to the server.
void rust_sto_tpcc_wrapper::shard_mutate(
    uint16_t table_id, lcdf::Str key, const std::string &value,
    uint8_t operation) {
  auto *index = static_cast<rust_sto_tpcc_ordered_index *>(
      get_index_by_table_id(table_id));
  if (!index || index->is_remote_)
    throw std::invalid_argument("invalid Rust STO participant table");
  mutate_protocol(*index, key, value, operation, true);
}

// @unsafe - Read-only affine FFI inspection; failure is conservatively active.
bool rust_sto_tpcc_wrapper::shard_participant_active() const noexcept {
  if (!tls_thread_)
    return false;
  if (!tls_protocol_)
    return tls_transaction_active_;
  sto_tpcc_protocol_report report{};
  uint32_t empty = 0;
  if (sto_tpcc_protocol_state(tls_thread_, &report, &empty) != STO_TPCC_OK)
    return true;
  return report.phase != 0;
}

// @unsafe - Unknown state must not be parked as empty.
bool rust_sto_tpcc_wrapper::shard_participant_empty() const noexcept {
  sto_tpcc_protocol_report report{};
  uint32_t empty = 0;
  return tls_thread_ && tls_protocol_ &&
      sto_tpcc_protocol_state(tls_thread_, &report, &empty) == STO_TPCC_OK &&
      empty != 0;
}

// @unsafe - Same process Rust authority serves coordinator and timestamp RPC.
bool rust_sto_tpcc_wrapper::shard_timestamp(uint32_t &timestamp) {
  const auto status = tls_thread_ && tls_protocol_
      ? sto_tpcc_protocol_timestamp(tls_thread_, &timestamp)
      : sto_tpcc_distributed_timestamp(&timestamp);
  if (status == STO_TPCC_RESOURCE_EXHAUSTED)
    return false;
  require_ok("distributed timestamp", status);
  return true;
}

// @unsafe - Admission has already checked request identity before this hook.
void rust_sto_tpcc_wrapper::shard_set_term(uint32_t term) {
  if (term > 9)
    throw std::invalid_argument("Rust STO decimal term out of range");
  if (tls_term_ == term)
    return;
  const bool active = shard_participant_active();
  if (active && !shard_participant_empty())
    throw std::logic_error("term changed inside a Rust STO participant");
  if (active)
    abort_current_transaction_noexcept();
  tls_term_ = term;
  if (active)
    begin_current_transaction();
}

// @unsafe - Replay owns its native worker and never initializes a shard client.
void rust_sto_tpcc_wrapper::replay_thread_init() { thread_init(true, 0); }

// @unsafe - Legacy partition is thread-affine log routing state.
void rust_sto_tpcc_wrapper::replay_thread_select_partition(int partition) {
  TThread::set_pid(partition);
}

// @unsafe - Observe recovered decisions before foreground promotion.
void rust_sto_tpcc_wrapper::observe_replay_timestamp(uint32_t time_term) {
  if (time_term != 0)
    require_ok("replay clock floor", sto_tpcc_observe_timestamp(time_term));
}

// @unsafe - Replay FFI copies key/payload and consumes its private attempt.
bool rust_sto_tpcc_wrapper::replay_apply_record(
    uint16_t table_id, std::string_view key, std::string_view payload,
    bool is_delete, uint32_t source_time_term) {
  auto *index = static_cast<rust_sto_tpcc_ordered_index *>(
      get_index_by_table_id(table_id));
  if (!index || !index->table_)
    throw std::invalid_argument("invalid Rust STO replay table");
  if (TThread::should_reclaim())
    require_ok("replay history floor", sto_tpcc_table_history_floor(
        index->table_, sync_util::sync_logger::retrieveShardW() / 10));
  const auto status = sto_tpcc_replay_apply(
      tls_thread_, index->table_,
      reinterpret_cast<const uint8_t *>(key.data()), key.size(),
      reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
      is_delete, source_time_term);
  if (status == STO_TPCC_RETRY)
    return false;
  require_ok("replay apply", status);
  return true;
}

[[noreturn]] void rust_sto_tpcc_wrapper::throw_fatal(
    const char *operation, sto_tpcc_status status) {
  const std::string message = std::string("Rust STO ") + operation +
                             " failed (status " + std::to_string(status) +
                             "): " + last_rust_error();
  if (status == STO_TPCC_UNKNOWN || status == STO_TPCC_COMMITTED_ERROR ||
      (tls_protocol_ && status == STO_TPCC_FATAL)) {
    // Legacy benchmark catch-all handlers retry. Publication uncertainty or
    // committed cleanup failure must instead stop this process.
    mako::benchmark_cerr() << message << std::endl;
    std::terminate();
  }
  if (status == STO_TPCC_RESOURCE_EXHAUSTED) {
    if (tls_protocol_ && tls_thread_) {
      if (TThread::mode() == 0 && TThread::sclient &&
          TThread::sclient->hasTouchedParticipants() &&
          TThread::sclient->remoteAbort() != mako::ErrorCode::SUCCESS)
        std::terminate();
      sto_tpcc_protocol_report report{};
      if (sto_tpcc_protocol_finish(tls_thread_, 0, &report) != STO_TPCC_OK)
        std::terminate();
    }
    tls_transaction_active_ = false;
    throw storage_resource_exhausted(message);
  }
  throw std::runtime_error(message);
}

void rust_sto_tpcc_wrapper::require_ok(const char *operation,
                                       sto_tpcc_status status) {
  if (status == STO_TPCC_OK)
    return;
  if (status == STO_TPCC_RETRY)
    throw abstract_abort_exception();
  throw_fatal(operation, status);
}

void rust_sto_tpcc_wrapper::require_txn_handle(c_void *txn) {
  if (!tls_thread_ || txn != tls_thread_ || !tls_transaction_active_)
    throw std::runtime_error("invalid or inactive Rust STO transaction handle");
}

// @unsafe - Begin only Rust state; the inactive C++ transaction is not started.
void rust_sto_tpcc_wrapper::begin_current_transaction() {
  if (!tls_thread_)
    throw std::runtime_error("Rust STO operation on an unattached thread");
  if (tls_transaction_active_)
    throw std::runtime_error("nested Rust STO transaction");
  tls_protocol_ = tls_owner_->protocol_mode_ || TThread::mode() == 1;
  if (tls_protocol_) {
    if (TThread::mode() == 0) {
      const int term = BenchmarkConfig::getInstance().getIsReplicated()
          ? get_epoch() : 0;
      if (term < 0 || term > 9)
        throw std::invalid_argument("Rust STO decimal term out of range");
      tls_term_ = static_cast<uint32_t>(term);
      if (TThread::sclient)
        TThread::sclient->beginAttempt(tls_term_);
    }
    require_ok("protocol begin", sto_tpcc_protocol_begin(tls_thread_, tls_term_));
    const auto history = sync_util::sync_logger::history_snapshot();
    const uint32_t failure_bound =
        sync_util::sync_logger::failed_shard_ts.load(std::memory_order_acquire);
    const bool bounded_failure =
        BenchmarkConfig::getInstance().getControlMode() == 1 &&
        failure_bound != std::numeric_limits<uint32_t>::max();
    require_ok("protocol visibility", sto_tpcc_protocol_visibility(
        tls_thread_, history.values.data(), history.known_mask,
        bounded_failure ? failure_bound : 0, bounded_failure));
  } else {
    require_ok("txn_begin", mako_sto_tpcc_txn_begin_trusted(tls_thread_));
  }
  tls_transaction_active_ = true;
}

// @unsafe - Consume affine state; uncertain/failed committed cleanup fail-stops.
void rust_sto_tpcc_wrapper::abort_current_transaction_noexcept() {
  if (!tls_thread_ || !tls_transaction_active_)
    return;
  if (tls_protocol_) {
    sto_tpcc_protocol_report report{};
    const auto status = sto_tpcc_protocol_finish(tls_thread_, 0, &report);
    if (status != STO_TPCC_OK)
      std::terminate();
  } else {
    const auto status = sto_tpcc_txn_abort(tls_thread_);
    if (status == STO_TPCC_UNKNOWN || status == STO_TPCC_COMMITTED_ERROR)
      std::terminate();
  }
  tls_transaction_active_ = false;
}

bool rust_sto_tpcc_wrapper::get_raw(const sto_tpcc_table *table,
                                    lcdf::Str key,
                                    std::string &encoded_value,
                                    size_t max_bytes_read) {
  require_txn_handle(tls_thread_);
  return get_active_raw(table, key, encoded_value, max_bytes_read);
}

// @unsafe - Copy one coherent Rust payload/metadata result to legacy storage.
bool rust_sto_tpcc_wrapper::get_active_raw(
    const sto_tpcc_table *table, lcdf::Str key,
    std::string &encoded_value, size_t max_bytes_read) {
  sto_tpcc_status status = STO_TPCC_OK;
  size_t actual = 0;
  tls_last_read_time_term_ = 0;
  const auto read_value = [&](uint8_t *output, size_t capacity) {
    return tls_protocol_
        ? sto_tpcc_protocol_get(
              tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
              key.length(), output, capacity, &actual,
              &tls_last_read_time_term_)
        : mako_sto_tpcc_get_trusted(
              tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
              key.length(), output, capacity, &actual);
  };

  // A zero-byte read still observes the row and distinguishes presence from
  // absence, but needs no temporary value allocation. The Rust ABI reports a
  // live nonempty row as BUFFER_TOO_SMALL with its exact logical length.
  if (max_bytes_read == 0) {
    encoded_value.clear();
    status = read_value(nullptr, 0);
    if (status == STO_TPCC_MISS)
      return false;
    if (status == STO_TPCC_OK || status == STO_TPCC_BUFFER_TOO_SMALL)
      return true;
    require_ok("get", status);
    return true;
  }

  const auto fetch = [&](size_t requested) {
    encoded_value.resize_and_overwrite(requested, [&](char *output,
                                                       size_t capacity) {
      status = read_value(reinterpret_cast<uint8_t *>(output), capacity);
      return status == STO_TPCC_OK ? actual : size_t{0};
    });
  };

  // TPC-C's reusable string arena normally retains at least this much space.
  // Copy directly into that final allocation instead of first filling a 1 KiB
  // stack buffer and assigning it into the string.
  fetch(std::max(encoded_value.capacity(), size_t{128}));
  if (status == STO_TPCC_MISS) {
    return false;
  }
  if (status == STO_TPCC_BUFFER_TOO_SMALL) {
    fetch(actual);
    require_ok("get", status);
  } else {
    require_ok("get", status);
  }
  if (encoded_value.size() > max_bytes_read)
    encoded_value.resize(max_bytes_read);
  return true;
}

void rust_sto_tpcc_wrapper::put_raw(const sto_tpcc_table *table,
                                    lcdf::Str key,
                                    const std::string &encoded_value,
                                    bool insert_only) {
  require_txn_handle(tls_thread_);
  const std::string_view payload =
      canonical_local_mako_payload(encoded_value);
  const sto_tpcc_status status =
      insert_only
          ? mako_sto_tpcc_insert_trusted(
                tls_thread_, table,
                reinterpret_cast<const uint8_t *>(key.data()), key.length(),
                reinterpret_cast<const uint8_t *>(payload.data()),
                payload.size())
          : mako_sto_tpcc_put_trusted(
                tls_thread_, table,
                reinterpret_cast<const uint8_t *>(key.data()), key.length(),
                reinterpret_cast<const uint8_t *>(payload.data()),
                payload.size());
  if (status == STO_TPCC_DUPLICATE && insert_only)
    return;
  require_ok(insert_only ? "insert" : "put", status);
}

void rust_sto_tpcc_wrapper::put_active_raw(
    const sto_tpcc_table *table, lcdf::Str key,
    const std::string &encoded_value, bool insert_only) {
  const std::string_view payload =
      canonical_local_mako_payload(encoded_value);
  // TxnOrderedIndex requires an encoded PUT value to retain its address and
  // bytes through transaction finish. The Rust intent may therefore borrow a
  // bounded PUT payload. Keep INSERT owning: legacy TPC-C loaders reuse one
  // encoding buffer across a transaction despite the stronger interface
  // comment, and synchronous copying preserves that established behavior.
  const sto_tpcc_status status =
      insert_only
          ? mako_sto_tpcc_insert_trusted(
                tls_thread_, table,
                reinterpret_cast<const uint8_t *>(key.data()), key.length(),
                reinterpret_cast<const uint8_t *>(payload.data()),
                payload.size())
          : mako_sto_tpcc_put_borrowed_trusted(
                tls_thread_, table,
                reinterpret_cast<const uint8_t *>(key.data()), key.length(),
                reinterpret_cast<const uint8_t *>(payload.data()),
                payload.size());
  // MassTrans::transInsert returns "already existed" without modifying the
  // row; the legacy abstract wrapper discards that return value.  Preserve
  // that validated no-op behavior here.  The separate nontransactional
  // insert() surface still reports a duplicate as `false` to its caller.
  if (status == STO_TPCC_DUPLICATE && insert_only)
    return;
  if (status != STO_TPCC_OK)
    abort_current_transaction_noexcept();
  require_ok(insert_only ? "insert" : "put", status);
}

bool rust_sto_tpcc_wrapper::remove_raw(const sto_tpcc_table *table,
                                       lcdf::Str key) {
  require_txn_handle(tls_thread_);
  return remove_active_raw(table, key);
}

bool rust_sto_tpcc_wrapper::remove_active_raw(const sto_tpcc_table *table,
                                              lcdf::Str key) {
  const sto_tpcc_status status = sto_tpcc_remove(
      tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
      key.length());
  if (status == STO_TPCC_MISS)
    return false;
  require_ok("remove", status);
  return true;
}

void rust_sto_tpcc_wrapper::scan_raw(const sto_tpcc_table *table,
                                     bool reverse,
                                     const std::string &start_key,
                                     const std::string *end_key,
                                     oi_scan_callback &callback,
                                     bool strip_value_metadata) {
  require_txn_handle(tls_thread_);
  scan_active_raw(table, reverse, start_key, end_key, callback,
                  strip_value_metadata);
}

// @unsafe - Rust keeps row borrows live for the synchronous callback only.
void rust_sto_tpcc_wrapper::scan_active_raw(
    const sto_tpcc_table *table, bool reverse,
    const std::string &start_key, const std::string *end_key,
    oi_scan_callback &callback, bool strip_value_metadata) {
  if (table == nullptr)
    throw std::invalid_argument(
        "remote range wire returns one median value; use tx_scan_remote_one");
  scan_bridge bridge{&callback, !strip_value_metadata, {}, {}};
  size_t visited = 0;
  const auto *start = reinterpret_cast<const uint8_t *>(start_key.data());
  const auto *end = end_key
                        ? reinterpret_cast<const uint8_t *>(end_key->data())
                        : nullptr;
  const auto direction = reverse ? STO_TPCC_SCAN_REVERSE : STO_TPCC_SCAN_FORWARD;
  const auto lower_kind = reverse
      ? (end_key ? STO_TPCC_BOUND_EXCLUDED : STO_TPCC_BOUND_UNBOUNDED)
      : STO_TPCC_BOUND_INCLUDED;
  const auto upper_kind = reverse ? STO_TPCC_BOUND_INCLUDED
      : (end_key ? STO_TPCC_BOUND_EXCLUDED : STO_TPCC_BOUND_UNBOUNDED);
  const auto *lower = reverse ? end : start;
  const auto *upper = reverse ? start : end;
  const size_t lower_size = reverse ? (end_key ? end_key->size() : 0)
                                    : start_key.size();
  const size_t upper_size = reverse ? start_key.size()
                                    : (end_key ? end_key->size() : 0);
  const sto_tpcc_status status = tls_protocol_
      ? sto_tpcc_protocol_scan(
            tls_thread_, table, direction, lower_kind, lower, lower_size,
            upper_kind, upper, upper_size, callback.max_records_hint(),
            invoke_versioned_scan_bridge, &bridge, &visited)
      : mako_sto_tpcc_scan_trusted(
            tls_thread_, table, direction, lower_kind, lower, lower_size,
            upper_kind, upper, upper_size, callback.max_records_hint(),
            invoke_scan_bridge, &bridge, &visited);
  if (bridge.failure)
    std::rethrow_exception(bridge.failure);
  require_ok(reverse ? "reverse scan" : "scan", status);
}

// @unsafe - Copy RPC-owned bytes and arena-owned protocol intents into Rust.
void rust_sto_tpcc_wrapper::mutate_protocol(
    const rust_sto_tpcc_ordered_index &index, lcdf::Str key,
    const std::string &value, uint8_t operation, bool lock_now) {
  const std::string_view payload = operation == 2
      ? std::string_view{} : canonical_local_mako_payload(value);
  const auto *key_bytes = reinterpret_cast<const uint8_t *>(key.data());
  const auto *value_bytes = reinterpret_cast<const uint8_t *>(payload.data());
  const bool remote = index.routes_remotely();
  if (!remote && TThread::should_reclaim())
    require_ok("history floor", sto_tpcc_table_history_floor(
        index.table_, sync_util::sync_logger::retrieveShardW() / 10));
  const auto status = remote
      ? sto_tpcc_protocol_stage_remote(
            tls_thread_, (index.table_id_ - 1) / mako::NUM_TABLES_PER_SHARD,
            index.table_id_, operation, key_bytes, key.length(),
            value_bytes, payload.size())
      : sto_tpcc_protocol_mutate(
            tls_thread_, index.table_, index.table_id_, operation,
            key_bytes, key.length(), value_bytes, payload.size(), lock_now);
  if ((status == STO_TPCC_DUPLICATE && operation == 1) ||
      (status == STO_TPCC_MISS && operation == 2))
    return;
  require_ok("protocol mutation", status);
  if (remote)
    require_ok("remote incremental lock", sto_tpcc_protocol_flush_remote(
        tls_thread_, protocol_callback, tls_owner_));
}

// @unsafe - Legacy response bytes are copied before releasing the RPC buffer.
bool rust_sto_tpcc_wrapper::remote_get(
    const rust_sto_tpcc_ordered_index &index, lcdf::Str key,
    std::string &value, size_t max_bytes_read) {
  if (!TThread::sclient)
    throw std::logic_error("Rust STO remote read has no shard client");
  require_ok("flush remote read", sto_tpcc_protocol_flush_remote(
      tls_thread_, protocol_callback, tls_owner_));
  const int status = TThread::sclient->remoteGet(
      index.table_id_, std::string(key.data(), key.length()), value);
  if (status != mako::ErrorCode::SUCCESS &&
      status != mako::ErrorCode::NOT_FOUND)
    throw abstract_abort_exception();
  uint32_t time_term = 0;
  if (status == mako::ErrorCode::SUCCESS) {
    const auto payload = canonical_local_mako_payload(value);
    time_term = mako::load_value_time_term(value.data(), value.size());
    value.resize(std::min(payload.size(), max_bytes_read));
  } else {
    value.clear();
  }
  require_ok("remote read dependency", sto_tpcc_protocol_observe_read(
      tls_thread_, (index.table_id_ - 1) / mako::NUM_TABLES_PER_SHARD, time_term));
  return status == mako::ErrorCode::SUCCESS;
}

// @unsafe - The only coordinator bridge: Rust drives phases and final writes;
// this callback translates legacy transport and asynchronous log submission.
sto_tpcc_status rust_sto_tpcc_wrapper::protocol_callback(
    void *, sto_tpcc_protocol_request *request) noexcept {
  try {
    int result = mako::ErrorCode::SUCCESS;
    switch (request->action) {
    case STO_TPCC_PROTOCOL_STAGE: {
      if (!TThread::sclient)
        return STO_TPCC_FATAL;
      // These std containers are the existing ShardClient wire API boundary.
      std::vector<int> tables{request->table_id};
      std::vector<std::string> keys{
          std::string(reinterpret_cast<const char *>(request->key),
                      request->key_length)};
      std::string value;
      if (request->value_length)
        value.assign(reinterpret_cast<const char *>(request->value),
                     request->value_length);
      append_mako_metadata(value, 0);
      std::vector<std::string> values{std::move(value)};
      std::vector<uint8_t> operations{request->operation};
      result = TThread::sclient->remoteBatchLock(
          tables, keys, values, &operations);
      break;
    }
    case STO_TPCC_PROTOCOL_TIMESTAMP:
      if (TThread::sclient && TThread::sclient->hasTouchedParticipants())
        result = TThread::sclient->remoteGetTimestamp(request->timestamp);
      break;
    case STO_TPCC_PROTOCOL_VALIDATE:
      if (TThread::sclient && TThread::sclient->hasTouchedParticipants()) {
        uint32_t watermark = 0;
        result = TThread::sclient->remoteValidate(watermark);
      }
      break;
    case STO_TPCC_PROTOCOL_INSTALL:
      if (TThread::sclient && TThread::sclient->hasTouchedParticipants())
        result = TThread::sclient->remoteInstall(request->timestamp);
      if (result != mako::ErrorCode::SUCCESS)
        return STO_TPCC_UNKNOWN;
      break;
    case STO_TPCC_PROTOCOL_ABORT:
      if (TThread::sclient && TThread::sclient->hasTouchedParticipants())
        result = TThread::sclient->remoteAbort();
      if (result != mako::ErrorCode::SUCCESS)
        return STO_TPCC_FATAL;
      break;
    case STO_TPCC_PROTOCOL_LOG: {
      if (tls_suppress_log_ ||
          !BenchmarkConfig::getInstance().getIsReplicated())
        return STO_TPCC_OK;
      if (request->value_length > static_cast<size_t>(INT_MAX))
        return STO_TPCC_COMMITTED_ERROR;
      const auto *bytes = reinterpret_cast<const char *>(request->value);
      const int local_partition = TThread::getLocalPartitionID();
      const int replication_partition =
          BenchmarkConfig::getInstance().getReplicationPartition(
              TThread::get_shard_index(), local_partition);
      if (!add_log_to_nc(bytes, static_cast<int>(request->value_length),
                         replication_partition, 1))
        return STO_TPCC_COMMITTED_ERROR;
#ifndef DISABLE_DISK
      const uint32_t time_term = request->timestamp;
      const int watermark_partition =
          BenchmarkConfig::getInstance().getReplicationWatermarkPartition(
              TThread::get_shard_index(), local_partition);
      mako::RocksDBPersistence::getInstance().persistAsync(
          bytes, request->value_length, TThread::get_shard_index(), local_partition,
          [watermark_partition, time_term](bool success) {
            if (success)
              sync_util::sync_logger::updateDiskTimestamp(watermark_partition, time_term);
            else
              std::terminate();
          });
#endif
      return STO_TPCC_OK;
    }
    default:
      return STO_TPCC_FATAL;
    }
    return result == mako::ErrorCode::SUCCESS ? STO_TPCC_OK : STO_TPCC_RETRY;
  } catch (...) {
    if (request->action == STO_TPCC_PROTOCOL_INSTALL)
      return STO_TPCC_UNKNOWN;
    if (request->action == STO_TPCC_PROTOCOL_LOG)
      return STO_TPCC_COMMITTED_ERROR;
    return STO_TPCC_FATAL;
  }
}
