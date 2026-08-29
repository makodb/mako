#include "rust_sto_tpcc_wrapper.hh"

#include <algorithm>
#include <array>
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

namespace {

mbta_wrapper &legacy_thread_support() {
  // TPC-C contains a few direct TThread reads outside abstract_db. Reuse the
  // native wrapper's established thread bring-up so those application-level
  // fields and its one-shard ShardClient retain identical values. No native
  // C++ table is allocated through this support object.
  static mbta_wrapper support;
  return support;
}

std::string_view tpcc_tablespace_name(std::string_view index_name) {
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

sto_tpcc_table_config tpcc_table_config_for(std::string_view index_name) {
  constexpr uint64_t kMiB = uint64_t{1} << 20;
  constexpr uint64_t kStaticRetained = 262'144;
  constexpr uint64_t kStaticConsumed = 524'288;
  constexpr uint64_t kGrowthRetained = 4'000'000;
  constexpr uint64_t kGrowthConsumed = 6'000'000;
  constexpr uint64_t kOrderLineRetained = 16'000'000;
  constexpr uint64_t kOrderLineConsumed = 20'000'000;

  // Paper-style TPC-C forces one tree per warehouse, so increasing the
  // 1/4/8/16-thread scale increases the table count rather than one
  // table's initial cardinality. The largest initial table is order_line at
  // about 300k rows per warehouse. Its exact encoded key is 16 bytes; the
  // 512 MiB key quota therefore still has a 2x margin at the 16M row cap.
  // The remaining 15.7M order-line slots cover over 1.04M maximum-size
  // (15-line) new-order transactions per warehouse after loading.
  const std::string_view tablespace = tpcc_tablespace_name(index_name);
  const bool static_cardinality =
      tablespace == "customer" || tablespace == "customer_name_idx" ||
      tablespace == "district" || tablespace == "item" ||
      tablespace == "stock" || tablespace == "stock_data" ||
      tablespace == "warehouse";

  sto_tpcc_table_config config{};
  if (tablespace == "order_line") {
    config.max_retained_records = kOrderLineRetained;
    config.max_consumed_record_ids = kOrderLineConsumed;
    config.max_retained_key_bytes = 512 * kMiB;
  } else if (static_cardinality) {
    config.max_retained_records = kStaticRetained;
    config.max_consumed_record_ids = kStaticConsumed;
    config.max_retained_key_bytes = 128 * kMiB;
  } else {
    // history, new_order, oorder, and oorder_c_id_idx each consume at most
    // one new key per corresponding TPC-C transaction. Keep this conservative
    // tier as the fallback so an unexpected table name fails by a clear bound
    // instead of silently receiving an unbounded registry.
    config.max_retained_records = kGrowthRetained;
    config.max_consumed_record_ids = kGrowthConsumed;
    config.max_retained_key_bytes = 512 * kMiB;
  }

  // LazySegmented reserves one 40-byte OnceLock directory cell per 1,024
  // consumed IDs and allocates the 48-byte records only as segments are used.
  // At 18 warehouses these tiers reserve about 31.7 MiB of directory cells,
  // versus about 79.0 MiB for the former uniform 8M-ID limit.
  config.scan_chunk_records = 128;
  config.scan_initial_key_arena_bytes = 16 * 1024;
  config.scan_max_key_arena_bytes = 64 * 1024;
  config.max_scan_chunks = 32'768;
  config.max_scan_physical_records = 4'000'000;
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

void strip_mako_value_metadata(std::string &value) {
  if (value.size() < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE))
    throw std::runtime_error("Rust STO returned a truncated Mako value");
  value.resize(value.size() - mako::EXTRA_BITS_FOR_VALUE);
}

struct scan_bridge {
  oi_scan_callback *callback;
  bool strip_metadata;
  std::string value;
  std::exception_ptr failure;
};

int32_t invoke_scan_bridge(void *context, const uint8_t *key,
                           size_t key_length, const uint8_t *value,
                           size_t value_length) {
  auto *bridge = static_cast<scan_bridge *>(context);
  try {
    bridge->value.assign(reinterpret_cast<const char *>(value), value_length);
    if (bridge->strip_metadata)
      strip_mako_value_metadata(bridge->value);
    return bridge->callback->invoke(reinterpret_cast<const char *>(key),
                                    key_length, bridge->value)
               ? 0
               : 1;
  } catch (...) {
    // Never unwind C++ through a Rust frame. Stop the scan, then rethrow from
    // scan_raw after the extern "C" call has returned.
    bridge->failure = std::current_exception();
    return 1;
  }
}

} // namespace

thread_local sto_tpcc_thread *rust_sto_tpcc_wrapper::tls_thread_ = nullptr;
thread_local bool rust_sto_tpcc_wrapper::tls_transaction_active_ = false;
thread_local bool rust_sto_tpcc_wrapper::tls_legacy_thread_initialized_ =
    false;

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
                                      size_t) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      const bool found = owner_->get_raw(table_, key, value);
      if (!owner_->commit_txn(owner_->tls_thread_))
        continue;
      if (found)
        strip_mako_value_metadata(value);
      return found;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    }
  }
}

bool rust_sto_tpcc_ordered_index::put(lcdf::Str key,
                                      const std::string &value) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      std::string ignored;
      const bool existed = owner_->get_raw(table_, key, ignored);
      const std::string encoded = mako::Encode(value);
      owner_->put_raw(table_, key, encoded, false);
      owner_->commit_txn(owner_->tls_thread_);
      return !existed;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
    }
  }
}

bool rust_sto_tpcc_ordered_index::insert(lcdf::Str key,
                                         const std::string &value) {
  while (true) {
    owner_->begin_current_transaction();
    try {
      const std::string encoded = mako::Encode(value);
      const sto_tpcc_status status = sto_tpcc_insert(
          owner_->tls_thread_, table_,
          reinterpret_cast<const uint8_t *>(key.data()), key.length(),
          reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size());
      if (status == STO_TPCC_DUPLICATE) {
        owner_->abort_current_transaction_noexcept();
        return false;
      }
      owner_->require_ok("insert", status);
      owner_->commit_txn(owner_->tls_thread_);
      return true;
    } catch (const abstract_db::abstract_abort_exception &) {
      owner_->abort_current_transaction_noexcept();
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
                                         std::string &value, size_t) {
  owner_->require_txn_handle(txn);
  const bool found = owner_->get_raw(table_, key, value);
  if (found)
    strip_mako_value_metadata(value);
  return found;
}

void rust_sto_tpcc_ordered_index::tx_put(c_void *txn, lcdf::Str key,
                                         const std::string &value) {
  owner_->require_txn_handle(txn);
  owner_->put_raw(table_, key, value, false);
}

void rust_sto_tpcc_ordered_index::tx_insert(c_void *txn, lcdf::Str key,
                                            const std::string &value) {
  owner_->require_txn_handle(txn);
  owner_->put_raw(table_, key, value, true);
}

void rust_sto_tpcc_ordered_index::tx_remove(c_void *txn, lcdf::Str key) {
  owner_->require_txn_handle(txn);
  (void)owner_->remove_raw(table_, key);
}

void rust_sto_tpcc_ordered_index::tx_scan(c_void *txn,
                                          const std::string &start_key,
                                          const std::string *end_key,
                                          oi_scan_callback &callback,
                                          str_arena *) {
  owner_->require_txn_handle(txn);
  owner_->scan_raw(table_, false, start_key, end_key, callback, true);
}

void rust_sto_tpcc_ordered_index::tx_rscan(c_void *txn,
                                           const std::string &start_key,
                                           const std::string *end_key,
                                           oi_scan_callback &callback,
                                           str_arena *) {
  owner_->require_txn_handle(txn);
  owner_->scan_raw(table_, true, start_key, end_key, callback, true);
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
  owner_->scan_raw(table_, false, start_key, &end_key, callback, true);
}

bool rust_sto_tpcc_ordered_index::shard_get(lcdf::Str key,
                                            std::string &value, size_t) {
  return owner_->get_raw(table_, key, value);
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

void rust_sto_tpcc_wrapper::preallocate_open_index() {}
ssize_t rust_sto_tpcc_wrapper::txn_max_batch_size() const { return 100; }
size_t rust_sto_tpcc_wrapper::sizeof_txn_object(uint64_t) const { return 1; }

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

void *rust_sto_tpcc_wrapper::new_txn(uint64_t, str_arena &, void *,
                                     TxnProfileHint) {
  begin_current_transaction();
  return tls_thread_;
}

bool rust_sto_tpcc_wrapper::commit_txn(void *txn) {
  require_txn_handle(txn);
  const sto_tpcc_status status = sto_tpcc_txn_commit(tls_thread_);
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

  const sto_tpcc_table_config table_config = tpcc_table_config_for(name);
  sto_tpcc_table *table = nullptr;
  require_ok("table_create",
             sto_tpcc_table_create(db_, &table_config, &table));

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
  require_ok("txn_begin", sto_tpcc_txn_begin(tls_thread_));
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
                                    std::string &encoded_value) {
  require_txn_handle(tls_thread_);
  // sto_tpcc_get initializes exactly `actual` bytes. Avoid clearing the full
  // 1 KiB fallback buffer on every transactional point read.
  std::array<uint8_t, 1024> local;
  size_t actual = 0;
  sto_tpcc_status status = sto_tpcc_get(
      tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
      key.length(), local.data(), local.size(), &actual);
  if (status == STO_TPCC_MISS) {
    encoded_value.clear();
    return false;
  }
  if (status == STO_TPCC_BUFFER_TOO_SMALL) {
    std::vector<uint8_t> dynamic(actual);
    status = sto_tpcc_get(
        tls_thread_, table, reinterpret_cast<const uint8_t *>(key.data()),
        key.length(), dynamic.data(), dynamic.size(), &actual);
    require_ok("get", status);
    encoded_value.assign(reinterpret_cast<const char *>(dynamic.data()), actual);
    return true;
  }
  require_ok("get", status);
  encoded_value.assign(reinterpret_cast<const char *>(local.data()), actual);
  return true;
}

void rust_sto_tpcc_wrapper::put_raw(const sto_tpcc_table *table,
                                    lcdf::Str key,
                                    const std::string &encoded_value,
                                    bool insert_only) {
  require_txn_handle(tls_thread_);
  const sto_tpcc_status status =
      insert_only
          ? sto_tpcc_insert(
                tls_thread_, table,
                reinterpret_cast<const uint8_t *>(key.data()), key.length(),
                reinterpret_cast<const uint8_t *>(encoded_value.data()),
                encoded_value.size())
          : sto_tpcc_put(
                tls_thread_, table,
                reinterpret_cast<const uint8_t *>(key.data()), key.length(),
                reinterpret_cast<const uint8_t *>(encoded_value.data()),
                encoded_value.size());
  // MassTrans::transInsert returns "already existed" without modifying the
  // row; the legacy abstract wrapper discards that return value.  Preserve
  // that validated no-op behavior here.  The separate nontransactional
  // insert() surface still reports a duplicate as `false` to its caller.
  if (status == STO_TPCC_DUPLICATE && insert_only)
    return;
  require_ok(insert_only ? "insert" : "put", status);
}

bool rust_sto_tpcc_wrapper::remove_raw(const sto_tpcc_table *table,
                                       lcdf::Str key) {
  require_txn_handle(tls_thread_);
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
  scan_bridge bridge{&callback, strip_value_metadata, {}, {}};
  size_t visited = 0;
  const auto *start = reinterpret_cast<const uint8_t *>(start_key.data());
  const auto *end = end_key
                        ? reinterpret_cast<const uint8_t *>(end_key->data())
                        : nullptr;
  const sto_tpcc_status status = sto_tpcc_scan(
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
