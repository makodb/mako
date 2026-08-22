// mako_local_abi.cc - exception-contained C facade over local STO/MassTrans.

#include "storage/mako_local_abi.h"

#include "lib/common.h"
#include "sto/MassTrans.hh"
#include "sto/StringWrapper.hh"
#include "sto/thread_registration.hh"

#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if STO_OPACITY
using mako_local_table_impl =
    MassTrans<std::string, versioned_str_struct, true /* opacity */>;
#else
using mako_local_table_impl =
    MassTrans<std::string, versioned_str_struct, false /* opacity */>;
#endif

struct mako_local_table {
  mako_local_db *owner;
  mako_local_table_impl *table;
  uint64_t id;
};

struct mako_local_db {
  std::mutex tables_mu;
  std::unordered_map<std::string, std::unique_ptr<mako_local_table>> tables;
  std::atomic<size_t> active_txns{0};
};

struct mako_local_txn {
  mako_local_db *owner;
  std::thread::id owner_thread;
  bool active;
  bool poisoned;
  size_t item_budget_used;

  // StringWrapper retains a pointer until commit. deque push_back preserves
  // references, including the address of small-string-optimised payloads, so
  // every write gets a distinct stable owner for the full transaction.
  std::deque<std::string> encoded_values;
  std::unordered_map<mako_local_table *, std::unordered_set<std::string>>
      mutated_keys;
};

namespace {

thread_local bool local_attached = false;
thread_local mako_local_txn *local_active_txn = nullptr;

bool valid_slice(const uint8_t *p, size_t n) {
  return p != nullptr || n == 0;
}

lcdf::Str as_key(const uint8_t *p, size_t n) {
  static const char empty = 0;
  const char *bytes = n == 0 ? &empty : reinterpret_cast<const char *>(p);
  return lcdf::Str(bytes, static_cast<int>(n));
}

bool on_owner_thread(const mako_local_txn *txn) {
  return txn->owner_thread == std::this_thread::get_id();
}

void finish_txn(mako_local_txn *txn) {
  if (!txn->active) return;
  txn->active = false;
  txn->poisoned = false;
  txn->item_budget_used = 0;
  if (local_active_txn == txn) local_active_txn = nullptr;
  txn->owner->active_txns.fetch_sub(1, std::memory_order_acq_rel);
  txn->encoded_values.clear();
  txn->mutated_keys.clear();
}

bool abort_and_finish(mako_local_txn *txn) {
  if (!txn->active) return true;
  // Transaction::stop() has no cleanup progress record. Retrying after a
  // partial exception could double-unlock or remove an inserted tuple twice.
  if (txn->poisoned) return false;
  try {
    Sto::silent_abort();
  } catch (...) {
    // Native cleanup may still retain StringWrapper pointers. Quarantine the
    // transaction and worker rather than freeing those buffers or pretending
    // the TLS transaction can be reused safely.
    txn->poisoned = true;
    return false;
  }
  finish_txn(txn);
  return true;
}

int check_txn(mako_local_txn *txn, mako_local_table *table = nullptr) {
  if (txn == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  if (!on_owner_thread(txn)) return MAKO_LOCAL_WRONG_THREAD;
  if (!txn->active || local_active_txn != txn) return MAKO_LOCAL_TXN_FINISHED;
  if (txn->poisoned) return MAKO_LOCAL_INTERNAL;
  if (table != nullptr && table->owner != txn->owner)
    return MAKO_LOCAL_WRONG_DB_OR_TABLE;
  return MAKO_LOCAL_OK;
}

int operation_abort(mako_local_txn *txn, int status) {
  return abort_and_finish(txn) ? status : MAKO_LOCAL_INTERNAL;
}

constexpr size_t kMasstreeSliceBytes = sizeof(uint64_t);

size_t write_item_charge(size_t key_len) {
  // A missing SET can observe the old leaf, create at most one new leaf per
  // eight-byte Masstree trie slice, and add its value item. Four fixed credits
  // also cover the existing-value resize path and cursor bookkeeping.
  return 4 + (key_len + kMasstreeSliceBytes - 1) / kMasstreeSliceBytes;
}

int reserve_item_budget(mako_local_txn *txn, size_t charge) {
  static_assert(MAKO_LOCAL_TXN_ITEM_BUDGET <=
                Transaction::tset_initial_capacity);
  if (txn->item_budget_used > MAKO_LOCAL_TXN_ITEM_BUDGET ||
      charge > MAKO_LOCAL_TXN_ITEM_BUDGET - txn->item_budget_used)
    return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
  txn->item_budget_used += charge;
  return MAKO_LOCAL_OK;
}

struct post_validate_bridge {
  mako_local_post_validate_hook hook;
  void *context;
};

bool invoke_post_validate_hook(void *opaque,
                               Transaction::tid_type timestamp) noexcept {
  auto *bridge = static_cast<post_validate_bridge *>(opaque);
  try {
    return bridge->hook(bridge->context, timestamp) != 0;
  } catch (...) {
    // Raw C++ callers can still supply a throwing function despite the C
    // declaration. Contain it before it can cross either the transaction core
    // or the C ABI. Rust's safe trampoline separately catches Rust panics in
    // unwind-enabled builds; panic=abort builds terminate before unwinding.
    return false;
  }
}

}  // namespace

extern "C" {

uint32_t mako_local_abi_version(void) noexcept {
  return MAKO_LOCAL_ABI_VERSION;
}

uint64_t mako_local_feature_bits(void) noexcept {
  uint64_t features = MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS;
  // Do not key this bit directly off READ_MY_WRITES. Several MassTrans paths,
  // including point get and scans, still have their RYW logic commented out,
  // so even an STO_RMW=ON build does not yet satisfy the public guarantee.
#if STO_OPACITY
  features |= MAKO_LOCAL_FEATURE_OPACITY;
#endif
  return features;
}

const char *mako_local_status_string(int status) noexcept {
  switch (status) {
    case MAKO_LOCAL_OK: return "ok";
    case MAKO_LOCAL_CONFLICT: return "transaction conflict";
    case MAKO_LOCAL_NOT_ATTACHED: return "thread not attached";
    case MAKO_LOCAL_WRONG_THREAD: return "transaction used from wrong thread";
    case MAKO_LOCAL_TXN_ALREADY_ACTIVE: return "transaction already active";
    case MAKO_LOCAL_TXN_FINISHED: return "transaction already finished";
    case MAKO_LOCAL_WRONG_DB_OR_TABLE: return "table belongs to another database";
    case MAKO_LOCAL_INVALID_ARGUMENT: return "invalid argument";
    case MAKO_LOCAL_THREAD_LIMIT: return "STO thread limit exhausted";
    case MAKO_LOCAL_BUSY: return "resource busy";
    case MAKO_LOCAL_OUT_OF_MEMORY: return "out of memory";
    case MAKO_LOCAL_INTERNAL: return "contained C++ failure";
    case MAKO_LOCAL_DUPLICATE_WRITE:
      return "second mutation of one key is not supported";
    case MAKO_LOCAL_TXN_TOO_LARGE:
      return "transaction exceeds the draft item budget";
    case MAKO_LOCAL_VALUE_TOO_LARGE:
      return "table name, key, or value exceeds the draft byte limit";
    case MAKO_LOCAL_COMMIT_HOOK_REJECTED:
      return "post-validation commit hook rejected transaction";
    case MAKO_LOCAL_TIMESTAMP_EXHAUSTED:
      return "STO commit timestamp exhausted";
    default: return "unknown mako-local status";
  }
}

int mako_local_advance_commit_tid_past(uint64_t observed) noexcept {
  if (observed == 0) return MAKO_LOCAL_INVALID_ARGUMENT;
  return Transaction::advance_tid_past(observed)
             ? MAKO_LOCAL_OK
             : MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
}

int mako_local_thread_attach(void) noexcept {
  if (local_attached) return MAKO_LOCAL_OK;
  try {
    // Do not replace native thread state underneath a live Mako transaction.
    if (Sto::in_progress()) return MAKO_LOCAL_BUSY;
    if (!mako::silo::claim_thread_runtime(
            mako::silo::thread_runtime::local_abi))
      return MAKO_LOCAL_BUSY;

    const int id = mako::silo::try_allocate_thread_id();
    if (id < 0) return MAKO_LOCAL_THREAD_LIMIT;

    TThread::set_id(id);
    // Sto::transaction() is process TLS and may have been created by legacy
    // direct STO code before that code adopted the shared allocator. We proved
    // it idle above, so refresh its cached owner ID before it can be reused.
    Sto::update_threadid();
    TThread::set_mode(0);
    TThread::set_shard_index(0);
    TThread::set_nshards(1);
    TThread::set_warehouses(1);
    TThread::set_pid(0);
    TThread::set_is_micro(0);
    TThread::disable_multiversion();
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    TThread::trans_nosend_abort = 0;
    TThread::increment_id = 0;
    TThread::sclient = nullptr;

    if (!mako::silo::ensure_epoch_runtime()) return MAKO_LOCAL_INTERNAL;
    mako_local_table_impl::thread_init();
    local_attached = true;
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

int mako_local_db_open(mako_local_db **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  try {
    *out = new mako_local_db();
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

int mako_local_db_close(mako_local_db *db) noexcept {
  if (db == nullptr) return MAKO_LOCAL_OK;
  try {
    if (db->active_txns.load(std::memory_order_acquire) != 0)
      return MAKO_LOCAL_BUSY;
    // Facade table handles are reclaimed here. Their MassTrans objects are
    // intentionally process-lifetime until Mako has a verified global RCU
    // quiescence protocol.
    delete db;
    return MAKO_LOCAL_OK;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

int mako_local_table_open(mako_local_db *db, const uint8_t *name,
                          size_t name_len, uint64_t table_id,
                          mako_local_table **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (db == nullptr || !valid_slice(name, name_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (name_len > MAKO_LOCAL_MAX_TABLE_NAME_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;

  try {
    const std::string table_name(
        name_len == 0 ? "" : reinterpret_cast<const char *>(name), name_len);
    std::lock_guard<std::mutex> guard(db->tables_mu);
    auto it = db->tables.find(table_name);
    if (it != db->tables.end()) {
      if (it->second->id != table_id) return MAKO_LOCAL_WRONG_DB_OR_TABLE;
      *out = it->second.get();
      return MAKO_LOCAL_OK;
    }
    for (const auto &[existing_name, existing] : db->tables) {
      (void)existing_name;
      if (existing->id == table_id) return MAKO_LOCAL_WRONG_DB_OR_TABLE;
    }

    // Retain ownership until every facade allocation and map insertion has
    // succeeded. On success MassTrans becomes intentionally process-lifetime.
    auto inner = std::make_unique<mako_local_table_impl>();
    inner->set_table_id(table_id);
    inner->set_is_remote(false);
    inner->set_table_name(table_name);

    auto table = std::make_unique<mako_local_table>();
    table->owner = db;
    table->table = inner.get();
    table->id = table_id;
    auto [inserted, did_insert] =
        db->tables.emplace(table_name, std::move(table));
    if (!did_insert) return MAKO_LOCAL_INTERNAL;
    inner.release();
    *out = inserted->second.get();
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

uint64_t mako_local_table_id(const mako_local_table *table) noexcept {
  return table == nullptr ? 0 : table->id;
}

int mako_local_txn_begin(mako_local_db *db, mako_local_txn **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (db == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;
  if (local_active_txn != nullptr)
    return local_active_txn->poisoned ? MAKO_LOCAL_INTERNAL
                                      : MAKO_LOCAL_TXN_ALREADY_ACTIVE;
  if (Sto::in_progress())
    return MAKO_LOCAL_TXN_ALREADY_ACTIVE;

  mako_local_txn *txn = nullptr;
  try {
    txn = new mako_local_txn{db, std::this_thread::get_id(), true, false, 0,
                             {}, {}};
    Sto::start_transaction();
    local_active_txn = txn;
    db->active_txns.fetch_add(1, std::memory_order_acq_rel);
    *out = txn;
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    if (txn != nullptr) delete txn;
    try { Sto::silent_abort(); } catch (...) {}
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    if (txn != nullptr) delete txn;
    try { Sto::silent_abort(); } catch (...) {}
    return MAKO_LOCAL_INTERNAL;
  }
}

int mako_local_txn_get(mako_local_txn *txn, mako_local_table *table,
                       const uint8_t *key, size_t key_len,
                       uint8_t **value_out, size_t *value_len_out,
                       uint8_t *found_out) noexcept {
  if (value_out == nullptr || value_len_out == nullptr || found_out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *value_out = nullptr;
  *value_len_out = 0;
  *found_out = 0;
  if (table == nullptr || !valid_slice(key, key_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
    const int reserved = reserve_item_budget(txn, 1);
    if (reserved != MAKO_LOCAL_OK) return reserved;
    std::string value;
    const bool found = table->table->transGet(as_key(key, key_len), value);
    if (TThread::transget_without_throw) {
      TThread::transget_without_throw = false;
      return operation_abort(txn, MAKO_LOCAL_CONFLICT);
    }
    if (!found) return MAKO_LOCAL_OK;
    if (value.size() < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE))
      return operation_abort(txn, MAKO_LOCAL_INTERNAL);

    value.resize(value.size() - mako::EXTRA_BITS_FOR_VALUE);
    void *bytes = std::malloc(value.empty() ? 1 : value.size());
    if (bytes == nullptr) return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
    if (!value.empty()) std::memcpy(bytes, value.data(), value.size());
    *value_out = static_cast<uint8_t *>(bytes);
    *value_len_out = value.size();
    *found_out = 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_abort(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_abort(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_put(mako_local_txn *txn, mako_local_table *table,
                       const uint8_t *key, size_t key_len,
                       const uint8_t *value, size_t value_len,
                       uint8_t *created_out) noexcept {
  if (created_out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *created_out = 0;
  if (table == nullptr || !valid_slice(key, key_len) ||
      !valid_slice(value, value_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES ||
      value_len > MAKO_LOCAL_MAX_VALUE_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
    auto &mutations = txn->mutated_keys[table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return MAKO_LOCAL_DUPLICATE_WRITE;
    const int reserved = reserve_item_budget(txn, write_item_charge(key_len));
    if (reserved != MAKO_LOCAL_OK) return reserved;
    const std::string raw(
        value_len == 0 ? "" : reinterpret_cast<const char *>(value), value_len);
    txn->encoded_values.push_back(mako::Encode(raw));
    const bool existed = table->table->transPut(
        as_key(key, key_len), StringWrapper(txn->encoded_values.back()));
    mutations.insert(mutation_key);
    *created_out = existed ? 0 : 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_abort(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_abort(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_insert(mako_local_txn *txn, mako_local_table *table,
                          const uint8_t *key, size_t key_len,
                          const uint8_t *value, size_t value_len,
                          uint8_t *inserted_out) noexcept {
  if (inserted_out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *inserted_out = 0;
  if (table == nullptr || !valid_slice(key, key_len) ||
      !valid_slice(value, value_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES ||
      value_len > MAKO_LOCAL_MAX_VALUE_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
    auto &mutations = txn->mutated_keys[table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return MAKO_LOCAL_DUPLICATE_WRITE;
    const int reserved = reserve_item_budget(txn, write_item_charge(key_len));
    if (reserved != MAKO_LOCAL_OK) return reserved;
    const std::string raw(
        value_len == 0 ? "" : reinterpret_cast<const char *>(value), value_len);
    txn->encoded_values.push_back(mako::Encode(raw));
    const bool existed = table->table->transInsert(
        as_key(key, key_len), StringWrapper(txn->encoded_values.back()));
    if (!existed) mutations.insert(mutation_key);
    *inserted_out = existed ? 0 : 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_abort(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_abort(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_remove(mako_local_txn *txn, mako_local_table *table,
                          const uint8_t *key, size_t key_len,
                          uint8_t *existed_out) noexcept {
  if (existed_out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *existed_out = 0;
  if (table == nullptr || !valid_slice(key, key_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
    auto &mutations = txn->mutated_keys[table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return MAKO_LOCAL_DUPLICATE_WRITE;
    const int reserved = reserve_item_budget(txn, 1);
    if (reserved != MAKO_LOCAL_OK) return reserved;
    const bool existed = table->table->transDelete(as_key(key, key_len));
    if (existed) mutations.insert(mutation_key);
    *existed_out = existed ? 1 : 0;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_abort(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_abort(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_commit(mako_local_txn *txn) noexcept {
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;
  try {
    const bool committed = Sto::try_commit_no_paxos();
    finish_txn(txn);
    return committed ? MAKO_LOCAL_OK : MAKO_LOCAL_CONFLICT;
  } catch (const Transaction::Abort &) {
    return operation_abort(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_abort(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_commit_with_hook(
    mako_local_txn *txn, mako_local_post_validate_hook hook,
    void *context) noexcept {
  if (hook == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;

  post_validate_bridge bridge{hook, context};
  Transaction::preinstall_failure failure =
      Transaction::preinstall_failure::none;
  try {
    const bool committed = Sto::try_commit_no_paxos(
        invoke_post_validate_hook, &bridge, &failure);
    finish_txn(txn);
    if (committed) return MAKO_LOCAL_OK;
    switch (failure) {
      case Transaction::preinstall_failure::hook_rejected:
        return MAKO_LOCAL_COMMIT_HOOK_REJECTED;
      case Transaction::preinstall_failure::timestamp_exhausted:
        return MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
      case Transaction::preinstall_failure::none:
        return MAKO_LOCAL_CONFLICT;
    }
  } catch (const Transaction::Abort &) {
    return operation_abort(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_abort(txn, MAKO_LOCAL_INTERNAL);
  }
  return operation_abort(txn, MAKO_LOCAL_INTERNAL);
}

int mako_local_txn_abort(mako_local_txn *txn) noexcept {
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;
  return abort_and_finish(txn) ? MAKO_LOCAL_OK : MAKO_LOCAL_INTERNAL;
}

int mako_local_txn_destroy(mako_local_txn *txn) noexcept {
  if (txn == nullptr) return MAKO_LOCAL_OK;
  if (!on_owner_thread(txn)) return MAKO_LOCAL_WRONG_THREAD;
  try {
    if (txn->active) {
      const int checked = check_txn(txn);
      if (checked != MAKO_LOCAL_OK) return checked;
    }
    if (!abort_and_finish(txn)) return MAKO_LOCAL_INTERNAL;
    delete txn;
    return MAKO_LOCAL_OK;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

void mako_local_bytes_free(void *bytes) noexcept {
  std::free(bytes);
}

}  // extern "C"
