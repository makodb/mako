#ifndef _BENCHMARK_MBTA_WRAPPER_H_
#define _BENCHMARK_MBTA_WRAPPER_H_
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include "abstract_db.h"
#include "abstract_ordered_index.h"
#include "sto/Transaction.hh"
#include "sto/MassTrans.hh"
#include "sto/Hashtable.hh"
#include "sto/simple_str.hh"
#include "sto/StringWrapper.hh"
#include <unordered_map>
#include <map>
#include <tuple>
#include <vector>
#include "benchmarks/tpcc.h"
#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "lib/table_registry.h"
#include "benchmarks/rpc_setup.h"
#include "mbta_sharded_ordered_index.hh"

// We have to do it on the coordinator instead of transaction.cc, because it only has a local copy of the readSet;
#define GET_NODE_POINTER(val,len) reinterpret_cast<mako::Node *>((char*)(val+len-mako::BITS_OF_NODE));
#define GET_NODE_EXTRA_POINTER(val,len) reinterpret_cast<uint32_t *>((char*)(val+len-mako::EXTRA_BITS_FOR_VALUE));
#define MAX(a,b) ((a)>(b)?(a):(b))

#if defined(FAIL_NEW_VERSION)
// control_mode==4, If a value is in the old epoch while this transaction is from the new epoch,  if not stable, we put it in the queue.
// If control_mode==4
#define UPDATE_VS(val,len) \
  mako::Node *header = GET_NODE_POINTER(val,len); \
  uint32_t *shardtimestamp = GET_NODE_EXTRA_POINTER(val,len); \
  /* Update single max timestamp in readset */ \
  TThread::txn->maxTimestampReadSet = MAX(TThread::txn->maxTimestampReadSet, header->timestamp); \
  if (BenchmarkConfig::getInstance().getControlMode()==4) { \
    if (*shardtimestamp % 10 < TThread::txn->current_term_ && sync_util::sync_logger::safety_check(header->timestamp)){ \
      TThread::transget_without_stable = true; \
      TThread::transget_without_throw = true; \
    } \
  }
#else
#define UPDATE_VS(val,len) \
  mako::Node *header = GET_NODE_POINTER(val,len); \
  /* Update single max timestamp in readset */ \
  TThread::txn->maxTimestampReadSet = MAX(TThread::txn->maxTimestampReadSet, header->timestamp); \
  if (BenchmarkConfig::getInstance().getControlMode()==1){ \
    if (TThread::txn->maxTimestampReadSet>sync_util::sync_logger::failed_shard_ts){ \
      TThread::transget_without_throw = true;\
    } \
  }
#endif
// It may cause too many aborts and slow down the system if using throw abstract_db::abstract_abort_exception()
// Instead, we use TThread::transget_without_throw = true.

#define STD_OP(f) \
  try { \
    f; \
  } catch (Transaction::Abort E) { \
    throw abstract_db::abstract_abort_exception(); \
  }

#define OP_LOGGING 0
#if OP_LOGGING
std::atomic<long> mt_get(0);
std::atomic<long> mt_put(0);
std::atomic<long> mt_del(0);
std::atomic<long> mt_scan(0);
std::atomic<long> mt_rscan(0);
std::atomic<long> ht_get(0);
std::atomic<long> ht_put(0);
std::atomic<long> ht_insert(0);
std::atomic<long> ht_del(0);
#endif

// ============================================================================
// mbta_ordered_index — the Silo/STO backend as a rusty-cpp inline-Rust
// DSL struct (docs/storage-interface.md). The #if RUSTYCPP_RUST block
// is the source of truth; regenerate with scripts/regen_storage_dsl.sh.
//
// The empty #[cpp_inherit] impl attaches the FullOrderedIndex base
// (TxnOrderedIndex + ShardParticipant); the inherent impl's methods
// override the inherited virtuals by signature.
//
// C++ stays where C++ must: the per-verb kernels below own the
// exception boundary (Sto ops throw Transaction::Abort — STD_OP
// translates it to abstract_db::abstract_abort_exception for the
// txn'd/2PC families; the non-txn family catches and retries in
// place), the UPDATE_VS read-set bookkeeping macro, and the RPC retry
// loops. The DSL owns the class shape, the interface attachment, and
// the remote/local dispatch.
//
// The index holds its MassTrans behind a raw pointer: MassTrans is
// non-movable and DSL structs are move-only with a synthesized
// fieldwise+move ctor. The allocation is process-lifetime, matching
// the historical table lifetime (tables are never torn down mid-run;
// close_index has no callers).
//
// (mbta_wrapper_norm.hh / mbta_wrapper_arena.hh are unused legacy
// copies of this file; they are not included anywhere.)
// ============================================================================

// MassTrans table type at namespace scope, so the kernels and external
// thread-bring-up call sites can name it (was the class-scoped
// mbta_ordered_index::mbta_type).
#if STO_OPACITY
typedef MassTrans<std::string, versioned_str_struct, true/*opacity*/> mbta_table;
#else
typedef MassTrans<std::string, versioned_str_struct, false/*opacity*/> mbta_table;
#endif

// Spelling for put_mbta's comparator (fn-pointer params need a
// single-ident alias in the DSL).
using oi_cmp_fn = bool (*)(const std::string &, const std::string &);

// ---------------------------------------------------------------------------
// C++ kernels for the DSL bodies.
// ---------------------------------------------------------------------------

// @unsafe - allocates the (non-movable) MassTrans the index owns
inline mbta_table *oi_mbta_make(const std::string &name, long table_id,
                                bool is_remote) {
  auto *t = new mbta_table();
  t->set_table_id(table_id);
  t->set_is_remote(is_remote);
  t->set_table_name(name);
  return t;
}

// @safe - identity reads/writes forwarded to MassTrans
inline bool oi_mbta_is_remote(mbta_table *t) { return t->get_is_remote(); }
inline int oi_mbta_table_id(mbta_table *t) { return t->get_table_id(); }
inline void oi_mbta_set_is_remote(mbta_table *t, bool s) {
  t->set_is_remote(s);
}
inline void oi_mbta_set_table_name(mbta_table *t, const std::string &n) {
  t->set_table_name(n);
}
inline size_t oi_mbta_size(const mbta_table *t) { return t->approx_size(); }

// ---- transactional verbs (caller-managed txn) -----------------------------

// @unsafe - Sto txn read; pokes TThread read-set metadata (UPDATE_VS)
inline bool oi_mbta_tx_get_local(mbta_table *t, lcdf::Str key,
                                 std::string &value) {
  STD_OP({
    bool ret = t->transGet(key, value);
    // Check for silent abort (transGet uses abort_without_throw for
    // certain failures). Throw to match RPC path behavior and allow
    // caller to handle properly.
    if (TThread::transget_without_throw) {
      TThread::transget_without_throw = false;
      throw Transaction::Abort();
    }
    if (ret) {
      UPDATE_VS(value.data(), value.length())
      if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
        value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
    }
    return ret;
  });
}

// @unsafe - remote txn read RPC; failures become abstract_abort
inline bool oi_mbta_tx_get_remote(mbta_table *t, lcdf::Str key,
                                  std::string &value) {
  int ret = TThread::sclient->remoteGet(t->get_table_id(), key, value);
  if (ret > 0) {
    throw abstract_db::abstract_abort_exception();
  }
  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
    UPDATE_VS(value.data(), value.length())
    value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
  }
  return true;
}

// @unsafe - Sto txn write (stores a pointer into the caller's buffer)
inline void oi_mbta_tx_put(mbta_table *t, lcdf::Str key,
                           const std::string &value) {
#if OP_LOGGING
  mt_put++;
#endif
  STD_OP({ t->transPut(key, StringWrapper(value)); });
}

// @unsafe - Sto txn insert
inline void oi_mbta_tx_insert(mbta_table *t, lcdf::Str key,
                              const std::string &value) {
  STD_OP(t->transInsert(key, StringWrapper(value));)
}

// @unsafe - Sto txn delete
inline void oi_mbta_tx_remove(mbta_table *t, lcdf::Str key) {
#if OP_LOGGING
  mt_del++;
#endif
  STD_OP(t->transDelete(key));
}

// @unsafe - Sto txn range read; strips EXTRA_BITS from delivered values
inline void oi_mbta_tx_scan(mbta_table *t, const std::string &start_key,
                            const std::string *end_key,
                            oi_scan_callback &callback, str_arena *arena) {
#if OP_LOGGING
  mt_scan++;
#endif
  mbta_table::Str end = end_key ? mbta_table::Str(*end_key) : mbta_table::Str();
  mbta_table::ValueAllocator value_allocator(
      [arena]() -> mbta_table::value_type* { return (*arena)(); });
  mbta_table::ValueAllocator *value_allocator_ptr =
      arena ? &value_allocator : nullptr;
  STD_OP(t->transQuery(start_key, end,
                       [&](mbta_table::Str key, std::string &value) {
    if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
      value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
    return callback.invoke(key.data(), key.length(), value);
  }, value_allocator_ptr));
}

// @unsafe - Sto txn reverse range read
inline void oi_mbta_tx_rscan(mbta_table *t, const std::string &start_key,
                             const std::string *end_key,
                             oi_scan_callback &callback, str_arena *arena) {
#if OP_LOGGING
  mt_rscan++;
#endif
  mbta_table::Str end = end_key ? mbta_table::Str(*end_key) : mbta_table::Str();
  mbta_table::ValueAllocator value_allocator(
      [arena]() -> mbta_table::value_type* { return (*arena)(); });
  mbta_table::ValueAllocator *value_allocator_ptr =
      arena ? &value_allocator : nullptr;
  STD_OP(t->transRQuery(start_key, end,
                        [&](mbta_table::Str key, std::string &value) {
    if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
      value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
    return callback.invoke(key.data(), key.length(), value);
  }, value_allocator_ptr));
}

// @unsafe - local single-match range read on the caller's txn
inline void oi_mbta_tx_scan_one_local(mbta_table *t,
                                      const std::string &start_key,
                                      const std::string &end_key,
                                      std::string &value) {
  bool found = false;
  STD_OP(t->transQuery(start_key, mbta_table::Str(end_key),
                       [&](mbta_table::Str key, std::string &v) {
    if (!found) {
      value = v;
      if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
        UPDATE_VS(value.data(), value.length())
        value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
      }
      found = true;
    }
    return false;  // Stop after first result
  }));
  // Check for silent abort after transQuery (may have used
  // abort_without_throw). Throw to match RPC path behavior.
  if (TThread::transget_without_throw) {
    TThread::transget_without_throw = false;
    throw abstract_db::abstract_abort_exception();
  }
  // Note: If no result found, value remains empty (same as remote scan
  // behavior)
}

// @unsafe - remote single-match scan RPC
inline void oi_mbta_tx_scan_one_remote(mbta_table *t,
                                       const std::string &start_key,
                                       const std::string &end_key,
                                       std::string &value) {
  int ret =
      TThread::sclient->remoteScan(t->get_table_id(), start_key, end_key, value);
  if (ret > 0) {
    throw abstract_db::abstract_abort_exception();
  }
  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
    UPDATE_VS(value.data(), value.length())
    value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
  }
}

// @unsafe - mbta-specific compare-and-put (replay path; put_mbta)
inline const char *oi_mbta_put_cmp(mbta_table *t, lcdf::Str key,
                                   oi_cmp_fn compar,
                                   const std::string &value) {
  STD_OP({
    t->transPutMbta(key, StringWrapper(value), compar);
    return 0;
  });
}

// ---- 2PC participant verbs (ambient Sto txn) ------------------------------

// @unsafe - stages a read into the serving thread's ambient Sto txn
inline bool oi_mbta_shard_get(mbta_table *t, lcdf::Str key,
                              std::string &value) {
  STD_OP({
    bool ret = t->transGet(key, value);
    return ret;
  });
}

// @unsafe - ambient-txn write + write-set lock
inline const char *oi_mbta_shard_put(mbta_table *t, lcdf::Str key,
                                     const std::string &value) {
  STD_OP({
    t->transPut(key, StringWrapper(value));
    if (!Sto::shard_try_lock_last_writeset()) {
      throw Transaction::Abort();
    }
    return 0;
  });
}

// @unsafe - ambient-txn range read (raw stored bytes, no strip)
inline bool oi_mbta_shard_scan(mbta_table *t, const std::string &start_key,
                               const std::string *end_key,
                               oi_scan_callback &callback, str_arena *arena) {
  mbta_table::Str end = end_key ? mbta_table::Str(*end_key) : mbta_table::Str();
  mbta_table::ValueAllocator value_allocator(
      [arena]() -> mbta_table::value_type* { return (*arena)(); });
  mbta_table::ValueAllocator *value_allocator_ptr =
      arena ? &value_allocator : nullptr;
  STD_OP(t->transQuery(start_key, end,
                       [&](mbta_table::Str key, std::string &value) {
    return callback.invoke(key.data(), key.length(), value);
  }, value_allocator_ptr));
  return true;
}

// ---- non-transactional verbs (Masstree-shape) -----------------------------
//
// Each local op delegates to MassTrans's one-op-txn variant and
// retries on OCC abort, so callers get Masstree-parity "no spurious
// failure" semantics. remove is MassTrans's direct raw write (the
// documented asymmetry). Values follow the raw-bytes convention:
// writes are Encoded here, once, at the storage boundary; reads/scans
// strip EXTRA_BITS_FOR_VALUE.
//
// Scan/rscan deliver callbacks live during the attempt; if the one-op
// txn aborts mid-scan the whole scan retries, so callbacks may observe
// a repeated prefix. This matches the pre-existing
// shard_scan-with-caller-retry behavior, just with the retry moved
// inside.

// Shared retry driver for the remote non-txn write RPCs. Transient
// failures (TIMEOUT: lost/late reply; SERVER_BUSY: the serving worker
// is mid-2PC) are retried, matching the local branch's
// retry-until-success semantics. Hard errors (leader check, unknown
// table) assert loudly.
// @unsafe - blocks on RPC promises
template <typename RpcFn>
inline bool oi_mbta_nontxn_remote_write(RpcFn &&rpc) {
  while (true) {
    bool op_result = false;
    int ret = rpc(&op_result);
    if (ret == mako::ErrorCode::SUCCESS)
      return op_result;
    ALWAYS_ASSERT(ret == mako::ErrorCode::TIMEOUT ||
                  ret == mako::ErrorCode::SERVER_BUSY ||
                  ret == mako::ErrorCode::ABORT);
    usleep(1000);  // brief backoff, then retry
  }
}

// @unsafe - self-contained remote read RPC with retry
inline bool oi_mbta_get_remote(mbta_table *t, lcdf::Str key,
                               std::string &value) {
  // Self-contained read RPC. NOT remoteGet — that one stages a
  // read-set item in the serving worker's participant txn (cleaned up
  // by the txn path's later 2PC abort/commit, which a non-txn caller
  // never sends), leaving the worker permanently "busy" for non-txn
  // writes. ABORT signals key-not-found; TIMEOUT (lost/late reply) is
  // retried.
  std::string k(key.data(), key.length());
  while (true) {
    int ret = TThread::sclient->nontxnGet(t->get_table_id(), k, value);
    if (ret == mako::ErrorCode::SUCCESS) break;
    if (ret == mako::ErrorCode::ABORT) return false;  // not found
    usleep(1000);  // transient — retry
  }
  // No strip here: the server serves nontxnGet through the L3 get,
  // which already removed the EXTRA_BITS suffix (unlike getReqType,
  // whose shard_get returns raw stored bytes).
  return true;
}

// @unsafe - one-op OCC txn with retry around Sto thread-local state
inline bool oi_mbta_get_local(mbta_table *t, lcdf::Str key,
                              std::string &value) {
  while (true) {
    try {
      bool ret = t->get(key, value);
      if (TThread::transget_without_throw) {
        TThread::transget_without_throw = false;
        continue;  // silent abort — retry
      }
      if (ret) {
        UPDATE_VS(value.data(), value.length())
        if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
          value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
      }
      return ret;
    } catch (Transaction::Abort &) { /* conflict — retry */ }
  }
}

// @unsafe - remote non-txn overwrite RPC (raw bytes on the wire; the
// owning shard's local branch encodes)
inline bool oi_mbta_put_remote(mbta_table *t, lcdf::Str key,
                               const std::string &value) {
  std::string k(key.data(), key.length());
  return oi_mbta_nontxn_remote_write([&](bool *r) {
    return TThread::sclient->nontxnPut(t->get_table_id(), k, value, r);
  });
}

// @unsafe - one-op OCC overwrite with retry
inline bool oi_mbta_put_local(mbta_table *t, lcdf::Str key,
                              const std::string &value) {
  // Encoding happens HERE, once, at the storage boundary: non-txn
  // callers pass raw bytes (unlike the txn'd put, which stores a
  // pointer into the caller's buffer until commit and therefore needs
  // the caller to own an Encode()d copy). The one-op txn commits
  // inside mbta.put, so the local's lifetime suffices.
  const std::string enc = mako::Encode(value);
  while (true) {
    try {
      return t->put(key, StringWrapper(enc));
    } catch (Transaction::Abort &) { /* conflict — retry */ }
  }
}

// @unsafe - remote non-txn put-if-absent RPC
inline bool oi_mbta_insert_remote(mbta_table *t, lcdf::Str key,
                                  const std::string &value) {
  std::string k(key.data(), key.length());
  return oi_mbta_nontxn_remote_write([&](bool *r) {
    return TThread::sclient->nontxnInsert(t->get_table_id(), k, value, r);
  });
}

// @unsafe - one-op OCC put-if-absent with retry
inline bool oi_mbta_insert_local(mbta_table *t, lcdf::Str key,
                                 const std::string &value) {
  // Raw-bytes convention: Encode applied here, once (see put above).
  const std::string enc = mako::Encode(value);
  while (true) {
    try {
      return t->insert(key, StringWrapper(enc));
    } catch (Transaction::Abort &) { /* conflict — retry */ }
  }
}

// @unsafe - remote non-txn delete RPC
inline bool oi_mbta_remove_remote(mbta_table *t, lcdf::Str key) {
  std::string k(key.data(), key.length());
  return oi_mbta_nontxn_remote_write([&](bool *r) {
    return TThread::sclient->nontxnRemove(t->get_table_id(), k, r);
  });
}

// @unsafe - direct raw write through the MassTrans cursor
inline bool oi_mbta_remove_local(mbta_table *t, lcdf::Str key) {
  return t->remove(key);
}

// @unsafe - one-op OCC range read with whole-scan retry
inline void oi_mbta_nontxn_scan(mbta_table *t, const std::string &start_key,
                                const std::string *end_key,
                                oi_scan_callback &callback,
                                str_arena *arena) {
  // Remote tables: fail loudly. The only scan RPC (remoteScan /
  // HandleScanRequest) returns a single first-match value, not a
  // stream — full remote scan needs new protocol (plan non-goal; see
  // docs/storage-interface.md). Note the txn'd scan on a remote table
  // silently scans the empty local tree; asserting here is
  // deliberately stricter.
  ALWAYS_ASSERT(!t->get_is_remote());
  mbta_table::Str end = end_key ? mbta_table::Str(*end_key) : mbta_table::Str();
  while (true) {
    try {
      mbta_table::ValueAllocator value_allocator(
          [arena]() -> mbta_table::value_type* { return (*arena)(); });
      mbta_table::ValueAllocator *value_allocator_ptr =
          arena ? &value_allocator : nullptr;
      t->scan(start_key, end, [&](mbta_table::Str key, std::string &value) {
        if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
          value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
        return callback.invoke(key.data(), key.length(), value);
      }, value_allocator_ptr);
      return;
    } catch (Transaction::Abort &) { /* conflict — retry whole scan */ }
  }
}

// @unsafe - one-op OCC reverse range read with whole-scan retry
inline void oi_mbta_nontxn_rscan(mbta_table *t, const std::string &start_key,
                                 const std::string *end_key,
                                 oi_scan_callback &callback,
                                 str_arena *arena) {
  // Remote tables: fail loudly (same rationale as scan above).
  ALWAYS_ASSERT(!t->get_is_remote());
  mbta_table::Str end = end_key ? mbta_table::Str(*end_key) : mbta_table::Str();
  while (true) {
    try {
      mbta_table::ValueAllocator value_allocator(
          [arena]() -> mbta_table::value_type* { return (*arena)(); });
      mbta_table::ValueAllocator *value_allocator_ptr =
          arena ? &value_allocator : nullptr;
      t->rscan(start_key, end, [&](mbta_table::Str key, std::string &value) {
        if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
          value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
        return callback.invoke(key.data(), key.length(), value);
      }, value_allocator_ptr);
      return;
    } catch (Transaction::Abort &) { /* conflict — retry whole scan */ }
  }
}

// @unsafe - throws: clear() is unimplemented on mbta tables
inline oi_stats_map oi_mbta_clear_unsupported() {
  // TODO: unclear if we need to implement; apparently this should
  // clear the tree and possibly return some stats
  throw 2;
}

#if RUSTYCPP_RUST
pub struct mbta_ordered_index {
    mbta: *mut mbta_table,
}

#[cpp_inherit]
impl FullOrderedIndex for mbta_ordered_index {
}

impl mbta_ordered_index {
    // ---- identity ----------------------------------------------------

    fn get_table_id(&mut self) -> i32 {
        unsafe { oi_mbta_table_id(self.mbta) }
    }

    fn get_is_remote(&mut self) -> bool {
        unsafe { oi_mbta_is_remote(self.mbta) }
    }

    fn set_is_remote(&mut self, s: bool) {
        unsafe { oi_mbta_set_is_remote(self.mbta, s) }
    }

    fn set_table_name(&mut self, name: &std::string) {
        unsafe { oi_mbta_set_table_name(self.mbta, name) }
    }

    // ---- transactional ops (TxnOrderedIndex) ------------------------

    fn tx_get(&mut self, txn: *mut c_void, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        let remote = unsafe { oi_mbta_is_remote(self.mbta) };
        if remote {
            return unsafe { oi_mbta_tx_get_remote(self.mbta, key, value) };
        }
        unsafe { oi_mbta_tx_get_local(self.mbta, key, value) }
    }

    fn tx_put(&mut self, txn: *mut c_void, key: lcdf::Str, value: &std::string) {
        unsafe { oi_mbta_tx_put(self.mbta, key, value) }
    }

    fn tx_insert(&mut self, txn: *mut c_void, key: lcdf::Str, value: &std::string) {
        unsafe { oi_mbta_tx_insert(self.mbta, key, value) }
    }

    fn tx_remove(&mut self, txn: *mut c_void, key: lcdf::Str) {
        unsafe { oi_mbta_tx_remove(self.mbta, key) }
    }

    fn tx_scan(&mut self, txn: *mut c_void, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { oi_mbta_tx_scan(self.mbta, start_key, end_key, callback, arena) }
    }

    fn tx_rscan(&mut self, txn: *mut c_void, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { oi_mbta_tx_rscan(self.mbta, start_key, end_key, callback, arena) }
    }

    fn tx_scan_remote_one(&mut self, txn: *mut c_void, start_key: &std::string, end_key: &std::string, value: &mut std::string) {
        let remote = unsafe { oi_mbta_is_remote(self.mbta) };
        if remote {
            unsafe { oi_mbta_tx_scan_one_remote(self.mbta, start_key, end_key, value) };
            return;
        }
        unsafe { oi_mbta_tx_scan_one_local(self.mbta, start_key, end_key, value) }
    }

    // mbta-specific compare-and-put, outside the traits (replay path;
    // see ThreadPool.cc).
    fn put_mbta(&mut self, txn: *mut c_void, key: lcdf::Str, compar: oi_cmp_fn, value: &std::string) -> *const c_char {
        unsafe { oi_mbta_put_cmp(self.mbta, key, compar, value) }
    }

    // ---- 2PC participant ops (ShardParticipant) ---------------------

    fn shard_get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        unsafe { oi_mbta_shard_get(self.mbta, key, value) }
    }

    fn shard_put(&mut self, key: lcdf::Str, value: &std::string) -> *const c_char {
        unsafe { oi_mbta_shard_put(self.mbta, key, value) }
    }

    fn shard_scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) -> bool {
        unsafe { oi_mbta_shard_scan(self.mbta, start_key, end_key, callback, arena) }
    }

    // ---- non-transactional ops (OrderedIndex) ------------------------

    fn get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        let remote = unsafe { oi_mbta_is_remote(self.mbta) };
        if remote {
            return unsafe { oi_mbta_get_remote(self.mbta, key, value) };
        }
        unsafe { oi_mbta_get_local(self.mbta, key, value) }
    }

    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let remote = unsafe { oi_mbta_is_remote(self.mbta) };
        if remote {
            return unsafe { oi_mbta_put_remote(self.mbta, key, value) };
        }
        unsafe { oi_mbta_put_local(self.mbta, key, value) }
    }

    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let remote = unsafe { oi_mbta_is_remote(self.mbta) };
        if remote {
            return unsafe { oi_mbta_insert_remote(self.mbta, key, value) };
        }
        unsafe { oi_mbta_insert_local(self.mbta, key, value) }
    }

    fn remove(&mut self, key: lcdf::Str) -> bool {
        let remote = unsafe { oi_mbta_is_remote(self.mbta) };
        if remote {
            return unsafe { oi_mbta_remove_remote(self.mbta, key) };
        }
        unsafe { oi_mbta_remove_local(self.mbta, key) }
    }

    fn scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { oi_mbta_nontxn_scan(self.mbta, start_key, end_key, callback, arena) }
    }

    fn rscan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { oi_mbta_nontxn_rscan(self.mbta, start_key, end_key, callback, arena) }
    }

    fn size(&self) -> usize {
        unsafe { oi_mbta_size(self.mbta) }
    }

    fn clear(&mut self) -> oi_stats_map {
        unsafe { oi_mbta_clear_unsupported() }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=mbta_wrapper.1 version=1 rust_sha256=10cad92217c38d1a9d7aefbea200d7798798aca41d56256394d9941a3d44aa31*/
struct mbta_ordered_index;

struct mbta_ordered_index : public FullOrderedIndex {
    mbta_table* mbta;
    mbta_ordered_index(mbta_table* mbta_init) : FullOrderedIndex(), mbta(std::move(mbta_init)) {}
    mbta_ordered_index(mbta_ordered_index&& other) noexcept : FullOrderedIndex(), mbta(std::move(other.mbta)) {}


    int32_t get_table_id();
    bool get_is_remote();
    void set_is_remote(bool s);
    void set_table_name(const std::string& name);
    bool tx_get(c_void* txn, lcdf::Str key, std::string& value, size_t max_bytes_read);
    void tx_put(c_void* txn, lcdf::Str key, const std::string& value);
    void tx_insert(c_void* txn, lcdf::Str key, const std::string& value);
    void tx_remove(c_void* txn, lcdf::Str key);
    void tx_scan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void tx_rscan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void tx_scan_remote_one(c_void* txn, const std::string& start_key, const std::string& end_key, std::string& value);
    const c_char* put_mbta(c_void* txn, lcdf::Str key, oi_cmp_fn compar, const std::string& value);
    bool shard_get(lcdf::Str key, std::string& value, size_t max_bytes_read);
    const c_char* shard_put(lcdf::Str key, const std::string& value);
    bool shard_scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    bool get(lcdf::Str key, std::string& value, size_t max_bytes_read);
    bool put(lcdf::Str key, const std::string& value);
    bool insert(lcdf::Str key, const std::string& value);
    bool remove(lcdf::Str key);
    void scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    size_t size() const;
    oi_stats_map clear();
};


inline int32_t mbta_ordered_index::get_table_id() {
    // @unsafe
    {
        return oi_mbta_table_id(this->mbta);
    }
}

inline bool mbta_ordered_index::get_is_remote() {
    // @unsafe
    {
        return oi_mbta_is_remote(this->mbta);
    }
}

inline void mbta_ordered_index::set_is_remote(bool s) {
    // @unsafe
    {
        oi_mbta_set_is_remote(this->mbta, std::move(s));
    }
}

inline void mbta_ordered_index::set_table_name(const std::string& name) {
    // @unsafe
    {
        oi_mbta_set_table_name(this->mbta, name);
    }
}

inline bool mbta_ordered_index::tx_get(c_void* txn, lcdf::Str key, std::string& value, size_t max_bytes_read) {
    const auto remote = oi_mbta_is_remote(this->mbta);
    if (remote) {
        return oi_mbta_tx_get_remote(this->mbta, std::move(key), value);
    }
    // @unsafe
    {
        return oi_mbta_tx_get_local(this->mbta, std::move(key), value);
    }
}

inline void mbta_ordered_index::tx_put(c_void* txn, lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        oi_mbta_tx_put(this->mbta, std::move(key), value);
    }
}

inline void mbta_ordered_index::tx_insert(c_void* txn, lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        oi_mbta_tx_insert(this->mbta, std::move(key), value);
    }
}

inline void mbta_ordered_index::tx_remove(c_void* txn, lcdf::Str key) {
    // @unsafe
    {
        oi_mbta_tx_remove(this->mbta, std::move(key));
    }
}

inline void mbta_ordered_index::tx_scan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        oi_mbta_tx_scan(this->mbta, start_key, end_key, callback, arena);
    }
}

inline void mbta_ordered_index::tx_rscan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        oi_mbta_tx_rscan(this->mbta, start_key, end_key, callback, arena);
    }
}

inline void mbta_ordered_index::tx_scan_remote_one(c_void* txn, const std::string& start_key, const std::string& end_key, std::string& value) {
    const auto remote = oi_mbta_is_remote(this->mbta);
    if (remote) {
        // @unsafe
        {
            oi_mbta_tx_scan_one_remote(this->mbta, start_key, end_key, value);
        }
        return;
    }
    // @unsafe
    {
        oi_mbta_tx_scan_one_local(this->mbta, start_key, end_key, value);
    }
}

inline const c_char* mbta_ordered_index::put_mbta(c_void* txn, lcdf::Str key, oi_cmp_fn compar, const std::string& value) {
    // @unsafe
    {
        return oi_mbta_put_cmp(this->mbta, std::move(key), std::move(compar), value);
    }
}

inline bool mbta_ordered_index::shard_get(lcdf::Str key, std::string& value, size_t max_bytes_read) {
    // @unsafe
    {
        return oi_mbta_shard_get(this->mbta, std::move(key), value);
    }
}

inline const c_char* mbta_ordered_index::shard_put(lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        return oi_mbta_shard_put(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::shard_scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        return oi_mbta_shard_scan(this->mbta, start_key, end_key, callback, arena);
    }
}

inline bool mbta_ordered_index::get(lcdf::Str key, std::string& value, size_t max_bytes_read) {
    const auto remote = oi_mbta_is_remote(this->mbta);
    if (remote) {
        return oi_mbta_get_remote(this->mbta, std::move(key), value);
    }
    // @unsafe
    {
        return oi_mbta_get_local(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::put(lcdf::Str key, const std::string& value) {
    const auto remote = oi_mbta_is_remote(this->mbta);
    if (remote) {
        return oi_mbta_put_remote(this->mbta, std::move(key), value);
    }
    // @unsafe
    {
        return oi_mbta_put_local(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::insert(lcdf::Str key, const std::string& value) {
    const auto remote = oi_mbta_is_remote(this->mbta);
    if (remote) {
        return oi_mbta_insert_remote(this->mbta, std::move(key), value);
    }
    // @unsafe
    {
        return oi_mbta_insert_local(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::remove(lcdf::Str key) {
    const auto remote = oi_mbta_is_remote(this->mbta);
    if (remote) {
        return oi_mbta_remove_remote(this->mbta, std::move(key));
    }
    // @unsafe
    {
        return oi_mbta_remove_local(this->mbta, std::move(key));
    }
}

inline void mbta_ordered_index::scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        oi_mbta_nontxn_scan(this->mbta, start_key, end_key, callback, arena);
    }
}

inline void mbta_ordered_index::rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        oi_mbta_nontxn_rscan(this->mbta, start_key, end_key, callback, arena);
    }
}

inline size_t mbta_ordered_index::size() const {
    // @unsafe
    {
        return oi_mbta_size(this->mbta);
    }
}

inline oi_stats_map mbta_ordered_index::clear() {
    // @unsafe
    {
        return oi_mbta_clear_unsupported();
    }
}
/*RUSTYCPP:GEN-END id=mbta_wrapper.1*/

// Builds the index shell plus its process-lifetime MassTrans (the
// fieldwise ctor is DSL-synthesized). Find-or-create stays
// mbta_wrapper's job.
inline mbta_ordered_index *mbta_index_build(const std::string &name,
                                            long table_id,
                                            bool is_remote = false) {
  return new mbta_ordered_index(oi_mbta_make(name, table_id, is_remote));
}


/*
class ht_ordered_index_string : public abstract_ordered_index {
public:
ht_ordered_index_string(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

std::string *arena(void);

bool tx_get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
#if OP_LOGGING
ht_get++;
#endif
STD_OP({
// TODO: we'll still be faster if we just add support for max_bytes_read
bool ret = ht.transGet(key, value);
// TODO: can we support this directly (max_bytes_read)? would avoid this wasted allocation
return ret;
  });
}

void tx_put(
void* txn,
const lcdf::Str key,
const std::string &value)
{
#if OP_LOGGING
ht_put++;
#endif
// TODO: there's an overload of put that takes non-const std::string and silo seems to use move for those.
// may be worth investigating if we can use that optimization to avoid copying keys
STD_OP({
ht.transPut(key, StringWrapper(value));
          });
  }

  void tx_insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
	ht.transPut(key, StringWrapper(value));
	});
  }

  void tx_remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
	ht.transDelete(key);
    });
  }

  void tx_scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            oi_scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void tx_rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             oi_scan_callback &callback,
             str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("rscan");
  }

  size_t size() const
  {
    return 0;
  }

  // TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
  std::map<std::string, uint64_t>
  clear() {
    throw 2;
  }

  typedef Hashtable<std::string, std::string, false, 999983, simple_str> ht_type;
private:
  friend class mbta_wrapper;
  ht_type ht;

  const std::string name;

  mbta_wrapper *db;

};


class ht_ordered_index_int : public abstract_ordered_index {
public:
  ht_ordered_index_int(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

  std::string *arena(void);

  bool tx_get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
    return false;
  }

  bool tx_get(
      void *txn,
      int32_t key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
#if OP_LOGGING
    ht_get++;
#endif
    STD_OP({
        bool ret = ht.transGet(key, value);
        return ret;
          });

  }


  void tx_put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
  }

  void tx_put(
      void* txn,
      int32_t key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value));
          });
  }

  
  void tx_insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
  }

  void tx_insert(void *txn,
                     int32_t key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value));});
  }


  void tx_remove(void *txn, lcdf::Str key) {
      return;
  }

  void tx_remove(void *txn, int32_t key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        ht.transDelete(key);});
  }     

  void tx_scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            oi_scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void tx_rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             oi_scan_callback &callback,
             str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("rscan");
  }

  size_t size() const
  {
    return 0;
  }

  // TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
  std::map<std::string, uint64_t>
  clear() {
    throw 2;
  }

  void print_stats() {
    printf("Hashtable %s: ", name.data());
    ht.print_stats();
  }

  typedef Hashtable<int32_t, std::string, false, 227497, simple_str> ht_type;
  //typedef std::unordered_map<K, std::string> ht_type;
private:
  friend class mbta_wrapper;
  ht_type ht;

  const std::string name;

  mbta_wrapper *db;

};


class ht_ordered_index_customer_key : public abstract_ordered_index {
public:
  ht_ordered_index_customer_key(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

  std::string *arena(void);

  bool tx_get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
    return false;
  }

  bool tx_get(
      void *txn,
      customer_key key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
#if OP_LOGGING
    ht_get++;
#endif
    STD_OP({
        bool ret = ht.transGet(key, value);
        return ret;
          });

  }


  void tx_put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
  }

  void tx_put(
      void* txn,
      customer_key key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value));
          });
  }

  
  void tx_insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
  }

  void tx_insert(void *txn,
                     customer_key key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value));});
  }


  void tx_remove(void *txn, lcdf::Str key) {
      return;
  }

  void tx_remove(void *txn, customer_key key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        ht.transDelete(key);});
  }     

  void tx_scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            oi_scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void tx_rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             oi_scan_callback &callback,
             str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("rscan");
  }

  size_t size() const
  {
    return 0;
  }

  // TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
  std::map<std::string, uint64_t>
  clear() {
    throw 2;
  }
  
   void print_stats() {
    printf("Hashtable %s: ", name.data());
    ht.print_stats();
  }

  typedef Hashtable<customer_key, std::string, false, 999983, simple_str> ht_type;
  //typedef std::unordered_map<K, std::string> ht_type;
private:
  friend class mbta_wrapper;
  ht_type ht;

  const std::string name;

  mbta_wrapper *db;

};


class ht_ordered_index_history_key : public abstract_ordered_index {
public:
  ht_ordered_index_history_key(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

  std::string *arena(void);

  bool tx_get(
      void *txn,
      lcdf::Str key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
#if OP_LOGGING
    ht_get++;
#endif
    STD_OP({
        assert(key.length() == sizeof(history_key));
        const history_key& k = *(reinterpret_cast<const history_key*>(key.data())); 
        bool ret = ht.transGet(k, value);
        return ret;
          });

  }
  
  void tx_put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        assert(key.length() == sizeof(history_key));
        const history_key& k = *(reinterpret_cast<const history_key*>(key.data()));
        ht.transPut(k, StringWrapper(value));
        return 0;
          });
  }

  void tx_insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        assert(key.length() == sizeof(history_key));
        const history_key& k = *(reinterpret_cast<const history_key*>(key.data()));
        ht.transPut(k, StringWrapper(value)); return 0;});
  }

  void tx_remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        assert(key.length() == sizeof(history_key));
        const history_key& k = *(reinterpret_cast<const history_key*>(key.data()));
        ht.transDelete(k);});
  }     

  void tx_scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            oi_scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void tx_rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             oi_scan_callback &callback,
             str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("rscan");
  }

  size_t size() const
  {
    return 0;
  }

  // TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
  std::map<std::string, uint64_t>
  clear() {
    throw 2;
  }

   void print_stats() {
    printf("Hashtable %s: ", name.data());
    ht.print_stats();
  }

  typedef Hashtable<history_key, std::string, false, 20000003, simple_str> ht_type;
private:
  friend class mbta_wrapper;
  ht_type ht;

  const std::string name;

  mbta_wrapper *db;

};


class ht_ordered_index_oorder_key : public abstract_ordered_index {
public:
  ht_ordered_index_oorder_key(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

  std::string *arena(void);

  bool tx_get(
      void *txn,
      lcdf::Str key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
#if OP_LOGGING
    ht_get++;
#endif
    STD_OP({
        assert(key.length() == sizeof(oorder_key));
        const oorder_key& k = *(reinterpret_cast<const oorder_key*>(key.data())); 
        bool ret = ht.transGet(k, value);
        return ret;
          });

  }
  
  void tx_put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        assert(key.length() == sizeof(oorder_key));
        const oorder_key& k = *(reinterpret_cast<const oorder_key*>(key.data()));
        ht.transPut(k, StringWrapper(value));
        return 0;
          });
  }

  void tx_insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        assert(key.length() == sizeof(oorder_key));
        const oorder_key& k = *(reinterpret_cast<const oorder_key*>(key.data()));
        ht.transPut(k, StringWrapper(value)); return 0;});
  }

  void tx_remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        assert(key.length() == sizeof(oorder_key));
        const oorder_key& k = *(reinterpret_cast<const oorder_key*>(key.data()));
        ht.transDelete(k);});
  }     

  void tx_scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            oi_scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void tx_rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             oi_scan_callback &callback,
             str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("rscan");
  }

  size_t size() const
  {
    return 0;
  }

  // TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
  std::map<std::string, uint64_t>
  clear() {
    throw 2;
  }

   void print_stats() {
    printf("Hashtable %s: ", name.data());
    ht.print_stats();
  }


  typedef Hashtable<oorder_key, std::string, false, 20000003, simple_str> ht_type;
private:
  friend class mbta_wrapper;
  ht_type ht;

  const std::string name;

  mbta_wrapper *db;

};


class ht_ordered_index_stock_key : public abstract_ordered_index {
public:
  ht_ordered_index_stock_key(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

  std::string *arena(void);

  bool tx_get(
      void *txn,
      lcdf::Str key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
#if OP_LOGGING
    ht_get++;
#endif
    STD_OP({
        assert(key.length() == sizeof(stock_key));
        const stock_key& k = *(reinterpret_cast<const stock_key*>(key.data())); 
        bool ret = ht.transGet(k, value);
        return ret;
          });

  }
  
  void tx_put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        assert(key.length() == sizeof(stock_key));
        const stock_key& k = *(reinterpret_cast<const stock_key*>(key.data()));
        ht.transPut(k, StringWrapper(value));
        return 0;
          });
  }

  void tx_insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        assert(key.length() == sizeof(stock_key));
        const stock_key& k = *(reinterpret_cast<const stock_key*>(key.data()));
        ht.transPut(k, StringWrapper(value)); return 0;});
  }

  void tx_remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        assert(key.length() == sizeof(stock_key));
        const stock_key& k = *(reinterpret_cast<const stock_key*>(key.data()));
        ht.transDelete(k);});
  }     

  void tx_scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            oi_scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void tx_rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             oi_scan_callback &callback,
             str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("rscan");
  }

  size_t size() const
  {
    return 0;
  }

  // TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
  std::map<std::string, uint64_t>
  clear() {
    throw 2;
  }

   void print_stats() {
    printf("Hashtable %s: ", name.data());
    ht.print_stats();
  }

  typedef Hashtable<stock_key, std::string, false, 3000017, simple_str> ht_type;
private:
  friend class mbta_wrapper;
  ht_type ht;

  const std::string name;

  mbta_wrapper *db;

};
*/


// Free-fn sugar declared in mbta_sharded_ordered_index.hh: put_mbta
// is mbta-specific and needs the complete mbta type for the cast;
// per-key tables are mbta by construction.
inline const char *mbta_sharded_put_mbta(
    mbta_sharded_ordered_index *t, void *txn, lcdf::Str key,
    bool (*compar)(const std::string &newValue,
                   const std::string &oldValue),
    const std::string &value) {
  return static_cast<mbta_ordered_index *>(
             oi_pick_shard(&t->shard_tables, key))
      ->put_mbta(txn, key, compar, value);
}

class mbta_wrapper : public abstract_db {
public:
  // tables for a database instance; we can pre-allocate many tables; 
  // then do a mapping when user creates one in the code 

  // table-id and index of this array is exactly same
  std::vector<mbta_ordered_index *> global_table_instances ;
  std::unordered_map<int, int> availableTable_id ;
  // Track created tables by (name, shard_index) to avoid duplicates
  std::map<std::tuple<std::string,int>, int> tables_taken;

  mbta_wrapper() { /* Avoid doing something here! */}

  void init() {
    preallocate_open_index() ;

    auto& benchConfig = BenchmarkConfig::getInstance();

    for (int i=0; i<benchConfig.getNshards(); i++) {
      availableTable_id[i] = i * mako::NUM_TABLES_PER_SHARD + 1 ;
    }
  }

  ssize_t txn_max_batch_size() const OVERRIDE { return 100; }
  
  void
  do_txn_epoch_sync() const
  {
    //txn_epoch_sync<Transaction>::sync();
  }

  void
  do_txn_finish() const
  {
#if PERF_LOGGING
    Transaction::print_stats();
    //    printf("v: %lu, k %lu, ref %lu, read %lu\n", version_mallocs, key_mallocs, ref_mallocs, read_mallocs);
   {
        using thd = threadinfo_t;
        thd tc = Transaction::tinfo_combined();
        printf("total_n: %llu, total_r: %llu, total_w: %llu, total_searched: %llu, total_aborts: %llu (%llu aborts at commit time), rdata_size: %llu, wdata_size: %llu\n", tc.p(txp_total_n), tc.p(txp_total_r), tc.p(txp_total_w), tc.p(txp_total_searched), tc.p(txp_total_aborts), tc.p(txp_commit_time_aborts), tc.p(txp_max_rdata_size), tc.p(txp_max_wdata_size));
    }

#endif
#if OP_LOGGING
    printf("mt_get: %ld, mt_put: %ld, mt_del: %ld, mt_scan: %ld, mt_rscan: %ld, ht_get: %ld, ht_put: %ld, ht_insert: %ld, ht_del: %ld\n", mt_get.load(), mt_put.load(), mt_del.load(), mt_scan.load(), mt_rscan.load(), ht_get.load(), ht_put.load(), ht_insert.load(), ht_del.load());
#endif 
    //txn_epoch_sync<Transaction>::finish();
  }

  // for the helper thread, loader == true, source == 1
  void
  thread_init(bool loader, int source)
  {
    static int tidcounter = 0;
    // Per-SHARD worker sequence. A single process can run several
    // shards (dbtest -L 0,1): pid = seq % warehouses is only correct
    // if each shard's workers draw a contiguous block, but concurrent
    // shard-runners interleave on a shared counter — two same-shard
    // workers could get the same pid, derive identical client ports,
    // and EADDRINUSE-panic (shard2SingleProcess CI flake).
    static constexpr size_t kMaxLocalShards = 64;
    static std::atomic<size_t> partition_seq[kMaxLocalShards];
    TThread::set_id(__sync_fetch_and_add(&tidcounter, 1));
    TThread::set_mode(0); // checking in-progress
    TThread::set_num_rpc_server(BenchmarkConfig::getInstance().getNumRpcServer());
    TThread::set_is_micro(BenchmarkConfig::getInstance().getIsMicro());
#if defined(DISABLE_MULTI_VERSION)
    TThread::disable_multiversion();
#else
    if (BenchmarkConfig::getInstance().getIsReplicated()) {
      TThread::enable_multiverison();
    }else{
      TThread::disable_multiversion();
    }
#endif
    TThread::set_shard_index(BenchmarkConfig::getInstance().getShardIndex());
    TThread::set_nshards(BenchmarkConfig::getInstance().getNshards());
    TThread::set_warehouses(BenchmarkConfig::getInstance().getConfig()->warehouses);
    Notice("thread_init: thread_id=%d, shard_index=%d, getShardIndex=%zu, loader=%d",
           TThread::id(), TThread::get_shard_index(), BenchmarkConfig::getInstance().getShardIndex(), loader);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    TThread::the_debug_bit = 0;
    if (BenchmarkConfig::getInstance().getLeaderConfig()){
      TThread::is_worker_leader = true;
    }

    TThread::increment_id = 0;
    TThread::skipBeforeRemoteNewOrder = 0;
    TThread::isHomeWarehouse = true;
    TThread::isRemoteShard = false;
    TThread::skipBeforeRemotePayment = 0;
    if(!loader) {
      size_t shard_slot = BenchmarkConfig::getInstance().getShardIndex() % kMaxLocalShards;
      size_t old = partition_seq[shard_slot].fetch_add(1);
      // Use local partition ID (0 to warehouses-1) within each shard
      // getPartitionID() will compute absolute partition ID using shard_index
      size_t local_pid = old % BenchmarkConfig::getInstance().getConfig()->warehouses;
      TThread::set_pid(local_pid);

      TThread::sclient = new mako::ShardClient(BenchmarkConfig::getInstance().getConfig()->configFile,
                                                 BenchmarkConfig::getInstance().getCluster(),
                                                 BenchmarkConfig::getInstance().getShardIndex(),
                                                 local_pid);

      // Verify remote shards are ready before proceeding (Option 4A)
      // This ensures distributed deployment safety: all shards must be listening
      // before any worker starts executing transactions
      int myShardIndex = BenchmarkConfig::getInstance().getShardIndex();
      int nshards = BenchmarkConfig::getInstance().getNshards();
      for (int i = 0; i < nshards; i++) {
        if (i == myShardIndex) continue;  // Skip self
        int retries = 0;
        const int maxRetries = 30;  // 30 seconds max wait
        while (TThread::sclient->checkRemoteShardReady(i) != mako::ErrorCode::SUCCESS) {
          retries++;
          if (retries >= maxRetries) {
            Warning("Shard %d not ready after %d retries, proceeding anyway", i, maxRetries);
            break;
          }
          usleep(1000000);  // 1 second retry interval
        }
        if (retries < maxRetries && retries > 0) {
          Notice("Shard %d ready after %d retries", i, retries);
        }
      }
      //Notice("ParID[worker-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getGlobalPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    } else {
      TThread::set_pid(TThread::id()%BenchmarkConfig::getInstance().getConfig()->warehouses);
      //Notice("ParID[load-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getGlobalPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    }
    
    if (TThread::id() == 0) {
      // someone has to do this (they don't provide us with a general init callback)
      mbta_table::static_init();
      // need this too
      pthread_t advancer;
      pthread_create(&advancer, NULL, Transaction::epoch_advancer, NULL);
      pthread_detach(advancer);
    }
    mbta_table::thread_init();
  }

  void
  thread_end()
  {

  }

  size_t
  sizeof_txn_object(uint64_t txn_flags) const
  {
    return sizeof(Transaction);
  }

  static __thread str_arena *thr_arena;
  void *new_txn(
                uint64_t txn_flags,
                str_arena &arena,
                void *buf,
                TxnProfileHint hint = HINT_DEFAULT) {
    Sto::start_transaction();
    thr_arena = &arena;
    return NULL;
  }

  bool commit_txn(void *txn) {
    if (!Sto::in_progress()) {
      throw abstract_db::abstract_abort_exception();
    }
    if (!Sto::try_commit()) {
      throw abstract_db::abstract_abort_exception();
    }
    return true;
  }

  bool commit_txn_no_paxos(void *txn) {
    if (!Sto::in_progress()) {
      throw abstract_db::abstract_abort_exception();
    }
    if (!Sto::try_commit_no_paxos()) {
      throw abstract_db::abstract_abort_exception();
    }
    return true;
  }

  void abort_txn(void *txn) {
    Sto::silent_abort();
    if (TThread::writeset_shard_bits>0||TThread::readset_shard_bits>0)
      TThread::sclient->remoteAbort();
  }

  void abort_txn_local(void *txn) {
    Sto::silent_abort();
  }

  void shard_reset() {
    Sto::start_transaction();
  }

  int shard_validate() {
    return Sto::shard_validate();
  }

  void shard_install(uint32_t timestamp) {
    Sto::shard_install(timestamp);
  }

  void shard_serialize_util(uint32_t timestamp)  {
    Sto::shard_serialize_util(timestamp); // it MUST be successful!!!
  }

  void shard_unlock(bool committed) {
    Sto::shard_unlock(committed);
  }

  void shard_abort_txn(void *txn) {
    Sto::silent_abort();
  }

  abstract_ordered_index *
  open_index(const std::string &name,
             size_t value_size_hint,
	           bool mostly_append = false,
             bool use_hashtable = false) {
    // We only actually create tables in preallocate_open_index now!
    std::cout << "deprecated function!" << std::endl;
    std::exit(EXIT_FAILURE);
    return nullptr;
  }


  abstract_ordered_index *
  open_index(const std::string &name, int shard_index) { // This is allocate a new table
    auto& benchConfig = BenchmarkConfig::getInstance();

    if (shard_index == -1) {
      shard_index = benchConfig.getShardIndex() ;
    } 

    if (tables_taken.find(std::make_tuple(name, shard_index)) != tables_taken.end() ) {
      int table_id = tables_taken[std::make_tuple(name, shard_index)];
      auto tbl = get_index_by_table_id(table_id) ;
      std::cout << "existing table is created with name: " << name 
              << ", table-id: " << tbl->get_table_id()
              << ", on shard-server id:" << shard_index << std::endl;
      return tbl ;
    }

    int available_table_id = __sync_fetch_and_add(&availableTable_id[shard_index], 1);

    // table-id is between [shard_index*mako::NUM_TABLES_PER_SHARD+1, shard_index*mako::NUM_TABLES_PER_SHARD+1+mako::NUM_TABLES_PER_SHARD]
    if (!(available_table_id >= shard_index*mako::NUM_TABLES_PER_SHARD+1 
        && available_table_id <= (shard_index*mako::NUM_TABLES_PER_SHARD+mako::NUM_TABLES_PER_SHARD))) {
          std::cout << "We don't have sufficient tables for you, please don't create too many tables more than " 
                    << mako::NUM_TABLES_PER_SHARD << " on each shard."
                    << " Assigned table_id (strange):" << available_table_id
                    << ", expected range is:" << (shard_index*mako::NUM_TABLES_PER_SHARD+1)
                    << "," << (shard_index*mako::NUM_TABLES_PER_SHARD+mako::NUM_TABLES_PER_SHARD) 
                    << ", shard_index: " << BenchmarkConfig::getInstance().getShardIndex()
                    << ", shard_index(args) [strange]:" << shard_index << std::endl;
          
          std::cout << "All existing tables:" << std::endl;
          for (const auto& [key, value] : tables_taken) {
              const auto& [str, num] = key;  // unpack the tuple
              std::cout << "(" << str << ", " << num << ") -> " << value << "\n";
          }
          
          std::exit(EXIT_FAILURE);
        }

    auto tbl = global_table_instances[available_table_id];
    tbl->set_table_name(name) ;
    // Register table in global registry for policy-based shard routing
    mako::get_table_registry().register_table(available_table_id, name);
    // Record this table to prevent duplicate creation for the same (name, shard)
    tables_taken[std::make_tuple(name, shard_index)] = available_table_id;
    std::cout << "new table is created with name: " << name 
              << ", table-id: " << tbl->get_table_id()
              << ", on shard-server id:" << shard_index << std::endl;
    mako::setup_update_table(available_table_id, tbl);
    return tbl;
  }

  mbta_sharded_ordered_index *
  open_sharded_index(const std::string &name) override {
    auto &benchConfig = BenchmarkConfig::getInstance();
    const size_t shard_count = static_cast<size_t>(benchConfig.getNshards());
    return mbta_sharded_build(
        name,
        shard_count,
        [this, &name](size_t shard) {
          return open_index(name, static_cast<int>(shard));
        });
  }

  // replay will use this function, otherwise NO; get table back;
  abstract_ordered_index *
  get_index_by_table_id(unsigned short table_id) {
    return global_table_instances[table_id];
  }

  // Table-id starts from 1
  void preallocate_open_index() {
    auto& benchConfig = BenchmarkConfig::getInstance();
    auto* config = benchConfig.getConfig();
    bool multi_shard_mode = config && config->multi_shard_mode;

    for (int i=0; i<=mako::NUM_TABLES_PER_SHARD * benchConfig.getNshards(); i++) {
      int table_id = i;
      auto tbl = mbta_index_build(std::to_string(table_id), table_id);
      int shard_index = (table_id - 1) / mako::NUM_TABLES_PER_SHARD;
      if (table_id==0) {
        shard_index = 0;  // table id 0 is not used!
      }

      // Determine if this table is local
      bool is_local = false;
      if (multi_shard_mode) {
        // In multi-shard mode, all shards in local_shard_indices are local
        const auto& local_shards = config->local_shard_indices;
        is_local = (std::find(local_shards.begin(), local_shards.end(), shard_index) != local_shards.end());
      } else {
        // Single-shard mode: only current shard is local
        is_local = (shard_index == static_cast<int>(benchConfig.getShardIndex()));
      }

      tbl->set_is_remote(!is_local);
      global_table_instances.push_back(tbl);
    }
  }

 void
 close_index(abstract_ordered_index *idx) {
   delete idx;
 }

};

// inline: this header is included from multiple TUs (apps AND libmako
// members since ThreadPool.cc joined); non-inline definitions here
// only ever linked by accident of single inclusion.
inline __thread str_arena* mbta_wrapper::thr_arena;

#endif
