#ifndef MAKO_BENCHMARKS_TPCC_FIXED_BATCH_H
#define MAKO_BENCHMARKS_TPCC_FIXED_BATCH_H

#include <cstddef>
#include <cstdint>

class FullOrderedIndex;
using abstract_ordered_index = FullOrderedIndex;

namespace tpcc_fixed_batch {

// customer::value has a 164-byte maximum encoded payload. Rows through 160
// bytes use Rust's borrowed bounded-value lane; the remaining valid encodings
// fall back to owned transaction staging.
constexpr size_t payment_value_capacity = 164;
constexpr size_t payment_history_value_length = 30;
constexpr size_t new_order_max_lines = 15;
constexpr size_t delivery_district_count = 10;
constexpr size_t delivery_max_lines_per_district = 15;
constexpr size_t stock_level_max_order_line_rows = 20 * 15;

// All key pointers name the complete fixed-width Masstree encoding used by
// TPC-C: warehouse is 4 bytes, district is 8, customer_key_prefix is the
// first 8 bytes of a 12-byte customer key, and each customer-name bound is
// 40 bytes. The name table and bounds are required only when customer_by_name
// is true. Every index is a local table owned by the transaction's database.
// The three output ranges are distinct payment_value_capacity-byte buffers and
// do not overlap the request, result, key storage, transaction, or index
// objects. Their addresses and returned prefixes must remain unchanged until
// the transaction commits or aborts because an implementation may stage the
// values by borrowing them.
struct payment_prefix_request {
  void *txn;
  abstract_ordered_index *warehouse;
  abstract_ordered_index *district;
  abstract_ordered_index *customer;
  abstract_ordered_index *customer_name;
  const char *warehouse_key;
  const char *district_key;
  const char *customer_key_prefix;
  const char *customer_name_lower;
  const char *customer_name_upper;
  int32_t customer_id;
  float payment_amount;
  bool customer_by_name;
  char *warehouse_value;
  char *district_value;
  char *customer_value;
};

struct payment_prefix_result {
  size_t warehouse_value_length;
  size_t district_value_length;
  size_t customer_value_length;
  int32_t customer_id;
};

// The full local Payment lane completes and commits the active transaction
// before returning. Unlike payment_prefix_request, no caller-owned replacement
// buffer is retained. The implementation must preserve the benchmark's exact
// encoded history key/value layouts and its duplicate-history no-op behavior.
struct payment_full_request {
  void *txn;
  abstract_ordered_index *warehouse;
  abstract_ordered_index *district;
  abstract_ordered_index *customer;
  abstract_ordered_index *customer_name;
  abstract_ordered_index *history;
  const char *warehouse_key;
  const char *district_key;
  const char *customer_key_prefix;
  const char *customer_name_lower;
  const char *customer_name_upper;
  int32_t customer_id;
  float payment_amount;
  uint32_t timestamp;
  int32_t warehouse_id;
  int32_t district_id;
  int32_t customer_warehouse_id;
  int32_t customer_district_id;
  bool customer_by_name;
};

struct payment_full_result {
  size_t history_value_length;
  int32_t customer_id;
};

// The full local NewOrder lane consumes the active transaction and commits it
// before returning. It is valid only when every supplier is the home
// warehouse and the benchmark uses the nontransactional fast order-ID
// generator. The two input arrays contain line_count entries and remain
// readable for the synchronous call.
struct new_order_full_request {
  void *txn;
  abstract_ordered_index *warehouse;
  abstract_ordered_index *district;
  abstract_ordered_index *customer;
  abstract_ordered_index *item;
  abstract_ordered_index *stock;
  abstract_ordered_index *new_order;
  abstract_ordered_index *oorder;
  abstract_ordered_index *oorder_c_id_idx;
  abstract_ordered_index *order_line;
  const uint32_t *item_ids;
  const uint32_t *quantities;
  int32_t warehouse_id;
  int32_t district_id;
  int32_t customer_id;
  int32_t order_id;
  uint32_t entry_date;
  uint32_t line_count;
};

struct new_order_full_result {
  // This is the benchmark's historical return accounting: new_order and
  // oorder plus every order-line encoded value. It is intentionally
  // independent of ignored duplicate header inserts.
  size_t reported_value_bytes;
};

// The full local Delivery lane consumes and resolves the active transaction.
// last_no_o_ids names exactly delivery_district_count mutable cursors. Each
// cursor is advanced immediately after its district selects a new-order row;
// those worker-local writes intentionally survive a later transaction abort.
struct delivery_full_request {
  void *txn;
  abstract_ordered_index *new_order;
  abstract_ordered_index *oorder;
  abstract_ordered_index *order_line;
  abstract_ordered_index *customer;
  int32_t *last_no_o_ids;
  int32_t warehouse_id;
  int32_t carrier_id;
  uint32_t timestamp;
};

struct delivery_full_result {
  // Delivery's historical benchmark byte accounting is always zero. The row
  // counters make the fused boundary directly testable without changing it.
  size_t reported_value_bytes;
  uint32_t delivered_districts;
  uint32_t updated_order_lines;
};

// The scalar StockLevel prefix has already read the district row and selected
// the current next-order ID. This tail scans the preceding twenty orders,
// probes distinct stock rows, and resolves the active read-only transaction.
struct stock_level_full_request {
  void *txn;
  abstract_ordered_index *order_line;
  abstract_ordered_index *stock;
  uint64_t current_next_order_id;
  int32_t warehouse_id;
  int32_t district_id;
  uint32_t threshold;
};

struct stock_level_full_result {
  size_t reported_value_bytes;
  uint32_t scanned_order_line_rows;
  uint32_t distinct_item_ids;
  uint32_t low_stock_count;
};

inline bool new_order_mode_is_eligible(bool fixed_key_layout,
                                       int control_mode, bool all_local,
                                       bool is_remote) {
  return fixed_key_layout && control_mode == 0 && all_local && !is_remote;
}

inline bool delivery_mode_is_eligible(bool fixed_key_layout,
                                      int control_mode, bool local_tables) {
  return fixed_key_layout && control_mode == 0 && local_tables;
}

inline bool stock_level_mode_is_eligible(bool fixed_key_layout,
                                         int control_mode,
                                         bool local_tables) {
  return fixed_key_layout && control_mode == 0 && local_tables;
}

// A single-row mutation is a workload optimization, not a semantic fallback:
// only the local Rust STO mode may replace the literal tx_get + tx_put pair.
// Other control modes retain their failure-injection and remote bookkeeping.
inline bool single_row_modify_mode_is_eligible(bool fixed_key_layout,
                                               int control_mode,
                                               bool local_row) {
  return fixed_key_layout && control_mode == 0 && local_row;
}

// The fused prefix preserves the literal Payment operation order only for a
// fully local transaction using the fixed Masstree key layout. Failure and
// remote-control modes retain the legacy scalar path and its instrumentation.
inline bool payment_prefix_mode_is_eligible(bool fixed_key_layout,
                                            int control_mode, bool all_local,
                                            bool is_remote) {
  return fixed_key_layout && control_mode == 0 && all_local && !is_remote;
}

inline bool suppliers_are_exact_home(const uint32_t *supplier_warehouse_ids,
                                     size_t count,
                                     uint32_t home_global_warehouse) {
  for (size_t index = 0; index < count; ++index) {
    if (supplier_warehouse_ids[index] != home_global_warehouse)
      return false;
  }
  return true;
}

template <typename StockValue>
inline StockValue apply_new_order_stock(const StockValue &current,
                                        uint32_t order_quantity,
                                        bool remote_supplier) {
  StockValue replacement(current);
  if (replacement.s_quantity - order_quantity >= 10)
    replacement.s_quantity -= order_quantity;
  else
    replacement.s_quantity += -int32_t(order_quantity) + 91;
  replacement.s_ytd += order_quantity;
  replacement.s_remote_cnt += remote_supplier ? 1 : 0;
  return replacement;
}

} // namespace tpcc_fixed_batch

class TxnTpccPaymentCapability {
public:
  virtual ~TxnTpccPaymentCapability() noexcept(false) {}
  virtual bool payment_full_enabled() const noexcept = 0;
  virtual tpcc_fixed_batch::payment_full_result tx_payment_full(
      const tpcc_fixed_batch::payment_full_request &request) = 0;
  virtual tpcc_fixed_batch::payment_prefix_result tx_payment_prefix(
      const tpcc_fixed_batch::payment_prefix_request &request) = 0;

protected:
  TxnTpccPaymentCapability() = default;
};

class TxnTpccNewOrderCapability {
public:
  virtual ~TxnTpccNewOrderCapability() noexcept(false) {}
  virtual tpcc_fixed_batch::new_order_full_result tx_new_order_full(
      const tpcc_fixed_batch::new_order_full_request &request) = 0;

protected:
  TxnTpccNewOrderCapability() = default;
};

class TxnTpccDeliveryCapability {
public:
  virtual ~TxnTpccDeliveryCapability() noexcept(false) {}
  virtual tpcc_fixed_batch::delivery_full_result tx_delivery_full(
      const tpcc_fixed_batch::delivery_full_request &request) = 0;

protected:
  TxnTpccDeliveryCapability() = default;
};

class TxnTpccStockLevelCapability {
public:
  virtual ~TxnTpccStockLevelCapability() noexcept(false) {}
  virtual tpcc_fixed_batch::stock_level_full_result tx_stock_level_full(
      const tpcc_fixed_batch::stock_level_full_request &request) = 0;

protected:
  TxnTpccStockLevelCapability() = default;
};

#endif
