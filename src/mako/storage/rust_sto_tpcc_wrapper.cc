#include "rust_sto_tpcc_wrapper.hh"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "rcu.h"
#include "mbta_wrapper.hh"

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

mbta_wrapper &legacy_thread_support() {
  // TPC-C contains a few direct TThread reads outside abstract_db. Reuse the
  // native wrapper's established thread bring-up so those application-level
  // fields and its one-shard ShardClient retain identical values. No native
  // C++ table is allocated through this support object.
  static mbta_wrapper support;
  return support;
}

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

constexpr sto_tpcc_table_config
tpcc_table_config_for(std::string_view index_name) {
  constexpr uint64_t kMiB = uint64_t{1} << 20;
  constexpr uint64_t kStaticRetained = 262'144;
  constexpr uint64_t kStaticConsumed = 524'288;
  constexpr uint64_t kGrowthRetained = 4'000'000;
  constexpr uint64_t kGrowthConsumed = 6'000'000;
  constexpr uint64_t kAppendHeavyRetained = 16'000'000;
  constexpr uint64_t kAppendHeavyConsumed = 20'000'000;

  // Paper-style TPC-C forces one tree per warehouse, so increasing the
  // 1/4/8/16-thread scale increases the table count rather than one
  // table's initial cardinality. The largest initial table is order_line at
  // about 300k rows per warehouse. Its exact encoded key is 16 bytes; the
  // 512 MiB key quota therefore still has a 2x margin at the 16M row cap.
  // The remaining 15.7M order-line slots cover over 1.04M maximum-size
  // (15-line) new-order transactions per warehouse after loading.
  //
  // History starts with 30k rows per warehouse and appends one 24-byte key for
  // every committed Payment. The generic 4M tier therefore failed after
  // exactly 3.97M pure-Payment commits, within a normal benchmark run. Its
  // 16M retained-record tier needs 384M key bytes and thus also fits this
  // quota, while the 20M consumed-ID allowance retains collision headroom.
  const std::string_view tablespace = tpcc_tablespace_name(index_name);
  const bool static_cardinality = tpcc_table_has_static_directory(index_name);

  sto_tpcc_table_config config{};
  if (tablespace == "order_line" || tablespace == "history") {
    config.max_retained_records = kAppendHeavyRetained;
    config.max_consumed_record_ids = kAppendHeavyConsumed;
    config.max_retained_key_bytes = 512 * kMiB;
  } else if (static_cardinality) {
    config.max_retained_records = kStaticRetained;
    config.max_consumed_record_ids = kStaticConsumed;
    config.max_retained_key_bytes = 128 * kMiB;
  } else {
    // new_order, oorder, and oorder_c_id_idx each consume at most one new key
    // per corresponding TPC-C transaction. Keep this conservative tier as the
    // fallback so an unexpected table name fails by a clear bound instead of
    // silently receiving an unbounded registry.
    config.max_retained_records = kGrowthRetained;
    config.max_consumed_record_ids = kGrowthConsumed;
    config.max_retained_key_bytes = 512 * kMiB;
  }

  // LazySegmented reserves one 40-byte OnceLock directory cell per 1,024
  // consumed IDs and allocates the 64-byte registry entries only as segments
  // are used.
  // At 18 warehouses these tiers reserve about 41.1 MiB of directory cells,
  // versus about 79.0 MiB for the former uniform 8M-ID limit.
  config.scan_chunk_records = 128;
  config.scan_initial_key_arena_bytes = 16 * 1024;
  config.scan_max_key_arena_bytes = 64 * 1024;
  config.max_scan_chunks = 32'768;
  config.max_scan_physical_records = 4'000'000;
  // Only standard TPC-C range-scan tables pay the table-wide value-generation
  // increment on committed row publication. Point-only tables keep the write
  // path free of this shared atomic RMW.
  config.trusted_scan_value_generation =
      tablespace == "customer_name_idx" || tablespace == "oorder_c_id_idx" ||
      tablespace == "new_order" || tablespace == "order_line";
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

constexpr sto_tpcc_table_config kHistoryCapacityRegression =
    tpcc_table_config_for("history_1");
static_assert(kHistoryCapacityRegression.max_retained_records == 16'000'000);
static_assert(kHistoryCapacityRegression.max_consumed_record_ids ==
              20'000'000);
static_assert(kHistoryCapacityRegression.max_retained_key_bytes >=
              kHistoryCapacityRegression.max_retained_records * uint64_t{24});

constexpr sto_tpcc_table_config kOrderCapacityRegression =
    tpcc_table_config_for("oorder_1");
static_assert(kOrderCapacityRegression.max_retained_records == 4'000'000);
static_assert(kOrderCapacityRegression.max_consumed_record_ids == 6'000'000);
static_assert(kOrderCapacityRegression.trusted_scan_value_generation == 0);
static_assert(tpcc_table_config_for("oorder_c_id_idx_1")
                  .trusted_scan_value_generation != 0);
static_assert(
    tpcc_table_config_for("warehouse_1").trusted_scan_value_generation == 0);
static_assert(tpcc_table_config_for("warehouse_1").bounded_atomic_values != 0);
static_assert(tpcc_table_config_for("customer_1").bounded_atomic_values != 0);
static_assert(tpcc_table_config_for("district_1").bounded_atomic_values != 0);
static_assert(
    tpcc_table_config_for("customer_name_idx_1").bounded_atomic_values == 0);

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

// This wrapper is intentionally limited to one local, non-replicated shard
// (enforced by init()). It therefore never consumes Mako's timestamp/Node
// suffix. The C++ transactional ABI still supplies mako::Encode()d values, so
// its supported representation is deliberately narrow and lossless:
//
//   logical payload || EXTRA_BITS_FOR_VALUE zero bytes
//
// Strip that canonical suffix before crossing into Rust. Fail closed on a
// short or nonzero suffix so meaningful native/distributed metadata is never
// silently discarded. Raw shard reads reconstruct the same canonical zeros.
std::string_view canonical_local_mako_payload(std::string_view encoded_value) {
  if (encoded_value.size() < kMakoValueMetadataBytes)
    throw std::invalid_argument(
        "Rust STO received a truncated Mako metadata suffix");

  const size_t payload_size = encoded_value.size() - kMakoValueMetadataBytes;
  const std::string_view metadata = encoded_value.substr(payload_size);
  if (!std::all_of(metadata.begin(), metadata.end(),
                   [](char byte) { return byte == '\0'; }))
    throw std::invalid_argument(
        "Rust STO does not support nonzero Mako metadata");
  return encoded_value.substr(0, payload_size);
}

void append_canonical_mako_metadata(std::string &payload) {
  payload.append(kMakoValueMetadataBytes, '\0');
}

struct scan_bridge {
  oi_scan_callback *callback;
  bool append_metadata;
  std::string canonical_value;
  std::exception_ptr failure;
};

int32_t invoke_scan_bridge(void *context, const uint8_t *key,
                           size_t key_length, const uint8_t *value,
                           size_t value_length) {
  auto *bridge = static_cast<scan_bridge *>(context);
  try {
    const char *callback_value = reinterpret_cast<const char *>(value);
    size_t callback_value_length = value_length;
    if (bridge->append_metadata) {
      bridge->canonical_value.clear();
      if (value_length != 0)
        bridge->canonical_value.assign(callback_value, value_length);
      append_canonical_mako_metadata(bridge->canonical_value);
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

sto_tpcc_table_config
rust_sto_tpcc_detail::table_config_for(std::string_view index_name) {
  return tpcc_table_config_for(index_name);
}

bool rust_sto_tpcc_detail::table_has_static_directory(
    std::string_view index_name) {
  return tpcc_table_has_static_directory(index_name);
}

thread_local sto_tpcc_thread *rust_sto_tpcc_wrapper::tls_thread_ = nullptr;
thread_local bool rust_sto_tpcc_wrapper::tls_transaction_active_ = false;
thread_local bool rust_sto_tpcc_wrapper::tls_legacy_thread_initialized_ =
    false;
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
    (void)sto_tpcc_table_destroy(table_);
    table_ = nullptr;
  }
}

bool rust_sto_tpcc_ordered_index::get(lcdf::Str key, std::string &value,
                                      size_t max_bytes_read) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      const bool found =
          owner_->get_raw(table_, key, value, max_bytes_read);
      if (!owner_->commit_txn(owner_->tls_thread_))
        continue;
      return found;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    } catch (...) {
      owner_->abort_current_transaction_noexcept();
      throw;
    }
  }
}

bool rust_sto_tpcc_ordered_index::put(lcdf::Str key,
                                      const std::string &value) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      std::string ignored;
      const bool existed = owner_->get_raw(table_, key, ignored, 0);
      const std::string encoded = mako::Encode(value);
      owner_->put_raw(table_, key, encoded, false);
      owner_->commit_txn(owner_->tls_thread_);
      return !existed;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    } catch (...) {
      owner_->abort_current_transaction_noexcept();
      throw;
    }
  }
}

bool rust_sto_tpcc_ordered_index::insert(lcdf::Str key,
                                         const std::string &value) {
  while (true) {
    owner_->begin_current_transaction();
    try {
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
      owner_->abort_current_transaction_noexcept();
    } catch (...) {
      owner_->abort_current_transaction_noexcept();
      throw;
    }
  }
}

bool rust_sto_tpcc_ordered_index::remove(lcdf::Str key) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      const bool existed = owner_->remove_raw(table_, key);
      owner_->commit_txn(owner_->tls_thread_);
      return existed;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    } catch (...) {
      owner_->abort_current_transaction_noexcept();
      throw;
    }
  }
}

void rust_sto_tpcc_ordered_index::scan(const std::string &start_key,
                                       const std::string *end_key,
                                       oi_scan_callback &callback,
                                       str_arena *) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      owner_->scan_raw(table_, false, start_key, end_key, callback, true);
      owner_->commit_txn(owner_->tls_thread_);
      return;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    } catch (...) {
      owner_->abort_current_transaction_noexcept();
      throw;
    }
  }
}

void rust_sto_tpcc_ordered_index::rscan(const std::string &start_key,
                                        const std::string *end_key,
                                        oi_scan_callback &callback,
                                        str_arena *) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      owner_->scan_raw(table_, true, start_key, end_key, callback, true);
      owner_->commit_txn(owner_->tls_thread_);
      return;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    } catch (...) {
      owner_->abort_current_transaction_noexcept();
      throw;
    }
  }
}

size_t rust_sto_tpcc_ordered_index::size() const {
  uint64_t rows = 0;
  owner_->require_ok("table_size", sto_tpcc_table_size(table_, &rows));
  return static_cast<size_t>(rows);
}

oi_stats_map rust_sto_tpcc_ordered_index::clear() { return {}; }
int32_t rust_sto_tpcc_ordered_index::get_table_id() { return table_id_; }
bool rust_sto_tpcc_ordered_index::get_is_remote() { return is_remote_; }

bool rust_sto_tpcc_ordered_index::tx_get(c_void *txn, lcdf::Str key,
                                         std::string &value,
                                         size_t max_bytes_read) {
  owner_->require_txn_handle(txn);
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

void rust_sto_tpcc_ordered_index::tx_put(c_void *txn, lcdf::Str key,
                                         const std::string &value) {
  owner_->require_txn_handle(txn);
  owner_->put_active_raw(table_, key, value, false);
}

void rust_sto_tpcc_ordered_index::tx_insert(c_void *txn, lcdf::Str key,
                                            const std::string &value) {
  owner_->require_txn_handle(txn);
  owner_->put_active_raw(table_, key, value, true);
}

void rust_sto_tpcc_ordered_index::tx_remove(c_void *txn, lcdf::Str key) {
  owner_->require_txn_handle(txn);
  (void)owner_->remove_active_raw(table_, key);
}

void rust_sto_tpcc_ordered_index::tx_scan(c_void *txn,
                                          const std::string &start_key,
                                          const std::string *end_key,
                                          oi_scan_callback &callback,
                                          str_arena *) {
  owner_->require_txn_handle(txn);
  owner_->scan_active_raw(table_, false, start_key, end_key, callback, true);
}

void rust_sto_tpcc_ordered_index::tx_rscan(c_void *txn,
                                           const std::string &start_key,
                                           const std::string *end_key,
                                           oi_scan_callback &callback,
                                           str_arena *) {
  owner_->require_txn_handle(txn);
  owner_->scan_active_raw(table_, true, start_key, end_key, callback, true);
}

void rust_sto_tpcc_ordered_index::tx_scan_remote_one(
    c_void *txn, const std::string &start_key, const std::string &end_key,
    std::string &value) {
  owner_->require_txn_handle(txn);
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

bool rust_sto_tpcc_ordered_index::shard_get(lcdf::Str key,
                                            std::string &value,
                                            size_t max_bytes_read) {
  const bool found = owner_->get_raw(table_, key, value);
  if (found) {
    append_canonical_mako_metadata(value);
    if (value.size() > max_bytes_read)
      value.resize(max_bytes_read);
  }
  return found;
}

const c_char *rust_sto_tpcc_ordered_index::shard_put(
    lcdf::Str key, const std::string &value) {
  owner_->put_raw(table_, key, value, false);
  return nullptr;
}

bool rust_sto_tpcc_ordered_index::shard_scan(
    const std::string &start_key, const std::string *end_key,
    oi_scan_callback &callback, str_arena *) {
  owner_->scan_raw(table_, false, start_key, end_key, callback, false);
  return true;
}

rust_sto_tpcc_wrapper::rust_sto_tpcc_wrapper()
    : db_(nullptr), next_table_id_(1) {}

rust_sto_tpcc_wrapper::~rust_sto_tpcc_wrapper() noexcept {
  tables_by_name_.clear();
  tables_by_id_.clear();
  tables_.clear();
  if (db_) {
    (void)sto_tpcc_db_destroy(db_);
    db_ = nullptr;
  }
}

void rust_sto_tpcc_wrapper::init() {
  auto &config = BenchmarkConfig::getInstance();
  if (config.getNshards() != 1 || config.getIsReplicated())
    throw std::runtime_error(
        "Rust STO TPC-C comparison supports one non-replicated shard");

  sto_tpcc_db_config ffi_config{};
  ffi_config.max_threads = static_cast<uint32_t>(
      std::max<size_t>(32, config.getNthreads() * 4 + 16));
  ffi_config.max_key_length = 1024;
  ffi_config.max_items_per_txn = 1024;
  ffi_config.max_locks_per_txn = 2048;
  require_ok("db_create", sto_tpcc_db_create(&ffi_config, &db_));
}

void rust_sto_tpcc_wrapper::on_load_complete() {
  for (const auto &table : tables_) {
    if (!tpcc_table_has_static_directory(table->name_))
      continue;
    require_ok("table_seal_directory_structure",
               sto_tpcc_table_seal_directory_structure(table->table_));
  }
}

void rust_sto_tpcc_wrapper::preallocate_open_index() {}
ssize_t rust_sto_tpcc_wrapper::txn_max_batch_size() const { return 100; }
size_t rust_sto_tpcc_wrapper::sizeof_txn_object(uint64_t txn_flags) const {
  ALWAYS_ASSERT(txn_flags == 0);
  return 1;
}

void rust_sto_tpcc_wrapper::thread_init(bool loader, int source) {
  if (tls_thread_)
    throw std::runtime_error("Rust STO thread was initialized twice");
  legacy_thread_support().thread_init(loader, source);
  tls_legacy_thread_initialized_ = true;
  require_ok("thread_create", sto_tpcc_thread_create(db_, &tls_thread_));
}

void rust_sto_tpcc_wrapper::thread_end() {
  if (tls_transaction_active_)
    abort_current_transaction_noexcept();
  if (tls_thread_) {
    const sto_tpcc_status status = sto_tpcc_thread_destroy(tls_thread_);
    tls_thread_ = nullptr;
    require_ok("thread_destroy", status);
  }
  if (tls_legacy_thread_initialized_) {
    legacy_thread_support().thread_end();
    tls_legacy_thread_initialized_ = false;
  }
}

void *rust_sto_tpcc_wrapper::new_txn(uint64_t txn_flags, str_arena &, void *,
                                     TxnProfileHint) {
  ALWAYS_ASSERT(txn_flags == 0);
  begin_current_transaction();
  return tls_thread_;
}

bool rust_sto_tpcc_wrapper::commit_txn(void *txn) {
  require_txn_handle(txn);
  const sto_tpcc_status status =
      mako_sto_tpcc_txn_commit_trusted(tls_thread_);
  tls_transaction_active_ = false;
  if (status == STO_TPCC_RETRY)
    throw abstract_abort_exception();
  require_ok("txn_commit", status);
  return true;
}

bool rust_sto_tpcc_wrapper::commit_txn_no_paxos(void *txn) {
  return commit_txn(txn);
}

void rust_sto_tpcc_wrapper::abort_txn(void *txn) {
  if (txn && txn != tls_thread_)
    throw std::runtime_error("foreign Rust STO transaction handle");
  abort_current_transaction_noexcept();
}

void rust_sto_tpcc_wrapper::abort_txn_local(void *txn) { abort_txn(txn); }

TxnFixedReadCapability *rust_sto_tpcc_wrapper::txn_fixed_read_capability(
    abstract_ordered_index *index) noexcept {
  if (index == nullptr)
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
  if (index == nullptr)
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
  if (index == nullptr)
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
  return this;
}

TxnTpccPaymentCapability *
rust_sto_tpcc_wrapper::txn_tpcc_payment_capability() noexcept {
  return kDisablePaymentPrefix ? nullptr : this;
}

TxnTpccNewOrderCapability *
rust_sto_tpcc_wrapper::txn_tpcc_new_order_capability() noexcept {
  return kDisableNewOrderFull ? nullptr : this;
}

TxnTpccDeliveryCapability *
rust_sto_tpcc_wrapper::txn_tpcc_delivery_capability() noexcept {
  return kDisableDeliveryFull ? nullptr : this;
}

TxnTpccStockLevelCapability *
rust_sto_tpcc_wrapper::txn_tpcc_stock_level_capability() noexcept {
  return kDisableStockLevelFull ? nullptr : this;
}

bool rust_sto_tpcc_wrapper::payment_full_enabled() const noexcept {
  return !kDisablePaymentFull;
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

abstract_ordered_index *rust_sto_tpcc_wrapper::get_index_by_table_id(
    unsigned short table_id) {
  const auto it = tables_by_id_.find(table_id);
  return it == tables_by_id_.end() ? nullptr : it->second;
}

abstract_ordered_index *rust_sto_tpcc_wrapper::open_index(
    const std::string &name, size_t, bool, bool) {
  return open_index(name, -1);
}

abstract_ordered_index *rust_sto_tpcc_wrapper::open_index(
    const std::string &name, int shard_index) {
  auto &config = BenchmarkConfig::getInstance();
  if (shard_index == -1)
    shard_index = static_cast<int>(config.getShardIndex());
  if (shard_index != static_cast<int>(config.getShardIndex()))
    throw std::runtime_error(
        "Rust STO TPC-C comparison does not support remote indexes");

  const auto key = std::make_tuple(name, shard_index);
  if (const auto found = tables_by_name_.find(key);
      found != tables_by_name_.end())
    return found->second;

  const sto_tpcc_table_config table_config =
      rust_sto_tpcc_detail::table_config_for(name);
  const sto_tpcc_resolved_cache_policy cache_policy =
      tpcc_resolved_cache_policy_for(name);
  sto_tpcc_table *table = nullptr;
  require_ok("table_create_with_cache_policy",
             sto_tpcc_table_create_with_cache_policy(
                 db_, &table_config, cache_policy, &table));

  const int32_t table_id = next_table_id_++;
  auto index = std::make_unique<rust_sto_tpcc_ordered_index>(
      this, table, table_id, name, false);
  auto *raw = index.get();
  tables_.push_back(std::move(index));
  tables_by_id_.emplace(table_id, raw);
  tables_by_name_.emplace(key, raw);
  return raw;
}

void rust_sto_tpcc_wrapper::close_index(abstract_ordered_index *) {
  // The benchmark opens its complete schema up front and retains borrowed
  // index pointers until the database is destroyed.
}

void rust_sto_tpcc_wrapper::shard_abort_txn(void *txn) { abort_txn(txn); }

int rust_sto_tpcc_wrapper::shard_validate() {
  throw std::runtime_error("Rust STO comparison has no distributed 2PC");
}
void rust_sto_tpcc_wrapper::shard_install(uint32_t) {
  throw std::runtime_error("Rust STO comparison has no distributed 2PC");
}
void rust_sto_tpcc_wrapper::shard_serialize_util(uint32_t) {
  throw std::runtime_error("Rust STO comparison has no distributed 2PC");
}
void rust_sto_tpcc_wrapper::shard_unlock(bool) {
  throw std::runtime_error("Rust STO comparison has no distributed 2PC");
}
void rust_sto_tpcc_wrapper::shard_reset() { begin_current_transaction(); }

[[noreturn]] void rust_sto_tpcc_wrapper::throw_fatal(
    const char *operation, sto_tpcc_status status) {
  throw std::runtime_error(std::string("Rust STO ") + operation +
                           " failed (status " + std::to_string(status) +
                           "): " + last_rust_error());
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

void rust_sto_tpcc_wrapper::begin_current_transaction() {
  if (!tls_thread_)
    throw std::runtime_error("Rust STO operation on an unattached thread");
  if (tls_transaction_active_)
    throw std::runtime_error("nested Rust STO transaction");
  require_ok("txn_begin", mako_sto_tpcc_txn_begin_trusted(tls_thread_));
  tls_transaction_active_ = true;
}

void rust_sto_tpcc_wrapper::abort_current_transaction_noexcept() {
  if (!tls_thread_ || !tls_transaction_active_)
    return;
  (void)sto_tpcc_txn_abort(tls_thread_);
  tls_transaction_active_ = false;
}

bool rust_sto_tpcc_wrapper::get_raw(const sto_tpcc_table *table,
                                    lcdf::Str key,
                                    std::string &encoded_value,
                                    size_t max_bytes_read) {
  require_txn_handle(tls_thread_);
  return get_active_raw(table, key, encoded_value, max_bytes_read);
}

bool rust_sto_tpcc_wrapper::get_active_raw(
    const sto_tpcc_table *table, lcdf::Str key,
    std::string &encoded_value, size_t max_bytes_read) {
  sto_tpcc_status status = STO_TPCC_OK;
  size_t actual = 0;

  // A zero-byte read still observes the row and distinguishes presence from
  // absence, but needs no temporary value allocation. The Rust ABI reports a
  // live nonempty row as BUFFER_TOO_SMALL with its exact logical length.
  if (max_bytes_read == 0) {
    encoded_value.clear();
    status = mako_sto_tpcc_get_trusted(
        tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
        key.length(), nullptr, 0, &actual);
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
      status = mako_sto_tpcc_get_trusted(
          tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
          key.length(), reinterpret_cast<uint8_t *>(output), capacity,
          &actual);
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

void rust_sto_tpcc_wrapper::scan_active_raw(
    const sto_tpcc_table *table, bool reverse,
    const std::string &start_key, const std::string *end_key,
    oi_scan_callback &callback, bool strip_value_metadata) {
  scan_bridge bridge{&callback, !strip_value_metadata, {}, {}};
  size_t visited = 0;
  const auto *start = reinterpret_cast<const uint8_t *>(start_key.data());
  const auto *end = end_key
                        ? reinterpret_cast<const uint8_t *>(end_key->data())
                        : nullptr;
  const sto_tpcc_status status = mako_sto_tpcc_scan_trusted(
      tls_thread_, table,
      reverse ? STO_TPCC_SCAN_REVERSE : STO_TPCC_SCAN_FORWARD,
      reverse ? (end_key ? STO_TPCC_BOUND_EXCLUDED
                         : STO_TPCC_BOUND_UNBOUNDED)
              : STO_TPCC_BOUND_INCLUDED,
      reverse ? end : start, reverse ? (end_key ? end_key->size() : 0)
                                    : start_key.size(),
      reverse ? STO_TPCC_BOUND_INCLUDED
              : (end_key ? STO_TPCC_BOUND_EXCLUDED
                         : STO_TPCC_BOUND_UNBOUNDED),
      reverse ? start : end,
      reverse ? start_key.size() : (end_key ? end_key->size() : 0),
      callback.max_records_hint(), invoke_scan_bridge, &bridge, &visited);
  if (bridge.failure)
    std::rethrow_exception(bridge.failure);
  require_ok(reverse ? "reverse scan" : "scan", status);
}
