#include <cstdlib>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "benchmarks/benchmark_config.h"
#include "benchmarks/tpcc.h"
#include "benchmarks/tpcc_fixed_batch.h"
#include "in_memory_ordered_index.h"
#include "lib/common.h"
#include "storage/rust_sto_tpcc_wrapper.hh"

namespace {

void require(bool condition) {
  if (!condition)
    std::abort();
}

template <size_t N>
bool suffix_equals(const std::array<char, N> &bytes, size_t begin,
                   char expected) {
  if (begin > bytes.size())
    return false;
  for (size_t index = begin; index < bytes.size(); ++index) {
    if (bytes[index] != expected)
      return false;
  }
  return true;
}

class checking_fixed_read final : public oi_fixed_read_callback {
public:
  checking_fixed_read(bool fail, std::string expected)
      : fail_(fail), expected_(std::move(expected)), visits_(0) {}

  void invoke(size_t index, const char *current_value,
              size_t current_value_length) override {
    require(index == visits_++);
    if (index == 1) {
      require(current_value == nullptr);
      require(current_value_length == 0);
      if (fail_)
        throw std::runtime_error("expected fixed callback failure");
      return;
    }
    require(current_value != nullptr);
    require(std::string(current_value, current_value_length) == expected_);
  }

  size_t visits() const { return visits_; }

private:
  bool fail_;
  std::string expected_;
  size_t visits_;
};

class checking_fixed_modify final : public oi_fixed_modify_callback {
public:
  checking_fixed_modify() : visits_(0) {}

  oi_fixed_mutation_result invoke(
      size_t index, const char *current_value,
      size_t current_value_length) override {
    require(index == visits_++);
    switch (index) {
    case 0:
      require(current_value != nullptr);
      require(std::string(current_value, current_value_length) ==
              "fixed-value");
      scratch_ = mako::Encode("first");
      return oi_fixed_mutation_result::put(scratch_);
    case 1:
      // The duplicate observes the first staged logical payload after the
      // wrapper validated and elided its canonical metadata suffix.
      require(current_value != nullptr);
      require(std::string(current_value, current_value_length) == "first");
      scratch_ = mako::Encode("second");
      return oi_fixed_mutation_result::put(scratch_);
    case 2:
      require(current_value == nullptr);
      require(current_value_length == 0);
      scratch_ = mako::Encode("temporary");
      return oi_fixed_mutation_result::put(scratch_);
    case 3:
      require(current_value != nullptr);
      require(std::string(current_value, current_value_length) ==
              "temporary");
      return oi_fixed_mutation_result::remove();
    default:
      std::abort();
    }
  }

  size_t visits() const { return visits_; }

private:
  std::string scratch_;
  size_t visits_;
};

class throwing_fixed_modify final : public oi_fixed_modify_callback {
public:
  oi_fixed_mutation_result invoke(size_t, const char *, size_t) override {
    throw std::runtime_error("expected fixed mutation failure");
  }
};

class replacement_fixed_modify final : public oi_fixed_modify_callback {
public:
  explicit replacement_fixed_modify(std::string replacement)
      : replacement_(std::move(replacement)), visits_(0) {}

  oi_fixed_mutation_result invoke(size_t, const char *current_value,
                                  size_t current_value_length) override {
    require(current_value != nullptr);
    require(current_value_length != 0);
    ++visits_;
    return oi_fixed_mutation_result::put(replacement_);
  }
  size_t visits() const { return visits_; }

private:
  std::string replacement_;
  size_t visits_;
};

class checking_raw_scan final : public oi_scan_callback {
public:
  checking_raw_scan(std::string expected_key, std::string expected_value)
      : expected_key_(std::move(expected_key)),
        expected_value_(std::move(expected_value)), visits_(0) {}

  bool invoke(const char *key, size_t key_length,
              const std::string &value) override {
    require(visits_++ == 0);
    require(std::string(key, key_length) == expected_key_);
    require(value == expected_value_);
    return false;
  }

  size_t max_records_hint() const override { return 1; }
  size_t visits() const { return visits_; }

private:
  std::string expected_key_;
  std::string expected_value_;
  size_t visits_;
};

class fallback_fixed_index final : public janus::InMemoryOrderedIndex {
public:
  fallback_fixed_index() : calls_(0) {}

  bool tx_get(c_void *, lcdf::Str key, std::string &value,
              size_t max_bytes_read) override {
    (void)key;
    (void)value;
    (void)max_bytes_read;
    ++calls_;
    return false;
  }

  size_t calls() const { return calls_; }

private:
  size_t calls_;
};

class fallback_fixed_db final : public abstract_db {
public:
  void *new_txn(uint64_t, str_arena &, void *, TxnProfileHint) override {
    return nullptr;
  }
  bool commit_txn(void *) override { return false; }
  bool commit_txn_no_paxos(void *) override { return false; }
  void abort_txn(void *) override {}
  void abort_txn_local(void *) override {}
  abstract_ordered_index *get_index_by_table_id(unsigned short) override {
    return nullptr;
  }
  abstract_ordered_index *open_index(const std::string &, size_t, bool,
                                     bool) override {
    return nullptr;
  }
  abstract_ordered_index *open_index(const std::string &, int) override {
    return nullptr;
  }
  void close_index(abstract_ordered_index *) override {}
  void preallocate_open_index() override {}
  void init() override {}
  void shard_abort_txn(void *) override {}
  int shard_validate() override { return 0; }
  void shard_install(uint32_t) override {}
  void shard_serialize_util(uint32_t) override {}
  void shard_unlock(bool) override {}
  void shard_reset() override {}
};

} // namespace

// Focused C++/Rust fixed-read smoke test. The CMake target uses the matched
// sto_tpcc_bench object/link flags and CTest runs it with a one-shard
// configuration; keeping it next to native_ffi.rs makes the cross-language
// lifecycle assertion easy to audit.
int main(int argc, char **argv) {
  require(argc == 2);

  transport::Configuration config(argv[1]);
  auto &benchmark = BenchmarkConfig::getInstance();
  benchmark.setConfig(&config);
  benchmark.setNthreads(1);
  benchmark.setNshards(1);
  benchmark.setShardIndex(0);
  benchmark.setIsReplicated(false);
  benchmark.setPinCpus(false);

  // A one-warehouse load leaves 3.97M rows in the former 4M history tier.
  // Pure Payment can cross that deterministic boundary inside the paired
  // runner's default interval, so history must retain the append-heavy tier
  // and enough key bytes for every admitted 24-byte key.
  const sto_tpcc_table_config history_config =
      rust_sto_tpcc_detail::table_config_for("history_1");
  require(history_config.max_retained_records == 16'000'000);
  require(history_config.max_consumed_record_ids == 20'000'000);
  require(history_config.max_retained_key_bytes >=
          history_config.max_retained_records * sizeof(history::key));

  // Keep the fix table-specific: the lower-volume one-row growth tables do
  // not need history's larger segment directory.
  const sto_tpcc_table_config order_config =
      rust_sto_tpcc_detail::table_config_for("oorder_1");
  require(order_config.max_retained_records == 4'000'000);
  require(order_config.max_consumed_record_ids == 6'000'000);
  require(rust_sto_tpcc_detail::table_config_for("warehouse_1")
              .bounded_atomic_values != 0);
  require(rust_sto_tpcc_detail::table_config_for("customer_1")
              .bounded_atomic_values != 0);
  require(rust_sto_tpcc_detail::table_config_for("district_1")
              .bounded_atomic_values != 0);
  require(rust_sto_tpcc_detail::table_config_for("customer_name_idx_1")
              .bounded_atomic_values == 0);

  rust_sto_tpcc_wrapper db;
  db.init();
  abstract_ordered_index *table = db.open_index("stock_1", -1);
  require(table != nullptr);
  abstract_ordered_index *second_table = db.open_index("order_line_1", -1);
  require(second_table != nullptr);
  abstract_ordered_index *raw_table = db.open_index("new_order_1", -1);
  require(raw_table != nullptr);
  abstract_ordered_index *payment_warehouse =
      db.open_index("warehouse_1", -1);
  abstract_ordered_index *payment_district = db.open_index("district_1", -1);
  abstract_ordered_index *payment_customer = db.open_index("customer_1", -1);
  abstract_ordered_index *payment_customer_name =
      db.open_index("customer_name_idx_1", -1);
  require(payment_warehouse != nullptr && payment_district != nullptr &&
          payment_customer != nullptr && payment_customer_name != nullptr);
  // Loader mode provides the same legacy TThread state needed by the TPC-C
  // bridge without creating an irrelevant ShardClient for this local test.
  db.thread_init(true, 0);

  str_arena arena;
  std::string value;
  void *txn;

  const char fixed_key[8] = {'f', 'i', 'x', 'e', 'd', '0', '0', '1'};
  const char missing_key[8] = {'m', 'i', 's', 's', 'i', 'n', 'g', '1'};
  char packed_keys[3 * sizeof(fixed_key)];
  std::memcpy(packed_keys, fixed_key, sizeof(fixed_key));
  std::memcpy(packed_keys + sizeof(fixed_key), missing_key,
              sizeof(missing_key));
  std::memcpy(packed_keys + 2 * sizeof(fixed_key), fixed_key,
              sizeof(fixed_key));
  char packed_modify_keys[4 * sizeof(fixed_key)];
  std::memcpy(packed_modify_keys, fixed_key, sizeof(fixed_key));
  std::memcpy(packed_modify_keys + sizeof(fixed_key), fixed_key,
              sizeof(fixed_key));
  std::memcpy(packed_modify_keys + 2 * sizeof(fixed_key), missing_key,
              sizeof(missing_key));
  std::memcpy(packed_modify_keys + 3 * sizeof(fixed_key), missing_key,
              sizeof(missing_key));

  // An unsupported backend invokes neither the capability callable nor a
  // hidden scalar lookup; its workload retains the literal original loop.
  fallback_fixed_index fallback;
  fallback_fixed_db fallback_db;
  require(fallback_db.txn_tpcc_new_order_capability() == nullptr);
  require(fallback_db.txn_tpcc_delivery_capability() == nullptr);
  require(fallback_db.txn_tpcc_stock_level_capability() == nullptr);
  require(db.txn_tpcc_new_order_capability() != nullptr);
  require(db.txn_tpcc_delivery_capability() != nullptr);
  require(db.txn_tpcc_stock_level_capability() != nullptr);
  require(tpcc_fixed_batch::new_order_max_lines == 15);
  require(tpcc_fixed_batch::delivery_district_count == 10);
  require(tpcc_fixed_batch::delivery_max_lines_per_district == 15);
  require(tpcc_fixed_batch::stock_level_max_order_line_rows == 300);
  bool unsupported_invoked = false;
  require(!tx_visit_fixed_if_supported(
      fallback_db.txn_fixed_read_capability(&fallback),
      [&](TxnFixedReadCapability &) {
        unsupported_invoked = true;
      }));
  require(!unsupported_invoked);
  require(fallback.calls() == 0);

  bool disabled_invoked = false;
  require(!tx_visit_fixed_if_supported(
      nullptr, [&](TxnFixedReadCapability &) { disabled_invoked = true; }));
  require(!disabled_invoked);

  const uint32_t home_suppliers[4] = {7, 7, 7, 7};
  uint32_t injected_suppliers[4] = {7, 7, 7, 7};
  require(tpcc_fixed_batch::new_order_mode_is_eligible(true, 0, true,
                                                        false));
  require(!tpcc_fixed_batch::new_order_mode_is_eligible(false, 0, true,
                                                         false));
  require(!tpcc_fixed_batch::new_order_mode_is_eligible(true, 1, true,
                                                         false));
  require(!tpcc_fixed_batch::new_order_mode_is_eligible(true, 0, false,
                                                         false));
  require(!tpcc_fixed_batch::new_order_mode_is_eligible(true, 0, true, true));
  require(tpcc_fixed_batch::delivery_mode_is_eligible(true, 0, true));
  require(!tpcc_fixed_batch::delivery_mode_is_eligible(false, 0, true));
  require(!tpcc_fixed_batch::delivery_mode_is_eligible(true, 1, true));
  require(!tpcc_fixed_batch::delivery_mode_is_eligible(true, 0, false));
  require(tpcc_fixed_batch::stock_level_mode_is_eligible(true, 0, true));
  require(!tpcc_fixed_batch::stock_level_mode_is_eligible(false, 0, true));
  require(!tpcc_fixed_batch::stock_level_mode_is_eligible(true, 1, true));
  require(!tpcc_fixed_batch::stock_level_mode_is_eligible(true, 0, false));
  require(tpcc_fixed_batch::single_row_modify_mode_is_eligible(true, 0,
                                                               true));
  require(!tpcc_fixed_batch::single_row_modify_mode_is_eligible(false, 0,
                                                                true));
  require(!tpcc_fixed_batch::single_row_modify_mode_is_eligible(true, 1,
                                                                true));
  require(!tpcc_fixed_batch::single_row_modify_mode_is_eligible(true, 0,
                                                                false));
  require(tpcc_fixed_batch::payment_prefix_mode_is_eligible(true, 0, true,
                                                             false));
  require(!tpcc_fixed_batch::payment_prefix_mode_is_eligible(false, 0, true,
                                                              false));
  require(!tpcc_fixed_batch::payment_prefix_mode_is_eligible(true, 1, true,
                                                              false));
  require(!tpcc_fixed_batch::payment_prefix_mode_is_eligible(true, 0, false,
                                                              false));
  require(!tpcc_fixed_batch::payment_prefix_mode_is_eligible(true, 0, true,
                                                              true));
  require(tpcc_fixed_batch::suppliers_are_exact_home(home_suppliers, 4, 7));
  // Mirrors the control-mode failure injection that mutates supplier[0]
  // after allLocal was computed: the final-array proof still rejects it.
  injected_suppliers[0] = 6;
  require(!tpcc_fixed_batch::suppliers_are_exact_home(injected_suppliers, 4,
                                                       7));
  const bool injected_control_mode_batch_allowed =
      tpcc_fixed_batch::new_order_mode_is_eligible(true, 2, true, true) &&
      tpcc_fixed_batch::suppliers_are_exact_home(injected_suppliers, 4, 7);
  require(!injected_control_mode_batch_allowed);

  const auto scalar_stock_update = [](stock::value current, uint quantity,
                                      bool remote) {
    if (current.s_quantity - quantity >= 10)
      current.s_quantity -= quantity;
    else
      current.s_quantity += -int32_t(quantity) + 91;
    current.s_ytd += quantity;
    current.s_remote_cnt += remote ? 1 : 0;
    return current;
  };
  const auto same_stock = [](const stock::value &left,
                             const stock::value &right) {
    return left.s_quantity == right.s_quantity &&
           left.s_ytd == right.s_ytd &&
           left.s_order_cnt == right.s_order_cnt &&
           left.s_remote_cnt == right.s_remote_cnt;
  };
  stock::value initial_stock;
  initial_stock.s_quantity = 15;
  initial_stock.s_ytd = 3;
  initial_stock.s_order_cnt = 4;
  initial_stock.s_remote_cnt = 5;
  const stock::value distinct_batch =
      tpcc_fixed_batch::apply_new_order_stock(initial_stock, 6, false);
  const stock::value distinct_scalar =
      scalar_stock_update(initial_stock, 6, false);
  require(same_stock(distinct_batch, distinct_scalar));
  const stock::value duplicate_batch = tpcc_fixed_batch::apply_new_order_stock(
      tpcc_fixed_batch::apply_new_order_stock(initial_stock, 5, false), 7,
      false);
  const stock::value duplicate_scalar = scalar_stock_update(
      scalar_stock_update(initial_stock, 5, false), 7, false);
  require(same_stock(duplicate_batch, duplicate_scalar));

  bool unsupported_modify_invoked = false;
  require(!tx_modify_fixed_if_supported(
      fallback_db.txn_fixed_modify_capability(&fallback),
      [&](TxnFixedModifyCapability &) {
        unsupported_modify_invoked = true;
      }));
  require(!unsupported_modify_invoked);
  require(fallback.calls() == 0);

  bool unsupported_put_invoked = false;
  require(!tx_put_fixed_if_supported(
      fallback_db.txn_fixed_put_capability(&fallback),
      [&](TxnFixedPutCapability &) {
        unsupported_put_invoked = true;
      }));
  require(!unsupported_put_invoked);
  require(fallback.calls() == 0);

  bool unsupported_insert_batch_invoked = false;
  require(!tx_insert_many_if_supported(
      fallback_db.txn_insert_batch_capability(),
      [&](TxnInsertBatchCapability &) {
        unsupported_insert_batch_invoked = true;
      }));
  require(!unsupported_insert_batch_invoked);
  require(fallback.calls() == 0);

  txn = db.new_txn(0, arena, nullptr);
  tx_put(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)),
         mako::Encode("fixed-value"));
  require(db.commit_txn(txn));

  // The local transactional surface round-trips only the logical payload.
  // The raw shard surface retains its historical encoded contract by
  // rebuilding the exact all-zero suffix that was validated and elided.
  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "fixed-value");
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value,
                 5));
  require(value == "fixed");
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value,
                 0));
  require(value.empty());
  require(db.commit_txn(txn));

  value.assign("stale");
  require(table->get(lcdf::Str(fixed_key, sizeof(fixed_key)), value, 4));
  require(value == "fixe");

  const std::string raw_key("canonical-raw-key");
  const std::string raw_end_key("canonical-raw-key~");
  const std::string raw_encoded = mako::Encode("canonical-raw-value");
  txn = db.new_txn(0, arena, nullptr);
  require(raw_table->shard_put(raw_key, raw_encoded) == nullptr);
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  std::string raw_value;
  require(raw_table->shard_get(raw_key, raw_value, std::string::npos));
  require(raw_value == raw_encoded);
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  const size_t raw_prefix_length = std::string("canonical-raw-value").size() + 3;
  require(raw_table->shard_get(raw_key, raw_value, raw_prefix_length));
  require(raw_value == raw_encoded.substr(0, raw_prefix_length));
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  checking_raw_scan raw_scan(raw_key, raw_encoded);
  require(raw_table->shard_scan(raw_key, &raw_end_key, raw_scan, &arena));
  require(raw_scan.visits() == 1);
  require(db.commit_txn(txn));

  // Unsupported widths fail before invoking user code. This is a programming
  // error, so the caller explicitly closes the still-active transaction.
  txn = db.new_txn(0, arena, nullptr);
  checking_fixed_read bad_width_callback(false, "fixed");
  bool bad_width_caught = false;
  try {
    db.txn_fixed_read_capability(table)->tx_visit_fixed(
        txn, packed_keys, 7, 1, 5, bad_width_callback);
  } catch (const std::runtime_error &error) {
    bad_width_caught =
        std::string(error.what()).find("unsupported fixed key width 7") !=
        std::string::npos;
  }
  require(bad_width_caught);
  require(bad_width_callback.visits() == 0);
  db.abort_txn(txn);

  txn = db.new_txn(0, arena, nullptr);
  checking_fixed_read fixed_success(false, "fixed");
  require(tx_visit_fixed_if_supported(
      db.txn_fixed_read_capability(table),
      [&](TxnFixedReadCapability &capability) {
        capability.tx_visit_fixed(txn, packed_keys, sizeof(fixed_key), 3, 5,
                                  fixed_success);
      }));
  require(fixed_success.visits() == 3);
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  checking_fixed_read fixed_failure(true, "fixed");
  bool caught = false;
  try {
    require(tx_visit_fixed_if_supported(
        db.txn_fixed_read_capability(table),
        [&](TxnFixedReadCapability &capability) {
          capability.tx_visit_fixed(txn, packed_keys, sizeof(fixed_key), 3, 5,
                                    fixed_failure);
        }));
  } catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "expected fixed callback failure";
  }
  require(caught);
  require(fixed_failure.visits() == 2);

  // The failed fixed callback also closed the attempt before rethrowing.
  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "fixed-value");
  require(db.commit_txn(txn));

  // PUT replacements enter as complete canonical encoded values. Duplicate
  // positions observe the earlier logical mutation without a metadata copy.
  txn = db.new_txn(0, arena, nullptr);
  checking_fixed_modify fixed_modify;
  require(tx_modify_fixed_if_supported(
      db.txn_fixed_modify_capability(table),
      [&](TxnFixedModifyCapability &capability) {
        capability.tx_modify_fixed(txn, packed_modify_keys,
                                   sizeof(fixed_key), 4, fixed_modify);
      }));
  require(fixed_modify.visits() == 4);
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "second");
  require(!tx_get(table, txn,
                  lcdf::Str(missing_key, sizeof(missing_key)), value));
  require(db.commit_txn(txn));

  // Fixed INSERT takes encoded caller-owned strings without any C++ callback.
  // Duplicate positions observe earlier staged inserts and existing live rows
  // remain unchanged while the other positions may still commit.
  const char put_key_a[8] = {'p', 'u', 't', '0', '0', '0', '0', '1'};
  const char put_key_b[8] = {'p', 'u', 't', '0', '0', '0', '0', '2'};
  char packed_put_keys[4 * sizeof(put_key_a)];
  std::memcpy(packed_put_keys, put_key_a, sizeof(put_key_a));
  std::memcpy(packed_put_keys + sizeof(put_key_a), put_key_a,
              sizeof(put_key_a));
  std::memcpy(packed_put_keys + 2 * sizeof(put_key_a), fixed_key,
              sizeof(fixed_key));
  std::memcpy(packed_put_keys + 3 * sizeof(put_key_a), put_key_b,
              sizeof(put_key_b));
  const std::string put_values[4] = {
      mako::Encode("put-first"), mako::Encode("put-ignored"),
      mako::Encode("existing-ignored"), mako::Encode("put-last")};
  const std::string_view put_value_views[4] = {
      put_values[0], put_values[1], put_values[2], put_values[3]};

  txn = db.new_txn(0, arena, nullptr);
  oi_fixed_put_result put_result{0, 0};
  require(tx_put_fixed_if_supported(
      db.txn_fixed_put_capability(table),
      [&](TxnFixedPutCapability &capability) {
        put_result = capability.tx_put_fixed(
            txn, packed_put_keys, sizeof(put_key_a), 4, put_value_views,
            oi_fixed_put_mode::insert);
      }));
  require(put_result.inserted == 2);
  require(put_result.first_duplicate == 1);
  require(put_result.has_duplicate());
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(put_key_a, sizeof(put_key_a)), value));
  require(value == "put-first");
  require(tx_get(table, txn, lcdf::Str(put_key_b, sizeof(put_key_b)), value));
  require(value == "put-last");
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "second");
  require(db.commit_txn(txn));

  const char packed_upsert_keys[2 * sizeof(fixed_key)] = {
      'f', 'i', 'x', 'e', 'd', '0', '0', '1',
      'p', 'u', 't', '0', '0', '0', '0', '1'};
  const std::string upsert_values[2] = {mako::Encode("upsert-fixed"),
                                         mako::Encode("upsert-put")};
  const std::string_view upsert_value_views[2] = {
      upsert_values[0], upsert_values[1]};
  txn = db.new_txn(0, arena, nullptr);
  require(tx_put_fixed_if_supported(
      db.txn_fixed_put_capability(table),
      [&](TxnFixedPutCapability &capability) {
        put_result = capability.tx_put_fixed(
            txn, packed_upsert_keys, sizeof(fixed_key), 2,
            upsert_value_views,
            oi_fixed_put_mode::upsert);
      }));
  require(put_result.inserted == 0);
  require(put_result.first_duplicate ==
          std::numeric_limits<size_t>::max());
  require(!put_result.has_duplicate());
  require(db.commit_txn(txn));

  // A heterogeneous INSERT batch crosses one ABI boundary while retaining
  // scalar order: a repeated key sees the earlier staged value, an existing
  // row stays unchanged, and a later operation on another table still runs.
  const std::string insert_many_key_a("batch-a");
  const std::string insert_many_key_b("batch-b");
  const std::string insert_many_values[4] = {
      mako::Encode("batch-first"), mako::Encode("batch-ignored"),
      mako::Encode("existing-ignored"), mako::Encode("batch-last")};
  const oi_insert_operation insert_many_operations[4] = {
      {table, insert_many_key_a, insert_many_values[0]},
      {table, insert_many_key_a, insert_many_values[1]},
      {table, std::string_view(fixed_key, sizeof(fixed_key)),
       insert_many_values[2]},
      {second_table, insert_many_key_b, insert_many_values[3]}};

  txn = db.new_txn(0, arena, nullptr);
  oi_fixed_put_result insert_many_result{0, 0};
  require(tx_insert_many_if_supported(
      db.txn_insert_batch_capability(),
      [&](TxnInsertBatchCapability &capability) {
        insert_many_result = capability.tx_insert_many(
            txn, insert_many_operations, 4);
      }));
  require(insert_many_result.inserted == 2);
  require(insert_many_result.first_duplicate == 1);
  require(insert_many_result.has_duplicate());
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, insert_many_key_a, value));
  require(value == "batch-first");
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "upsert-fixed");
  require(tx_get(second_table, txn, insert_many_key_b, value));
  require(value == "batch-last");
  require(db.commit_txn(txn));

  // Every encoded fixed-put value is validated before the single Rust call,
  // so a bad later descriptor cannot leave the earlier descriptor staged.
  const char rejected_fixed_key_a[8] = {'r', 'e', 'j', 'f', 'i', 'x', '0', '1'};
  const char rejected_fixed_key_b[8] = {'r', 'e', 'j', 'f', 'i', 'x', '0', '2'};
  char rejected_fixed_keys[2 * sizeof(rejected_fixed_key_a)];
  std::memcpy(rejected_fixed_keys, rejected_fixed_key_a,
              sizeof(rejected_fixed_key_a));
  std::memcpy(rejected_fixed_keys + sizeof(rejected_fixed_key_a),
              rejected_fixed_key_b, sizeof(rejected_fixed_key_b));
  auto expect_fixed_put_rejection = [&](const std::string &rejected,
                                        const char *expected_message) {
    const std::string canonical = mako::Encode("never-staged-fixed");
    const std::string_view rejected_views[2] = {canonical, rejected};
    txn = db.new_txn(0, arena, nullptr);
    bool rejected_as_expected = false;
    try {
      require(tx_put_fixed_if_supported(
          db.txn_fixed_put_capability(table),
          [&](TxnFixedPutCapability &capability) {
            (void)capability.tx_put_fixed(
                txn, rejected_fixed_keys, sizeof(rejected_fixed_key_a), 2,
                rejected_views, oi_fixed_put_mode::upsert);
          }));
    } catch (const std::invalid_argument &error) {
      rejected_as_expected = std::string(error.what()) == expected_message;
    }
    require(rejected_as_expected);
    db.abort_txn(txn);
  };
  expect_fixed_put_rejection(
      "x", "Rust STO received a truncated Mako metadata suffix");
  std::string nonzero_fixed = mako::Encode("rejected-fixed");
  nonzero_fixed[nonzero_fixed.size() - mako::EXTRA_BITS_FOR_VALUE + 9] = 1;
  expect_fixed_put_rejection(
      nonzero_fixed, "Rust STO does not support nonzero Mako metadata");

  txn = db.new_txn(0, arena, nullptr);
  require(!tx_get(table, txn,
                  lcdf::Str(rejected_fixed_key_a,
                            sizeof(rejected_fixed_key_a)),
                  value));
  require(!tx_get(table, txn,
                  lcdf::Str(rejected_fixed_key_b,
                            sizeof(rejected_fixed_key_b)),
                  value));
  require(db.commit_txn(txn));

  // Heterogeneous batches receive the same all-descriptors-before-FFI
  // validation, including batches spanning more than one table.
  const std::string rejected_batch_key_a("rejected-batch-a");
  const std::string rejected_batch_key_b("rejected-batch-b");
  auto expect_insert_many_rejection = [&](const std::string &rejected,
                                          const char *expected_message) {
    const std::string canonical = mako::Encode("never-staged-batch");
    const oi_insert_operation rejected_operations[2] = {
        {table, rejected_batch_key_a, canonical},
        {second_table, rejected_batch_key_b, rejected}};
    txn = db.new_txn(0, arena, nullptr);
    bool rejected_as_expected = false;
    try {
      require(tx_insert_many_if_supported(
          db.txn_insert_batch_capability(),
          [&](TxnInsertBatchCapability &capability) {
            (void)capability.tx_insert_many(txn, rejected_operations, 2);
          }));
    } catch (const std::invalid_argument &error) {
      rejected_as_expected = std::string(error.what()) == expected_message;
    }
    require(rejected_as_expected);
    db.abort_txn(txn);
  };
  expect_insert_many_rejection(
      "x", "Rust STO received a truncated Mako metadata suffix");
  std::string nonzero_batch = mako::Encode("rejected-batch");
  nonzero_batch[nonzero_batch.size() - mako::EXTRA_BITS_FOR_VALUE + 10] = 1;
  expect_insert_many_rejection(
      nonzero_batch, "Rust STO does not support nonzero Mako metadata");

  txn = db.new_txn(0, arena, nullptr);
  require(!tx_get(table, txn, rejected_batch_key_a, value));
  require(!tx_get(second_table, txn, rejected_batch_key_b, value));
  require(db.commit_txn(txn));

  // A fixed-modify replacement is borrowed only for the callback's dynamic
  // extent. Validate and shorten that same lease before returning it to Rust;
  // callback failure aborts the attempt before the exception is rethrown.
  auto expect_fixed_modify_rejection = [&](std::string rejected,
                                           const char *expected_message) {
    txn = db.new_txn(0, arena, nullptr);
    replacement_fixed_modify callback(std::move(rejected));
    bool rejected_as_expected = false;
    try {
      require(tx_modify_fixed_if_supported(
          db.txn_fixed_modify_capability(table),
          [&](TxnFixedModifyCapability &capability) {
            capability.tx_modify_fixed(txn, fixed_key, sizeof(fixed_key), 1,
                                       callback);
          }));
    } catch (const std::invalid_argument &error) {
      rejected_as_expected = std::string(error.what()) == expected_message;
    }
    require(rejected_as_expected);
    require(callback.visits() == 1);
  };
  expect_fixed_modify_rejection(
      "x", "Rust STO received a truncated Mako metadata suffix");
  std::string nonzero_modify = mako::Encode("rejected-modify");
  nonzero_modify[nonzero_modify.size() - mako::EXTRA_BITS_FOR_VALUE + 11] = 1;
  expect_fixed_modify_rejection(
      std::move(nonzero_modify),
      "Rust STO does not support nonzero Mako metadata");

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "upsert-fixed");
  require(db.commit_txn(txn));

  // Raw shard writes use the same canonical-zero input contract. The wrapper
  // never normalizes or drops nonzero metadata on this otherwise unsupported
  // distributed surface.
  const std::string rejected_shard_key("rejected-shard-key");
  auto expect_shard_put_rejection = [&](const std::string &rejected,
                                        const char *expected_message) {
    txn = db.new_txn(0, arena, nullptr);
    bool rejected_as_expected = false;
    try {
      (void)raw_table->shard_put(rejected_shard_key, rejected);
    } catch (const std::invalid_argument &error) {
      rejected_as_expected = std::string(error.what()) == expected_message;
    }
    require(rejected_as_expected);
    db.abort_txn(txn);
  };
  expect_shard_put_rejection(
      "x", "Rust STO received a truncated Mako metadata suffix");
  std::string nonzero_shard = mako::Encode("rejected-shard");
  nonzero_shard[nonzero_shard.size() - mako::EXTRA_BITS_FOR_VALUE + 12] = 1;
  expect_shard_put_rejection(
      nonzero_shard, "Rust STO does not support nonzero Mako metadata");

  txn = db.new_txn(0, arena, nullptr);
  require(!raw_table->shard_get(rejected_shard_key, value,
                                std::string::npos));
  require(db.commit_txn(txn));

  // A C++ exception is converted to FAILED until Rust has aborted and
  // returned, then rethrown with the wrapper TLS state already inactive.
  txn = db.new_txn(0, arena, nullptr);
  throwing_fixed_modify throwing_modify;
  caught = false;
  try {
    require(tx_modify_fixed_if_supported(
        db.txn_fixed_modify_capability(table),
        [&](TxnFixedModifyCapability &capability) {
          capability.tx_modify_fixed(txn, fixed_key, sizeof(fixed_key), 1,
                                     throwing_modify);
        }));
  } catch (const std::runtime_error &error) {
    caught = std::string(error.what()) == "expected fixed mutation failure";
  }
  require(caught);

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "upsert-fixed");
  require(db.commit_txn(txn));

  // Scalar writes fail synchronously before FFI. Exercise each byte in the
  // complete suffix so this optimization cannot accidentally validate only
  // the timestamp, pointer, or padding subset of Mako's metadata layout.
  const char rejected_scalar_key[8] = {'r', 'e', 'j', 's', 'c', 'a', 'l', 'r'};
  auto expect_scalar_put_rejection = [&](const std::string &rejected,
                                         const char *expected_message) {
    txn = db.new_txn(0, arena, nullptr);
    bool rejected_as_expected = false;
    try {
      tx_put(table, txn,
             lcdf::Str(rejected_scalar_key, sizeof(rejected_scalar_key)),
             rejected);
    } catch (const std::invalid_argument &error) {
      rejected_as_expected = std::string(error.what()) == expected_message;
    }
    require(rejected_as_expected);
    db.abort_txn(txn);
  };
  expect_scalar_put_rejection(
      "x", "Rust STO received a truncated Mako metadata suffix");
  for (size_t byte = 0;
       byte < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE); ++byte) {
    std::string nonzero_scalar = mako::Encode("rejected-scalar");
    const size_t suffix_begin =
        nonzero_scalar.size() - mako::EXTRA_BITS_FOR_VALUE;
    nonzero_scalar[suffix_begin + byte] = 1;
    expect_scalar_put_rejection(
        nonzero_scalar, "Rust STO does not support nonzero Mako metadata");
  }

  txn = db.new_txn(0, arena, nullptr);
  require(!tx_get(table, txn,
                  lcdf::Str(rejected_scalar_key,
                            sizeof(rejected_scalar_key)),
                  value));
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(table, txn, lcdf::Str(fixed_key, sizeof(fixed_key)), value));
  require(value == "upsert-fixed");
  require(db.commit_txn(txn));

  // Exercise the private Payment ABI across C++ and Rust, including the
  // name-index scan and caller-buffer lifetime through commit and abort.
  const warehouse::key payment_w_key(1);
  warehouse::value payment_w_value;
  payment_w_value.w_ytd = 100.0f;
  payment_w_value.w_tax = 0.1f;
  payment_w_value.w_name.assign("WAREHOUSE");

  const district::key payment_d_key(1, 1);
  district::value payment_d_value;
  payment_d_value.d_ytd = 200.0f;
  payment_d_value.d_tax = 0.2f;
  payment_d_value.d_next_o_id = 3'001;
  payment_d_value.d_name.assign("DISTRICT");

  const customer::key payment_c_key(1, 1, 7);
  customer::value payment_c_value;
  payment_c_value.c_discount = 0.05f;
  payment_c_value.c_credit.assign("GC");
  payment_c_value.c_last.assign("LAST");
  payment_c_value.c_first.assign("ALICE");
  payment_c_value.c_credit_lim = 50'000.0f;
  payment_c_value.c_balance = 1'000.0f;
  payment_c_value.c_ytd_payment = 25.0f;
  payment_c_value.c_payment_cnt = 1;
  payment_c_value.c_delivery_cnt = 2;
  payment_c_value.c_since = 123;

  const std::string payment_last_name(16, 'L');
  const std::string payment_first_name("ALICE");
  const std::string name_lower_first(16, '\0');
  const std::string name_upper_first(16, static_cast<char>(0xff));
  const customer_name_idx::key payment_name_key(
      1, 1, inline_str_fixed<16>(payment_last_name),
      inline_str_fixed<16>(payment_first_name));
  const customer_name_idx::key payment_name_lower_key(
      1, 1, inline_str_fixed<16>(payment_last_name),
      inline_str_fixed<16>(name_lower_first));
  const customer_name_idx::key payment_name_upper_key(
      1, 1, inline_str_fixed<16>(payment_last_name),
      inline_str_fixed<16>(name_upper_first));
  const customer_name_idx::value payment_name_value(7);

  const std::string encoded_payment_w_value = Encode(payment_w_value);
  const std::string encoded_payment_d_value = Encode(payment_d_value);
  const std::string encoded_payment_c_value = Encode(payment_c_value);
  const std::string encoded_payment_name_value =
      Encode(payment_name_value);
  txn = db.new_txn(0, arena, nullptr);
  tx_put(payment_warehouse, txn, EncodeK(payment_w_key),
         encoded_payment_w_value);
  tx_put(payment_district, txn, EncodeK(payment_d_key),
         encoded_payment_d_value);
  tx_put(payment_customer, txn, EncodeK(payment_c_key),
         encoded_payment_c_value);
  tx_put(payment_customer_name, txn, EncodeK(payment_name_key),
         encoded_payment_name_value);
  require(db.commit_txn(txn));

  std::array<uint8_t, 4> payment_w_key_bytes;
  std::array<uint8_t, 8> payment_d_key_bytes;
  std::array<uint8_t, 12> payment_c_key_bytes;
  std::array<uint8_t, 40> payment_name_lower_bytes;
  std::array<uint8_t, 40> payment_name_upper_bytes;
  encoder<warehouse::key>().write(payment_w_key_bytes.data(),
                                  &payment_w_key);
  encoder<district::key>().write(payment_d_key_bytes.data(),
                                 &payment_d_key);
  const customer::key payment_c_prefix_key(1, 1, 0);
  encoder<customer::key>().write(payment_c_key_bytes.data(),
                                 &payment_c_prefix_key);
  encoder<customer_name_idx::key>().write(payment_name_lower_bytes.data(),
                                           &payment_name_lower_key);
  encoder<customer_name_idx::key>().write(payment_name_upper_bytes.data(),
                                           &payment_name_upper_key);

  std::array<char, tpcc_fixed_batch::payment_value_capacity> payment_w_out;
  std::array<char, tpcc_fixed_batch::payment_value_capacity> payment_d_out;
  std::array<char, tpcc_fixed_batch::payment_value_capacity> payment_c_out;
  constexpr char payment_w_sentinel = static_cast<char>(0xa1);
  constexpr char payment_d_sentinel = static_cast<char>(0xb2);
  constexpr char payment_c_sentinel = static_cast<char>(0xc3);
  payment_w_out.fill(payment_w_sentinel);
  payment_d_out.fill(payment_d_sentinel);
  payment_c_out.fill(payment_c_sentinel);
  txn = db.new_txn(0, arena, nullptr);
  tpcc_fixed_batch::payment_prefix_request payment_request{
      txn,
      payment_warehouse,
      payment_district,
      payment_customer,
      payment_customer_name,
      reinterpret_cast<const char *>(payment_w_key_bytes.data()),
      reinterpret_cast<const char *>(payment_d_key_bytes.data()),
      reinterpret_cast<const char *>(payment_c_key_bytes.data()),
      reinterpret_cast<const char *>(payment_name_lower_bytes.data()),
      reinterpret_cast<const char *>(payment_name_upper_bytes.data()),
      0,
      7.25f,
      true,
      payment_w_out.data(),
      payment_d_out.data(),
      payment_c_out.data()};
  const tpcc_fixed_batch::payment_prefix_result payment_result =
      db.txn_tpcc_payment_capability()->tx_payment_prefix(payment_request);
  require(payment_result.customer_id == 7);
  warehouse::value patched_w;
  district::value patched_d;
  customer::value patched_c;
  require(encoder<warehouse::value>().failsafe_read(
              reinterpret_cast<const uint8_t *>(payment_w_out.data()),
              payment_result.warehouse_value_length, &patched_w) != nullptr);
  require(encoder<district::value>().failsafe_read(
              reinterpret_cast<const uint8_t *>(payment_d_out.data()),
              payment_result.district_value_length, &patched_d) != nullptr);
  require(encoder<customer::value>().failsafe_read(
              reinterpret_cast<const uint8_t *>(payment_c_out.data()),
              payment_result.customer_value_length, &patched_c) != nullptr);
  require(suffix_equals(payment_w_out, payment_result.warehouse_value_length,
                        payment_w_sentinel));
  require(suffix_equals(payment_d_out, payment_result.district_value_length,
                        payment_d_sentinel));
  require(suffix_equals(payment_c_out, payment_result.customer_value_length,
                        payment_c_sentinel));
  require(patched_w.w_ytd == 107.25f);
  require(patched_d.d_ytd == 207.25f);
  require(patched_c.c_balance == 992.75f);
  require(patched_c.c_ytd_payment == 32.25f);
  require(patched_c.c_payment_cnt == 2);
  require(db.commit_txn(txn));

  // Committed storage must no longer depend on the borrowed output bytes.
  payment_w_out.fill(0x31);
  payment_d_out.fill(0x42);
  payment_c_out.fill(0x53);
  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(payment_warehouse, txn, EncodeK(payment_w_key), value));
  Decode(value, patched_w);
  require(patched_w.w_ytd == 107.25f);
  require(tx_get(payment_district, txn, EncodeK(payment_d_key), value));
  Decode(value, patched_d);
  require(patched_d.d_ytd == 207.25f);
  require(tx_get(payment_customer, txn, EncodeK(payment_c_key), value));
  Decode(value, patched_c);
  require(patched_c.c_balance == 992.75f);
  require(patched_c.c_ytd_payment == 32.25f);
  require(patched_c.c_payment_cnt == 2);
  require(db.commit_txn(txn));

  txn = db.new_txn(0, arena, nullptr);
  payment_request.txn = txn;
  payment_request.customer_name = nullptr;
  payment_request.customer_name_lower = nullptr;
  payment_request.customer_name_upper = nullptr;
  payment_request.customer_id = 7;
  payment_request.payment_amount = 3.5f;
  payment_request.customer_by_name = false;
  (void)db.txn_tpcc_payment_capability()->tx_payment_prefix(payment_request);
  db.abort_txn(txn);
  payment_w_out.fill(0x64);
  payment_d_out.fill(0x75);
  payment_c_out.fill(static_cast<char>(0x86));
  txn = db.new_txn(0, arena, nullptr);
  require(tx_get(payment_warehouse, txn, EncodeK(payment_w_key), value));
  Decode(value, patched_w);
  require(patched_w.w_ytd == 107.25f);
  require(tx_get(payment_district, txn, EncodeK(payment_d_key), value));
  Decode(value, patched_d);
  require(patched_d.d_ytd == 207.25f);
  require(tx_get(payment_customer, txn, EncodeK(payment_c_key), value));
  Decode(value, patched_c);
  require(patched_c.c_balance == 992.75f);
  require(patched_c.c_ytd_payment == 32.25f);
  require(patched_c.c_payment_cnt == 2);
  require(db.commit_txn(txn));

  db.thread_end();
  return 0;
}
