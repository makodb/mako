#ifndef MAKO_STORAGE_RUST_STO_TPCC_WRAPPER_HH
#define MAKO_STORAGE_RUST_STO_TPCC_WRAPPER_HH

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "abstract_db.h"
#include "sto_tpcc_ffi.h"

class rust_sto_tpcc_wrapper;

// FullOrderedIndex bridge used only by the matched TPC-C comparison target.
// The application and record encoders remain the existing C++ TPC-C code;
// all transactional storage operations below cross the narrow Rust C ABI.
class rust_sto_tpcc_ordered_index final : public FullOrderedIndex {
public:
  rust_sto_tpcc_ordered_index(rust_sto_tpcc_wrapper *owner,
                              sto_tpcc_table *table,
                              int32_t table_id,
                              std::string name,
                              bool is_remote);
  ~rust_sto_tpcc_ordered_index() noexcept override;

  bool get(lcdf::Str key, std::string &value, size_t max_bytes_read) override;
  bool put(lcdf::Str key, const std::string &value) override;
  bool insert(lcdf::Str key, const std::string &value) override;
  bool remove(lcdf::Str key) override;
  void scan(const std::string &start_key, const std::string *end_key,
            oi_scan_callback &callback, str_arena *arena) override;
  void rscan(const std::string &start_key, const std::string *end_key,
             oi_scan_callback &callback, str_arena *arena) override;
  size_t size() const override;
  oi_stats_map clear() override;
  int32_t get_table_id() override;
  bool get_is_remote() override;

  bool tx_get(c_void *txn, lcdf::Str key, std::string &value,
              size_t max_bytes_read) override;
  void tx_put(c_void *txn, lcdf::Str key, const std::string &value) override;
  void tx_insert(c_void *txn, lcdf::Str key,
                 const std::string &value) override;
  void tx_remove(c_void *txn, lcdf::Str key) override;
  void tx_scan(c_void *txn, const std::string &start_key,
               const std::string *end_key, oi_scan_callback &callback,
               str_arena *arena) override;
  void tx_rscan(c_void *txn, const std::string &start_key,
                const std::string *end_key, oi_scan_callback &callback,
                str_arena *arena) override;
  void tx_scan_remote_one(c_void *txn, const std::string &start_key,
                          const std::string &end_key,
                          std::string &value) override;

  bool shard_get(lcdf::Str key, std::string &value,
                 size_t max_bytes_read) override;
  const c_char *shard_put(lcdf::Str key, const std::string &value) override;
  bool shard_scan(const std::string &start_key, const std::string *end_key,
                  oi_scan_callback &callback, str_arena *arena) override;

private:
  rust_sto_tpcc_wrapper *owner_;
  sto_tpcc_table *table_;
  int32_t table_id_;
  std::string name_;
  bool is_remote_;

  friend class rust_sto_tpcc_wrapper;
};

class rust_sto_tpcc_wrapper final : public abstract_db {
public:
  rust_sto_tpcc_wrapper();
  ~rust_sto_tpcc_wrapper() noexcept override;

  void init() override;
  void preallocate_open_index() override;
  ssize_t txn_max_batch_size() const override;
  size_t sizeof_txn_object(uint64_t txn_flags) const override;
  void thread_init(bool loader, int source = 0) override;
  void thread_end() override;
  void *new_txn(uint64_t txn_flags, str_arena &arena, void *buf,
                TxnProfileHint hint = HINT_DEFAULT) override;
  bool commit_txn(void *txn) override;
  bool commit_txn_no_paxos(void *txn) override;
  void abort_txn(void *txn) override;
  void abort_txn_local(void *txn) override;

  abstract_ordered_index *get_index_by_table_id(
      unsigned short table_id) override;
  abstract_ordered_index *open_index(const std::string &name,
                                     size_t value_size_hint,
                                     bool mostly_append = false,
                                     bool use_hashtable = false) override;
  abstract_ordered_index *open_index(const std::string &name,
                                     int shard_index = -1) override;
  void close_index(abstract_ordered_index *idx) override;

  void shard_abort_txn(void *txn) override;
  int shard_validate() override;
  void shard_install(uint32_t timestamp) override;
  void shard_serialize_util(uint32_t timestamp) override;
  void shard_unlock(bool committed) override;
  void shard_reset() override;

private:
  sto_tpcc_db *db_;
  int32_t next_table_id_;
  std::vector<std::unique_ptr<rust_sto_tpcc_ordered_index>> tables_;
  std::unordered_map<int32_t, rust_sto_tpcc_ordered_index *> tables_by_id_;
  std::map<std::tuple<std::string, int>, rust_sto_tpcc_ordered_index *>
      tables_by_name_;

  static thread_local sto_tpcc_thread *tls_thread_;
  static thread_local bool tls_transaction_active_;
  static thread_local bool tls_legacy_thread_initialized_;

  [[noreturn]] static void throw_fatal(const char *operation,
                                       sto_tpcc_status status);
  static void require_ok(const char *operation, sto_tpcc_status status);
  static void require_txn_handle(c_void *txn);
  static void begin_current_transaction();
  static void abort_current_transaction_noexcept();

  bool get_raw(const sto_tpcc_table *table, lcdf::Str key,
               std::string &encoded_value);
  void put_raw(const sto_tpcc_table *table, lcdf::Str key,
               const std::string &encoded_value, bool insert_only);
  bool remove_raw(const sto_tpcc_table *table, lcdf::Str key);
  void scan_raw(const sto_tpcc_table *table, bool reverse,
                const std::string &start_key, const std::string *end_key,
                oi_scan_callback &callback, bool strip_value_metadata);

  friend class rust_sto_tpcc_ordered_index;
};

#endif
