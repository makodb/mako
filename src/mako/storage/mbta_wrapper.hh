#ifndef _BENCHMARK_MBTA_WRAPPER_H_
#define _BENCHMARK_MBTA_WRAPPER_H_
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include "abstract_db.h"
#include "abstract_ordered_index.h"
#include "sto/Transaction.hh"
#include "sto/thread_registration.hh"
#include "sto/replay_record.h"
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
#include "lib/sharding_admission.h"
#include "cluster/full_scan.h"
#include <exception>
#include "benchmarks/rpc_setup.h"
#include "mbta_sharded_ordered_index.hh"

// We have to do it on the coordinator instead of transaction.cc, because it only has a local copy of the readSet;
#define MAX(a,b) ((a)>(b)?(a):(b))

#if defined(FAIL_NEW_VERSION)
// control_mode==4, If a value is in the old epoch while this transaction is from the new epoch,  if not stable, we put it in the queue.
// If control_mode==4
#define UPDATE_VS(val,len) \
  do { \
    const uint32_t node_timestamp = \
      mako::load_value_node_timestamp((val), (len)); \
    const uint32_t shard_timestamp = \
      mako::load_value_time_term((val), (len)); \
    /* Update single max timestamp in readset */ \
    TThread::txn->maxTimestampReadSet = \
      MAX(TThread::txn->maxTimestampReadSet, node_timestamp); \
    if (BenchmarkConfig::getInstance().getControlMode()==4) { \
      if (shard_timestamp % 10 < TThread::txn->current_term_ && \
          sync_util::sync_logger::safety_check(node_timestamp)){ \
        TThread::transget_without_stable = true; \
        TThread::transget_without_throw = true; \
      } \
    } \
  } while (0)
#else
#define UPDATE_VS(val,len) \
  do { \
    const uint32_t node_timestamp = \
      mako::load_value_node_timestamp((val), (len)); \
    /* Update single max timestamp in readset */ \
    TThread::txn->maxTimestampReadSet = \
      MAX(TThread::txn->maxTimestampReadSet, node_timestamp); \
    if (BenchmarkConfig::getInstance().getControlMode()==1){ \
      if (TThread::txn->maxTimestampReadSet > \
          sync_util::sync_logger::failed_shard_ts.load(std::memory_order_relaxed)){ \
        TThread::transget_without_throw = true;\
      } \
    } \
  } while (0)
#endif
// It may cause too many aborts and slow down the system if using throw abstract_db::abstract_abort_exception()
// Instead, we use TThread::transget_without_throw = true.

#define STD_OP(f) \
  try { \
    f; \
  } catch (Transaction::Abort E) { \
    throw abstract_db::abstract_abort_exception(); \
  }

// MassTrans uses this TLS flag when it has already aborted locally and cannot
// throw through a hot-path read. UPDATE_VS also uses it to reject a value after
// it was copied. Consume the flag at every adapter boundary before exposing the
// value or reporting success.
inline bool oi_mbta_consume_silent_abort() {
  if (!TThread::transget_without_throw)
    return false;

  TThread::transget_without_throw = false;
  if (TThread::txn != nullptr && TThread::txn->has_active_state())
    Sto::abort_without_throw();
  return true;
}

inline void oi_mbta_throw_if_silent_abort() {
  if (oi_mbta_consume_silent_abort())
    throw abstract_db::abstract_abort_exception();
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
// txn'd/2PC families; the non-txn point verbs catch and retry in
// place, the non-txn scans surface a conflict as an abort), the
// UPDATE_VS read-set bookkeeping macro, and the RPC retry loops. The
// DSL owns the class shape, the interface attachment, and the
// remote/local dispatch.
//
// The index holds its MassTrans behind a raw pointer: MassTrans is
// non-movable and DSL structs are move-only with a synthesized
// fieldwise+move ctor. The allocation is process-lifetime, matching
// the historical table lifetime (tables are never torn down mid-run;
// close_index has no callers).
//
// (The unused legacy copies of this file have been deleted; this is
// the only one.)
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

// @unsafe - defined after the concrete index; never recursively calls its verbs.
inline mbta_table* oi_mbta_point_table(mbta_table* source, const mako::ShardingPoint& point);

// @unsafe - already admitted by the resolver; pokes TThread read-set metadata (UPDATE_VS).
inline bool oi_mbta_tx_get_local(mbta_table *t, lcdf::Str key, std::string &value) {
  STD_OP({
    bool ret = t->transGet(key, value);
    oi_mbta_throw_if_silent_abort();
    if (ret) {
      UPDATE_VS(value.data(), value.length());
      oi_mbta_throw_if_silent_abort();
      if (value.length() >= mako::EXTRA_BITS_FOR_VALUE)
        value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
    }
    return ret;
  });
}

// @unsafe - remote txn read RPC; failures become abstract_abort
inline bool oi_mbta_tx_get_remote(mbta_table *t, lcdf::Str key,
                                  std::string &value, const mako::ShardingPoint& point) {
  int ret = TThread::sclient->remoteGet(t->get_table_id(), key, value, point);
  if (ret == mako::ErrorCode::NOT_FOUND) return false;
  if (ret > 0) {
    // remoteGet has already marked this participant in the coordinator's
    // read-set bits. Resolve the local attempt and abort every staged remote
    // participant before the facade observes the failure.
    Sto::abort_without_throw();
    throw abstract_db::abstract_abort_exception();
  }
  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
    UPDATE_VS(value.data(), value.length());
    oi_mbta_throw_if_silent_abort();
    value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
  }
  return true;
}

// @unsafe - select the captured canonical incarnation before touching the engine.
inline bool oi_mbta_tx_get(mbta_table* source, lcdf::Str key, std::string& value) {
  const auto point = mako::sharding_resolve_point(source->get_table_id(), key.data(), key.length());
  auto* table = oi_mbta_point_table(source, point);
  return table->get_is_remote() ? oi_mbta_tx_get_remote(table, key, value, point)
                                : oi_mbta_tx_get_local(table, key, value);
}

// @unsafe - Sto txn write (stores a pointer into the caller's buffer)
inline void oi_mbta_tx_put(mbta_table *t, lcdf::Str key,
                           const std::string &value) {
  const auto point = mako::sharding_resolve_point(t->get_table_id(), key.data(), key.length());
  t = oi_mbta_point_table(t, point);
#if OP_LOGGING
  mt_put++;
#endif
  STD_OP({
    t->transPut(key, StringWrapper(value));
    oi_mbta_throw_if_silent_abort();
  });
}

// @unsafe - Sto txn insert
inline void oi_mbta_tx_insert(mbta_table *t, lcdf::Str key,
                              const std::string &value) {
  const auto point = mako::sharding_resolve_point(t->get_table_id(), key.data(), key.length());
  t = oi_mbta_point_table(t, point);
  STD_OP({
    t->transInsert(key, StringWrapper(value));
    oi_mbta_throw_if_silent_abort();
  });
}

// @unsafe - Sto txn delete
inline void oi_mbta_tx_remove(mbta_table *t, lcdf::Str key) {
  const auto point = mako::sharding_resolve_point(t->get_table_id(), key.data(), key.length());
  t = oi_mbta_point_table(t, point);
#if OP_LOGGING
  mt_del++;
#endif
  STD_OP({
    // Proxy carriers must stage an explicit delete, not insert-then-delete.
    if (t->get_is_remote()) t->transDeleteRemote(key);
    else t->transDelete(key);
    oi_mbta_throw_if_silent_abort();
  });
}

// @unsafe - borrowed legacy string storage crosses the native Rust ABI.
inline MakoShardBytes oi_mbta_scan_bytes(const std::string& value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
// @unsafe - status failures become the existing explicit transactional abort.
inline void oi_mbta_scan_require(uint32_t status) {
  if (status != MAKO_SHARD_OK) throw abstract_db::abstract_abort_exception();
}
// @unsafe - borrows bounded transport storage only for the immediate Rust call.
inline MakoScanIdentity oi_mbta_scan_identity(const mako::ShardingRequest& request) {
  if (request.coordinate_length > sizeof(request.coordinate))
    throw abstract_db::abstract_abort_exception();
  return {request.transaction, request.grant, request.snapshot_version, request.table,
          uint32_t(request.fixed_coordinate),
          {request.coordinate, request.coordinate_length}};
}

// @unsafe - Rust owns opaque cursor/page allocations, C++ owns lexical lifetime.
struct oi_mbta_scan_state {
  MakoFullScan* scan = nullptr;
  // @unsafe - releases only the cursor allocation, never transaction leases.
  ~oi_mbta_scan_state() { mako_full_scan_free(scan); }
  // @safe - constructs an empty lexical owner.
  oi_mbta_scan_state() = default;
  oi_mbta_scan_state(const oi_mbta_scan_state&) = delete;
  oi_mbta_scan_state& operator=(const oi_mbta_scan_state&) = delete;
};
struct oi_mbta_scan_page_state {
  MakoScanPage* page = nullptr;
  // @unsafe - opaque native allocation released exactly once.
  ~oi_mbta_scan_page_state() { mako_scan_page_free(page); }
  // @safe - constructs an empty lexical owner.
  oi_mbta_scan_page_state() = default;
  oi_mbta_scan_page_state(const oi_mbta_scan_page_state&) = delete;
  oi_mbta_scan_page_state& operator=(const oi_mbta_scan_page_state&) = delete;
};

// @unsafe - real engine range primitive, under the transaction's retained lease.
// Rust owns page size, row ordering, cursor checks and EOF encoding.
inline void oi_mbta_full_scan_page(mbta_table* table,
                                  const mako::ShardingRequest& request,
                                  const uint8_t* input, size_t input_length,
                                  uint8_t* output, size_t capacity,
                                  size_t* output_length) {
  if (table->get_is_remote()) throw abstract_db::abstract_abort_exception();
  oi_mbta_scan_page_state state;
  oi_mbta_scan_require(mako_scan_page_new({input, input_length}, &state.page));
  const auto identity = oi_mbta_scan_identity(request);
  oi_mbta_scan_require(mako_scan_page_identity_matches(state.page, &identity));
  const auto bounds = mako_scan_page_bounds(state.page);
  const std::string lo(reinterpret_cast<const char*>(bounds.lo.data), bounds.lo.len);
  const std::string hi(reinterpret_cast<const char*>(bounds.hi.data), bounds.hi.len);
  mako::sharding_require_interval(table->get_table_id(), bounds.lo,
                                  bounds.has_hi != 0, bounds.hi,
                                  {request.grant, request.snapshot_version});
  const auto start = bounds.has_cursor ? bounds.cursor
      : (bounds.reverse ? bounds.hi : bounds.lo);
  const mbta_table::Str begin(reinterpret_cast<const char*>(start.data), start.len);
  const mbta_table::Str end = bounds.reverse ? mbta_table::Str(lo)
      : (bounds.has_hi ? mbta_table::Str(hi) : mbta_table::Str());
  uint32_t status = MAKO_SHARD_OK;
  // @unsafe - synchronous STO callback; no exception may cross the Rust ABI.
  auto append = [&](mbta_table::Str key, std::string& value) {
    uint32_t proceed = 0;
    status = mako_scan_page_add(state.page,
        {reinterpret_cast<const uint8_t*>(key.data()), size_t(key.length())},
        oi_mbta_scan_bytes(value), &proceed);
    return status == MAKO_SHARD_OK && proceed != 0;
  };
  STD_OP({
    if (bounds.reverse && !bounds.has_hi && !bounds.has_cursor) {
      // @unsafe - Masstree rscan requires a concrete key, not an infinity
      // sentinel. Exhaust the forward range in this same OCC transaction so
      // its node observations cover the maximum and concurrent insertions.
      auto observe = [&](mbta_table::Str key, std::string&) {
        status = mako_scan_page_observe_max(state.page,
            {reinterpret_cast<const uint8_t*>(key.data()), size_t(key.length())});
        return status == MAKO_SHARD_OK;
      };
      table->transQuery(mbta_table::Str(lo), mbta_table::Str(), observe);
      oi_mbta_scan_require(status);
      oi_mbta_throw_if_silent_abort();
      MakoShardBytes maximum{};
      uint32_t present = 0;
      oi_mbta_scan_require(mako_scan_page_maximum(state.page, &maximum, &present));
      if (present) {
        const mbta_table::Str top(reinterpret_cast<const char*>(maximum.data),
                                  maximum.len);
        table->transRQuery(top, end, append, nullptr, mbta_table::mythreadinfo,
                          true, true);
      }
    } else if (bounds.reverse) {
      table->transRQuery(begin, end, append, nullptr, mbta_table::mythreadinfo,
                        false, true);
    } else
      table->transQuery(begin, end, append, nullptr, mbta_table::mythreadinfo,
                       !bounds.has_cursor);
  });
  oi_mbta_scan_require(status);
  oi_mbta_throw_if_silent_abort();
  oi_mbta_scan_require(mako_scan_page_finish(state.page, output, capacity,
                                             output_length));
}

struct oi_mbta_scan_delivery {
  oi_scan_callback& callback;
  str_arena* arena;
  bool strip;
  bool stopped = false;
  std::exception_ptr exception;

  void begin_remote(uint32_t) {}
  void local(mbta_table* source, const mako::ShardingRequest& request,
             MakoScanBounds bounds, MakoShardRoute route);

  // @unsafe - shared epoch/read-set bookkeeping and callback exception boundary.
  uint32_t deliver(MakoShardBytes key, std::string& value) {
    try {
      if (strip && value.size() >= mako::EXTRA_BITS_FOR_VALUE) {
        UPDATE_VS(value.data(), value.size());
        // UPDATE_VS can reject a copied value; never expose it to the callback.
        oi_mbta_throw_if_silent_abort();
        value.resize(value.size() - mako::EXTRA_BITS_FOR_VALUE);
      }
      stopped = !callback.invoke(
          reinterpret_cast<const char*>(key.data), key.len, value);
      return stopped ? 0 : 1;
    } catch (...) {
      exception = std::current_exception();
      stopped = true;
      return 0;
    }
  }

  // @unsafe - callback values retain the caller-requested arena lifetime.
  static uint32_t row(void* opaque, MakoShardBytes key, MakoShardBytes bytes) {
    auto& self = *static_cast<oi_mbta_scan_delivery*>(opaque);
    try {
      std::string temporary;
      std::string& value = self.arena ? *(*self.arena)() : temporary;
      value.assign(reinterpret_cast<const char*>(bytes.data), bytes.len);
      return self.deliver(key, value);
    } catch (...) {
      self.exception = std::current_exception();
      self.stopped = true;
      return 0;
    }
  }
};

// @unsafe - legacy engine kernel; Rust owns segment selection and admission.
// Bounds borrow the immutable snapshot/caller until synchronous traversal ends.
// Unbounded reverse traversal discovers a real maximum in this same OCC txn.
inline void oi_mbta_scan_direct(mbta_table* table, MakoShardBytes lo,
                                bool has_hi, MakoShardBytes hi, bool reverse,
                                oi_mbta_scan_delivery& delivery) {
  if (table->get_is_remote())
    throw abstract_db::abstract_abort_exception();
  mbta_table::ValueAllocator value_allocator(
      [&delivery]() -> mbta_table::value_type* { return (*delivery.arena)(); });
  auto* allocator = delivery.arena ? &value_allocator : nullptr;
  auto emit = [&](mbta_table::Str key, std::string& value) {
    return delivery.deliver(
        {reinterpret_cast<const uint8_t*>(key.data()), size_t(key.length())},
        value) != 0;
  };
  const mbta_table::Str lower(reinterpret_cast<const char*>(lo.data), lo.len);
  const mbta_table::Str upper(reinterpret_cast<const char*>(hi.data), hi.len);
  STD_OP({
    if (reverse && !has_hi) {
      // Masstree has no +infinity key. Keep the forward scan's OCC/gap
      // observations, including concurrent insertions above the observed max.
      std::string maximum;
      bool present = false;
      auto observe = [&](mbta_table::Str key, std::string&) {
        maximum.assign(key.data(), key.length());
        present = true;
        return true;
      };
      table->transQuery(lower, mbta_table::Str(), observe);
      oi_mbta_throw_if_silent_abort();
      if (present)
        table->transRQuery(mbta_table::Str(maximum), lower, emit, allocator,
            mbta_table::mythreadinfo, true, true);
    } else if (reverse) {
      // The normalized segment is [lo,hi): exclude hi and include lo.
      table->transRQuery(upper, lower, emit, allocator,
          mbta_table::mythreadinfo, false, true);
    } else {
      table->transQuery(lower, has_hi ? upper : mbta_table::Str(),
          emit, allocator, mbta_table::mythreadinfo, true);
    }
  });
  // Do not translate a callback's Transaction::Abort into an engine exception.
  if (delivery.exception) std::rethrow_exception(delivery.exception);
  oi_mbta_throw_if_silent_abort();
}

// @unsafe - storage binding resolver is defined after the concrete index type.
inline mbta_table* oi_mbta_scan_local_table(mbta_table* source,
                                           const mako::ShardingRequest& request);

inline void oi_mbta_scan_delivery::local(mbta_table* source,
    const mako::ShardingRequest& request, MakoScanBounds bounds, MakoShardRoute route) {
  auto* table = oi_mbta_scan_local_table(source, request);
  mako::sharding_require_interval(table->get_table_id(), bounds.lo, bounds.has_hi, bounds.hi, route);
  oi_mbta_scan_direct(table, bounds.lo, bounds.has_hi, bounds.hi, bounds.reverse, *this);
}

template<typename Index, typename Delivery>
struct oi_sharded_scan_dispatch {
  Index* table;
  const std::string& lo;
  const std::string* hi;
  bool reverse;
  bool fixed;
  Delivery& delivery;
  std::exception_ptr exception;

  // @unsafe - one immutable native snapshot calls this bridge for each segment.
  static uint32_t segment(void* opaque, MakoShardBytes segment_lo,
                          uint32_t has_hi, MakoShardBytes segment_hi,
                          MakoShardRoute route) {
    auto& self = *static_cast<oi_sharded_scan_dispatch*>(opaque);
    try {
      const auto low = self.fixed ? oi_mbta_scan_bytes(self.lo) : segment_lo;
      const auto high = self.fixed
          ? (self.hi ? oi_mbta_scan_bytes(*self.hi) : MakoShardBytes{}) : segment_hi;
      const uint32_t bounded = self.fixed ? self.hi != nullptr : has_hi;
      auto request = mako::sharding_request_with_route(
          self.table->get_table_id(), std::string(), route);
      // Disabled routing still needs the physical destination on the wire.
      request.grant = route.grant;
      request.snapshot_version = route.snapshot_version;
      if (route.grant.owner == uint32_t(TThread::get_shard_index())) {
        self.delivery.local(self.table, request, {low, high, {}, bounded, 0, self.reverse}, route);
        return self.delivery.stopped ? 1 : 0;
      }
      self.delivery.begin_remote(route.grant.owner);
      const auto identity = oi_mbta_scan_identity(request);
      oi_mbta_scan_state state;
      oi_mbta_scan_require(mako_full_scan_new(
          {low, high, {}, bounded, 0, self.reverse}, &identity, &state.scan));
      uint8_t input[mako::full_scan_request_capacity];
      std::string response;
      uint32_t done = 0;
      while (!done) {
        size_t length = 0;
        oi_mbta_scan_require(mako_full_scan_request(state.scan, input,
                                                    sizeof(input), &length));
        if (!TThread::sclient || TThread::sclient->fullScanPage(
            self.table->get_table_id(), request, input, length, response)
                != mako::ErrorCode::SUCCESS)
          throw abstract_db::abstract_abort_exception();
        oi_mbta_scan_require(mako_full_scan_consume(state.scan,
            oi_mbta_scan_bytes(response), Delivery::row,
            &self.delivery, &done));
        if (self.delivery.exception) std::rethrow_exception(self.delivery.exception);
      }
      return self.delivery.stopped ? 1 : 0;
    } catch (...) {
      self.exception = std::current_exception();
      return 2;
    }
  }
};

using oi_mbta_scan_dispatch = oi_sharded_scan_dispatch<mbta_table, oi_mbta_scan_delivery>;

// @unsafe - snapshot traversal borrows no native mutex across RPC/user callbacks.
template<typename Index, typename Delivery>
inline void oi_sharded_full_scan(Index* table, const std::string& start,
                                 const std::string* end, bool reverse, Delivery& delivery) {
  using Dispatch = oi_sharded_scan_dispatch<Index, Delivery>;
  // Engine reverse bounds are (end,start]. The appended NULs normalize bounds
  // only; every page cursor remains the exact last binary key, exclusively.
  std::string lower = reverse ? (end ? *end : std::string()) : start;
  std::string upper = reverse ? start : (end ? *end : std::string());
  if (reverse) {
    if (end) lower.push_back('\0');
    upper.push_back('\0');
  }
  const bool has_upper = reverse || end != nullptr;
  if (has_upper && lower >= upper) return;
  // Genuinely disabled local scans need no native admission. Native-enabled
  // scans below select captured segments and admit each complete local scope.
  // Only remote segments require transport pages.
  if (!mako_sharding_enabled() && !table->get_is_remote()) {
    delivery.local(table, {}, {oi_mbta_scan_bytes(lower), oi_mbta_scan_bytes(upper),
                               {}, has_upper, 0, reverse}, {});
    return;
  }
  Dispatch dispatch{table, lower, has_upper ? &upper : nullptr,
                                reverse, false, delivery};
  uint32_t status = MAKO_SHARD_OK;
  if (mako::sharding_admission_enabled()) {
    const auto* binding = mako::sharding_table_binding(table->get_table_id());
    if (binding->kind == 0) {
      dispatch.fixed = binding->fixed_coordinate.is_some();
      if (dispatch.fixed) {
        const auto& coordinate = binding->fixed_coordinate.as_ref().unwrap();
        std::string coordinate_end = coordinate;
        coordinate_end.push_back('\0');
        status = mako::sharding_scan_segments(binding->table,
            oi_mbta_scan_bytes(coordinate), 1, oi_mbta_scan_bytes(coordinate_end),
            reverse, Dispatch::segment, &dispatch);
      } else {
        status = mako::sharding_scan_segments(binding->table,
            oi_mbta_scan_bytes(lower), has_upper, oi_mbta_scan_bytes(upper),
            reverse, Dispatch::segment, &dispatch);
      }
      if (dispatch.exception) std::rethrow_exception(dispatch.exception);
      oi_mbta_scan_require(status);
      return;
    }
  }
  mako::ShardingRequest request{};
  const int owner = table->get_is_remote()
      ? mako::sharding_scan_request(table->get_table_id(), start, request)
      : TThread::get_shard_index();
  if (owner < 0) throw abstract_db::abstract_abort_exception();
  status = Dispatch::segment(&dispatch, oi_mbta_scan_bytes(lower),
      has_upper, oi_mbta_scan_bytes(upper), {{uint32_t(owner), 0}, MAKO_SHARD_NO_SNAPSHOT});
  if (dispatch.exception) std::rethrow_exception(dispatch.exception);
  if (status == 2) throw abstract_db::abstract_abort_exception();
}

inline void oi_mbta_full_scan(mbta_table* table, const std::string& start,
                              const std::string* end, oi_scan_callback& callback,
                              str_arena* arena, bool reverse, bool strip) {
  oi_mbta_scan_delivery delivery{callback, arena, strip};
  oi_sharded_full_scan(table, start, end, reverse, delivery);
}

// @unsafe - Sto txn range read; strips EXTRA_BITS from delivered values
inline void oi_mbta_tx_scan(mbta_table *t, const std::string &start_key,
                            const std::string *end_key,
                            oi_scan_callback &callback, str_arena *arena) {
#if OP_LOGGING
  mt_scan++;
#endif
  oi_mbta_full_scan(t, start_key, end_key, callback, arena, false, true);
}

// @unsafe - Sto txn reverse range read
inline void oi_mbta_tx_rscan(mbta_table *t, const std::string &start_key,
                             const std::string *end_key,
                             oi_scan_callback &callback, str_arena *arena) {
#if OP_LOGGING
  mt_rscan++;
#endif
  oi_mbta_full_scan(t, start_key, end_key, callback, arena, true, true);
}

// @unsafe - local single-match range read on the caller's txn
inline void oi_mbta_tx_scan_one_local(mbta_table *t,
                                      const std::string &start_key,
                                      const std::string &end_key,
                                      std::string &value) {
  struct First : oi_scan_callback {
    std::string& value;
    // @unsafe - preserves the local first-match API, not the remote median API.
    explicit First(std::string& output) : value(output) {}
    // @unsafe - legacy output string copy, callback stops at the first row.
    bool invoke(const char*, size_t, const std::string& found) override {
      value = found;
      return false;
    }
  } first(value);
  oi_mbta_full_scan(t, start_key, &end_key, first, nullptr, false, true);
}

// @unsafe - legacy remote median scan RPC; intentionally not a full-page scan
inline void oi_mbta_tx_scan_one_remote(mbta_table *t,
                                       const std::string &start_key,
                                       const std::string &end_key,
                                       std::string &value) {
  int ret =
      TThread::sclient->remoteScan(t->get_table_id(), start_key, end_key, value);
  if (ret > 0) {
    // Match remote get failure semantics. Do not leave a live native attempt
    // for Commit to publish after this operation has reported an abort.
    // This includes ErrorCode::NOT_FOUND: an empty range is a successful
    // absence read on the participant (which therefore stays live and is
    // resolved by the remoteAbort below), but this median-row verb has no
    // row to return, so the attempt aborts as it does on both parent lines.
    Sto::abort_without_throw();
    throw abstract_db::abstract_abort_exception();
  }
  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
    UPDATE_VS(value.data(), value.length());
    oi_mbta_throw_if_silent_abort();
    value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
  }
}

// @unsafe - mbta-specific compare-and-put (replay path; put_mbta)
inline const char *oi_mbta_put_cmp(mbta_table *t, lcdf::Str key,
                                   oi_cmp_fn compar,
                                   const std::string &value) {
  STD_OP({
    t->transPutMbta(key, StringWrapper(value), compar);
    oi_mbta_throw_if_silent_abort();
    return 0;
  });
}

// ---- 2PC participant verbs (ambient Sto txn) ------------------------------

// @unsafe - stages a read into the serving thread's ambient Sto txn
inline bool oi_mbta_shard_get(mbta_table *t, lcdf::Str key,
                              std::string &value) {
  mako::sharding_require_point(t->get_table_id(), key.data(), key.length());
  STD_OP({
    bool ret = t->transGet(key, value);
    oi_mbta_throw_if_silent_abort();
    return ret;
  });
}

// @unsafe - ambient-txn write + write-set lock
inline const char *oi_mbta_shard_put(mbta_table *t, lcdf::Str key,
                                     const std::string &value) {
  mako::sharding_require_point(t->get_table_id(), key.data(), key.length());
  STD_OP({
    // RPC scratch strings are overwritten by the next piece; the engine must
    // own this value until install/abort, protected by its retained STO epoch.
    t->transPut(key, value);
    oi_mbta_throw_if_silent_abort();
    if (!Sto::shard_try_lock_last_writeset()) {
      throw Transaction::Abort();
    }
    return 0;
  });
}

// @unsafe - stage a real delete (including an absent-key read), then lock writes.
inline void oi_mbta_shard_remove(mbta_table* t, lcdf::Str key) {
  mako::sharding_require_point(t->get_table_id(), key.data(), key.length());
  STD_OP({
    t->transDelete(key);
    oi_mbta_throw_if_silent_abort();
    if (!Sto::shard_try_lock_last_writeset()) throw Transaction::Abort();
  });
}

// @unsafe - stage a real insert on the owner, then lock it.
// Shared by mbta_wrapper::shard_mutate and the shard receiver's batch-lock
// path (server.cc stage_batch_write), which must stay in step for wire kind 1.
inline void oi_mbta_shard_insert(mbta_table* t, lcdf::Str key,
                                 const std::string& value) {
  mako::sharding_require_point(t->get_table_id(), key.data(), key.length());
  STD_OP({
    // RPC scratch strings are overwritten by the next piece; the engine owns
    // the staged value until install/abort.
    t->transInsert(key, value);
    oi_mbta_throw_if_silent_abort();
    if (!Sto::shard_try_lock_last_writeset()) throw Transaction::Abort();
  });
}

// @unsafe - ambient-txn range read (raw stored bytes, no strip)
inline bool oi_mbta_shard_scan(mbta_table *t, const std::string &start_key,
                               const std::string *end_key,
                               oi_scan_callback &callback, str_arena *arena) {
  mako::sharding_require_scan(t->get_table_id(), start_key, end_key);
  mbta_table::Str end = end_key ? mbta_table::Str(*end_key) : mbta_table::Str();
  mbta_table::ValueAllocator value_allocator(
      [arena]() -> mbta_table::value_type* { return (*arena)(); });
  mbta_table::ValueAllocator *value_allocator_ptr =
      arena ? &value_allocator : nullptr;
  STD_OP(t->transQuery(start_key, end,
                       [&](mbta_table::Str key, std::string &value) {
    return callback.invoke(key.data(), key.length(), value);
  }, value_allocator_ptr));
  oi_mbta_throw_if_silent_abort();
  return true;
}

// ---- non-transactional verbs (Masstree-shape) -----------------------------
//
// Each local point op (get/put/insert/remove) delegates to MassTrans's
// one-op-txn variant and retries on OCC abort, so callers get
// Masstree-parity "no spurious failure" semantics. Values follow the
// raw-bytes convention: writes are Encoded here, once, at the storage
// boundary; reads/scans strip EXTRA_BITS_FOR_VALUE.
//
// Scan/rscan are one logical OCC scan (oi_mbta_nontxn_scan/rscan):
// callbacks are delivered live and are never replayed. An OCC conflict,
// a failed remote page, or a rejected value resolves the attempt
// (Sto::abort_without_throw, which also sends the terminal to every
// touched participant) and surfaces as abstract_db::abstract_abort_exception;
// a caller that needs "no spurious failure" retries the whole scan itself
// and must tolerate the prefix its callback already saw.

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
    if (mako::sharding_admission_enabled() &&
        ret != mako::ErrorCode::TIMEOUT && ret != mako::ErrorCode::SERVER_BUSY)
      throw abstract_db::abstract_abort_exception();
    ALWAYS_ASSERT(ret == mako::ErrorCode::TIMEOUT ||
                  ret == mako::ErrorCode::SERVER_BUSY ||
                  ret == mako::ErrorCode::ABORT);
    usleep(1000);  // brief backoff, then retry
  }
}

// @unsafe - self-contained remote read RPC with retry
inline bool oi_mbta_get_remote(mbta_table *t, lcdf::Str key,
                               std::string &value, const mako::ShardingPoint& point) {
  // Self-contained read RPC. NOT remoteGet — that one stages a
  // read-set item in the serving worker's participant txn (cleaned up
  // by the txn path's later 2PC abort/commit, which a non-txn caller
  // never sends), leaving the worker permanently "busy" for non-txn
  // writes. ABORT signals key-not-found; TIMEOUT (lost/late reply) is
  // retried.
  std::string k(key.data(), key.length());
  while (true) {
    int ret = TThread::sclient->nontxnGet(t->get_table_id(), k, value, point);
    if (ret == mako::ErrorCode::SUCCESS) break;
    if (ret == mako::ErrorCode::ABORT) return false;  // not found
    if (mako::sharding_admission_enabled() &&
        ret != mako::ErrorCode::TIMEOUT && ret != mako::ErrorCode::SERVER_BUSY)
      throw abstract_db::abstract_abort_exception();
    usleep(1000);  // transient — retry
  }
  // No strip here: the server serves nontxnGet through the L3 get,
  // which already removed the EXTRA_BITS suffix (unlike getReqType,
  // whose shard_get returns raw stored bytes).
  return true;
}

// @unsafe - already admitted by the resolver; one-op OCC txn with retry.
inline bool oi_mbta_get_local(mbta_table *t, lcdf::Str key, std::string &value) {
  while (true) {
    try {
      bool ret = t->get(key, value);
      if (oi_mbta_consume_silent_abort())
        continue;  // silent abort — retry
      if (ret) {
        UPDATE_VS(value.data(), value.length());
        if (oi_mbta_consume_silent_abort())
          continue;  // UPDATE_VS rejected the copied value
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
                               const std::string &value, const mako::ShardingPoint& point) {
  std::string k(key.data(), key.length());
  return oi_mbta_nontxn_remote_write([&](bool *r) {
    return TThread::sclient->nontxnPut(t->get_table_id(), k, value, r, point);
  });
}

// @unsafe - already admitted by the resolver; one-op OCC overwrite with retry.
inline bool oi_mbta_put_local(mbta_table *t, lcdf::Str key, const std::string &value) {
  // Encoding happens HERE, once, at the storage boundary: non-txn
  // callers pass raw bytes (unlike the txn'd put, which stores a
  // pointer into the caller's buffer until commit and therefore needs
  // the caller to own an Encode()d copy). The one-op txn commits
  // inside mbta.put, so the local's lifetime suffices.
  const std::string enc = mako::Encode(value);
  while (true) {
    try {
      bool inserted = t->put(key, StringWrapper(enc));
      if (oi_mbta_consume_silent_abort())
        continue;
      return inserted;
    } catch (Transaction::Abort &) { /* conflict — retry */ }
  }
}

// @unsafe - remote non-txn put-if-absent RPC
inline bool oi_mbta_insert_remote(mbta_table *t, lcdf::Str key,
                                  const std::string &value, const mako::ShardingPoint& point) {
  std::string k(key.data(), key.length());
  return oi_mbta_nontxn_remote_write([&](bool *r) {
    return TThread::sclient->nontxnInsert(t->get_table_id(), k, value, r, point);
  });
}

// @unsafe - already admitted by the resolver; one-op OCC put-if-absent with retry.
inline bool oi_mbta_insert_local(mbta_table *t, lcdf::Str key, const std::string &value) {
  // Raw-bytes convention: Encode applied here, once (see put above).
  const std::string enc = mako::Encode(value);
  while (true) {
    try {
      bool inserted = t->insert(key, StringWrapper(enc));
      if (oi_mbta_consume_silent_abort())
        continue;
      return inserted;
    } catch (Transaction::Abort &) { /* conflict — retry */ }
  }
}

// @unsafe - remote non-txn delete RPC
inline bool oi_mbta_remove_remote(mbta_table *t, lcdf::Str key, const mako::ShardingPoint& point) {
  std::string k(key.data(), key.length());
  return oi_mbta_nontxn_remote_write([&](bool *r) {
    return TThread::sclient->nontxnRemove(t->get_table_id(), k, r, point);
  });
}

// @unsafe - already admitted by the resolver; one-op OCC delete with retry.
inline bool oi_mbta_remove_local(mbta_table *t, lcdf::Str key) {
  while (true) {
    try {
      bool removed = t->erase(key);
      if (oi_mbta_consume_silent_abort())
        continue;
      return removed;
    } catch (Transaction::Abort &) { /* conflict — retry */ }
  }
}

// @unsafe - pins one captured route across dispatch and all one-op OCC/RPC retries.
inline bool oi_mbta_get(mbta_table* source, lcdf::Str key, std::string& value) {
  mako::ShardingOperation operation;
  const auto point = mako::sharding_resolve_point(source->get_table_id(), key.data(), key.length());
  auto* table = oi_mbta_point_table(source, point);
  return table->get_is_remote() ? oi_mbta_get_remote(table, key, value, point)
                                : oi_mbta_get_local(table, key, value);
}
// @unsafe - raw overwrite on the canonical physical owner or origin proxy.
inline bool oi_mbta_put(mbta_table* source, lcdf::Str key, const std::string& value) {
  mako::ShardingOperation operation;
  const auto point = mako::sharding_resolve_point(source->get_table_id(), key.data(), key.length());
  auto* table = oi_mbta_point_table(source, point);
  return table->get_is_remote() ? oi_mbta_put_remote(table, key, value, point)
                                : oi_mbta_put_local(table, key, value);
}
// @unsafe - raw insert retains its actual boolean result across reply retries.
inline bool oi_mbta_insert(mbta_table* source, lcdf::Str key, const std::string& value) {
  mako::ShardingOperation operation;
  const auto point = mako::sharding_resolve_point(source->get_table_id(), key.data(), key.length());
  auto* table = oi_mbta_point_table(source, point);
  return table->get_is_remote() ? oi_mbta_insert_remote(table, key, value, point)
                                : oi_mbta_insert_local(table, key, value);
}
// @unsafe - direct raw delete remains under the operation's native lease.
inline bool oi_mbta_remove(mbta_table* source, lcdf::Str key) {
  mako::ShardingOperation operation;
  const auto point = mako::sharding_resolve_point(source->get_table_id(), key.data(), key.length());
  auto* table = oi_mbta_point_table(source, point);
  return table->get_is_remote() ? oi_mbta_remove_remote(table, key, point)
                                : oi_mbta_remove_local(table, key);
}

// @unsafe - one logical OCC scan; never replay externally visible callbacks.
inline void oi_mbta_nontxn_scan(mbta_table *t, const std::string &start_key,
                                const std::string *end_key,
                                oi_scan_callback &callback,
                                str_arena *arena) {
  mako::ShardingOperation operation;
  try {
    Sto::start_transaction();
    oi_mbta_full_scan(t, start_key, end_key, callback, arena, false, true);
    Sto::commit();
  } catch (Transaction::Abort&) {
    Sto::abort_without_throw();
    throw abstract_db::abstract_abort_exception();
  } catch (...) {
    Sto::abort_without_throw();
    throw;
  }
}

// @unsafe - descending one-op OCC scan with terminal cleanup on every error.
inline void oi_mbta_nontxn_rscan(mbta_table *t, const std::string &start_key,
                                 const std::string *end_key,
                                 oi_scan_callback &callback,
                                 str_arena *arena) {
  mako::ShardingOperation operation;
  try {
    Sto::start_transaction();
    oi_mbta_full_scan(t, start_key, end_key, callback, arena, true, true);
    Sto::commit();
  } catch (Transaction::Abort&) {
    Sto::abort_without_throw();
    throw abstract_db::abstract_abort_exception();
  } catch (...) {
    Sto::abort_without_throw();
    throw;
  }
}

// @unsafe - throws: clear() is unimplemented on mbta tables
inline oi_stats_map oi_mbta_clear_unsupported() {
  // TODO: unclear if we need to implement; apparently this should
  // clear the tree and possibly return some stats
  throw oi_clear_unsupported{};
}

#if RUSTYCPP_RUST
pub struct mbta_ordered_index {
    mbta: *mut mbta_table,
}

#[cfg_attr(any(), cpp_inherit)]
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
        unsafe { oi_mbta_tx_get(self.mbta, key, value) }
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
        unsafe { oi_mbta_get(self.mbta, key, value) }
    }

    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        unsafe { oi_mbta_put(self.mbta, key, value) }
    }

    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        unsafe { oi_mbta_insert(self.mbta, key, value) }
    }

    fn remove(&mut self, key: lcdf::Str) -> bool {
        unsafe { oi_mbta_remove(self.mbta, key) }
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
/*RUSTYCPP:GEN-BEGIN id=mbta_wrapper.1 version=1 rust_sha256=2b0db26e45e4c0129ce703dd3655ba5acba324e320ec9f7bee4496ea91b07d3f*/
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
    // @unsafe
    {
        return oi_mbta_tx_get(this->mbta, std::move(key), value);
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
    // @unsafe
    {
        return oi_mbta_get(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::put(lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        return oi_mbta_put(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::insert(lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        return oi_mbta_insert(this->mbta, std::move(key), value);
    }
}

inline bool mbta_ordered_index::remove(lcdf::Str key) {
    // @unsafe
    {
        return oi_mbta_remove(this->mbta, std::move(key));
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

// @unsafe - borrowed engine handle checked once; never mutate shared proxy flags.
inline mbta_table* oi_mbta_point_table(mbta_table* source, const mako::ShardingPoint& point) {
  auto* handle = point.handle();
  if (!handle) return source; // explicit static/immutable/disabled path
  auto* index = dynamic_cast<mbta_ordered_index*>(handle);
  if (!index) throw abstract_db::abstract_abort_exception();
  return index->mbta;
}

// @unsafe - resolve the actual owner-specific physical table, not proxy bytes.
inline mbta_table* oi_mbta_scan_local_table(
    mbta_table* source, const mako::ShardingRequest& request) {
  if (!mako::sharding_admission_enabled()) {
    if (source->get_is_remote()) throw abstract_db::abstract_abort_exception();
    return source;
  }
  auto* physical = mako::sharding_physical_handle(request);
  auto* index = dynamic_cast<mbta_ordered_index*>(physical);
  if (index && !index->mbta->get_is_remote()) return index->mbta;
  throw abstract_db::abstract_abort_exception();
}

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
  inline static thread_local bool tls_benchmark_thread_attached_ = false;

  // @unsafe - Apply a source record through this backend's concrete catalog.
  // Only a definite predecision conflict requests retry. Other failures escape.
  bool replay_apply_record(uint16_t table_id, std::string_view key,
                           std::string_view payload, bool is_delete,
                           uint32_t source_time_term) override {
    observe_replay_timestamp(source_time_term);
    if (table_id >= global_table_instances.size() ||
        global_table_instances[table_id] == nullptr) {
      throw std::logic_error("unregistered native replay table");
    }
    static thread_local str_arena replay_arena;
    static thread_local std::string replay_value;
    mako::materialize_replay_value(
        replay_value, payload, is_delete, source_time_term);
    struct quiesce_guard {
      // @unsafe - The replay transaction has ended before its RCU slot retires.
      ~quiesce_guard() {
        if (TThread::txn == nullptr || !TThread::txn->has_active_state())
          Transaction::rcu_quiesce();
      }
    } quiesce;
    void* txn = new_txn(0, replay_arena, nullptr, HINT_DEFAULT);
    try {
      global_table_instances[table_id]->put_mbta(
          txn, lcdf::Str(key.data(), key.size()),
          [](const std::string& incoming, const std::string& existing) {
            return mako::replay_source_is_newer(
                mako::load_value_time_term(incoming.data(), incoming.size()),
                mako::load_value_time_term(existing.data(), existing.size()));
          },
          replay_value);
      return commit_txn_no_paxos(txn);
    } catch (const abstract_abort_exception&) {
      abort_txn_local(txn);
      return false;
    }
  }

  // @unsafe - Attach the ordinary native backend without a coordinator client.
  void replay_thread_init() override {
    thread_init(true, 2);
    // A replay worker attaches like a loader (no coordinator client) but is
    // not in the loading phase: replication-log application has its own
    // native admission lane (mako::ShardingReplicationReplay).
    TThread::in_loading_phase = false;
    TThread::disable_multiversion();
  }
  // @unsafe - A replay partition is not this OS thread's STO/RCU identity.
  void replay_thread_select_partition(int partition) override {
    TThread::set_pid(partition);
    Sto::update_threadid();
  }
  // @unsafe - Floor only the distributed clock, never the OCC or cache HLC.
  void observe_replay_timestamp(uint32_t source_time_term) override {
    if (!mako::valid_replay_time_term(source_time_term))
      throw std::logic_error("invalid replay source timestamp");
    Transaction::observe_legacy_distributed_timestamp(source_time_term / 10);
  }

  // @safe - Inspect only this OS thread's participant state.
  bool shard_participant_active() const noexcept override {
    return TThread::txn != nullptr && TThread::txn->has_active_state();
  }
  // @safe - Inspect only this OS thread's staged frame.
  bool shard_participant_empty() const noexcept override {
    return TThread::txn == nullptr || !TThread::txn->has_staged_items();
  }
  // @unsafe - Uses the legacy process-wide atomic distributed clock.
  bool shard_timestamp(uint32_t& timestamp) override {
    return Transaction::try_allocate_legacy_distributed_timestamp(timestamp);
  }
  // @unsafe - The receiver admits the term before any participant operation.
  void shard_set_term(uint32_t term) override {
    if (term > 9 || TThread::txn == nullptr ||
        !TThread::txn->has_active_state()) {
      throw std::logic_error("invalid native participant term or state");
    }
    TThread::txn->current_term_ = static_cast<uint8_t>(term);
  }

  // Fixed native catalog: NUM_TABLES_PER_SHARD IDs are preallocated per shard.
  // One logical name consumes one ID on every configured shard; IDs are not
  // reclaimed. Opening beyond the budget returns nullptr.

  // table-id and index of this array is exactly same
  std::vector<mbta_ordered_index *> global_table_instances ;
  std::unordered_map<int, int> availableTable_id ;
  // Track created tables by (name, shard_index) to avoid duplicates
  std::map<std::tuple<std::string,int>, int> tables_taken;
  std::mutex table_catalog_mutex_;

  mbta_wrapper() { /* Avoid doing something here! */}

  void init() {
    // Initialize the process-wide MassTrans callback before the epoch worker
    // can observe it. Multiple database instances share the same Transaction
    // epoch domain, so both operations must be idempotent.
    static std::once_flag mass_trans_init_once;
    std::call_once(mass_trans_init_once, [] {
      mbta_table::static_init();
    });
    Transaction::start_epoch_advancer();

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

  // Initialize the application-level TThread fields shared by the native and
  // Rust TPC-C backends. This deliberately does not initialize MassTrans or
  // start its epoch advancer, so a backend that does not use native STO can
  // reuse the benchmark context without acquiring process-lifetime Masstree
  // state.
  static void
  benchmark_thread_init(bool loader)
  {
    if (tls_benchmark_thread_attached_) {
      throw std::logic_error("database thread was initialized twice");
    }
    // Reject a partially owned raw STO context before changing this thread's
    // stable ID, mode, topology, or shard-client state. A caller must resolve
    // the transaction at the boundary where it was created.
    if (TThread::txn != nullptr && TThread::txn->has_active_state()) {
      throw std::logic_error(
          "database thread initialization found an active transaction");
    }
    if (TThread::sclient != nullptr) {
      throw std::logic_error(
          "database thread initialization found a live shard client");
    }
    tls_benchmark_thread_attached_ = true;
    struct attachment_reservation {
      bool committed = false;
      ~attachment_reservation() {
        if (!committed)
          mbta_wrapper::tls_benchmark_thread_attached_ = false;
      }
    } reservation;

    // Per-SHARD worker sequence. A single process can run several
    // shards (dbtest -L 0,1): pid = seq % warehouses is only correct
    // if each shard's workers draw a contiguous block, but concurrent
    // shard-runners interleave on a shared counter — two same-shard
    // workers could get the same pid, derive identical client ports,
    // and EADDRINUSE-panic (shard2SingleProcess CI flake).
    static constexpr size_t kMaxLocalShards = 64;
    static constexpr size_t kMaxProtocolShards = 31;
    static std::atomic<size_t> partition_seq[kMaxLocalShards];
    auto& benchmark_config = BenchmarkConfig::getInstance();
    transport::Configuration* const transport_config =
        benchmark_config.getConfig();
    const size_t shard_index = benchmark_config.getShardIndex();
    const size_t shard_count = benchmark_config.getNshards();
    // Shard participation still flows through unsigned-int masks and signed
    // `1 << shard` expressions in ShardClient. Until those are widened, only
    // indices 0..30 are representable without overflow or signed-shift UB.
    ALWAYS_ASSERT(shard_count > 0 && shard_count <= kMaxProtocolShards);
    ALWAYS_ASSERT(shard_index < shard_count &&
                  shard_index < kMaxLocalShards);

    size_t warehouse_count = benchmark_config.getNthreads();
    if (transport_config != nullptr) {
      // Configuration::warehouses is signed. Check it before conversion so a
      // negative YAML value cannot become a huge size_t and corrupt pid/port
      // assignment.
      ALWAYS_ASSERT(transport_config->warehouses > 0);
      ALWAYS_ASSERT(transport_config->nshards > 0 &&
                    transport_config->nshards <=
                        static_cast<int>(kMaxProtocolShards));
      ALWAYS_ASSERT(shard_index <
                    static_cast<size_t>(transport_config->nshards));
      warehouse_count =
          static_cast<size_t>(transport_config->warehouses);
    }
    ALWAYS_ASSERT(warehouse_count > 0);

    // A detached OS thread keeps ownership of its RCU slot. Reusing that slot
    // on a later attachment avoids exhausting MAX_THREADS while preserving
    // the no-cross-thread-recycling safety boundary. The shared registration
    // also rejects a worker that already serves the Rust-facing local ABI.
    if (!mako::silo::claim_thread_runtime(
            mako::silo::thread_runtime::native_mako)) {
      Panic("OS thread cannot switch from the local C ABI to native Mako");
    }
    const int thread_id = mako::silo::try_allocate_thread_id();
    if (thread_id < 0) {
      Panic("STO thread-ID space exhausted (%d process-lifetime slots)",
            MAX_THREADS);
    }
    TThread::set_id(thread_id);
    // Sto caches the thread id in its reusable thread-local Transaction.
    // Restore this OS thread's assigned id in case intervening code changed
    // TThread::id(), then refresh the cache before any lock or RCU operation.
    Sto::update_threadid();
    TThread::set_mode(0); // checking in-progress
    TThread::in_loading_phase = loader;
    TThread::set_num_rpc_server(benchmark_config.getNumRpcServer());
    TThread::set_is_micro(benchmark_config.getIsMicro());
#if defined(DISABLE_MULTI_VERSION)
    TThread::disable_multiversion();
#else
    if (benchmark_config.getIsReplicated()) {
      TThread::enable_multiverison();
    }else{
      TThread::disable_multiversion();
    }
#endif
    TThread::set_shard_index(benchmark_config.getShardIndex());
    TThread::set_nshards(benchmark_config.getNshards());
    TThread::set_warehouses(warehouse_count);
    Notice("thread_init: thread_id=%d, shard_index=%d, getShardIndex=%zu, loader=%d",
           TThread::id(), TThread::get_shard_index(),
           benchmark_config.getShardIndex(), loader);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    TThread::the_debug_bit = 0;
    TThread::is_worker_leader = benchmark_config.getLeaderConfig();

    TThread::increment_id = 0;
    TThread::skipBeforeRemoteNewOrder = 0;
    TThread::isHomeWarehouse = true;
    TThread::isRemoteShard = false;
    TThread::skipBeforeRemotePayment = 0;
    if(!loader) {
      const size_t shard_slot = shard_index;
      static thread_local size_t assigned_local_pid[kMaxLocalShards] = {};
      static thread_local size_t assigned_warehouse_count[kMaxLocalShards] = {};
      if (assigned_warehouse_count[shard_slot] == 0) {
        size_t old = partition_seq[shard_slot].fetch_add(1);
        if (old >= warehouse_count) {
          throw std::runtime_error(
              "native STO worker topology exceeded configured warehouses");
        }
        assigned_local_pid[shard_slot] = old;
        assigned_warehouse_count[shard_slot] = warehouse_count;
      } else {
        // A process-wide shard topology is immutable once a thread attaches.
        // Changing it would also invalidate ShardClient port ownership.
        ALWAYS_ASSERT(assigned_warehouse_count[shard_slot] == warehouse_count);
      }
      // Preserve the local partition and ShardClient port across detach and
      // reattach on the same OS thread.
      size_t local_pid = assigned_local_pid[shard_slot];
      TThread::set_pid(local_pid);

      if (transport_config == nullptr) {
        // The RocksDB-compatible local facade permits a standalone,
        // non-replicated one-shard database without a transport YAML file.
        // Such a transaction can never acquire remote shard bits and needs no
        // ShardClient, but it still needs TThread and MassTrans attachment.
        ALWAYS_ASSERT(benchmark_config.getNshards() == 1);
        ALWAYS_ASSERT(!benchmark_config.getIsReplicated());
      } else {
        try {
          TThread::sclient = new mako::ShardClient(
              transport_config->configFile,
              benchmark_config.getCluster(),
              benchmark_config.getShardIndex(),
              local_pid);

          // Verify remote shards are ready before proceeding (Option 4A)
          // This ensures distributed deployment safety: all shards must be listening
          // before any worker starts executing transactions
          int myShardIndex = benchmark_config.getShardIndex();
          int nshards = benchmark_config.getNshards();
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
        } catch (...) {
          delete TThread::sclient;
          TThread::sclient = nullptr;
          throw;
        }
      }
      //Notice("ParID[worker-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getGlobalPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    } else {
      TThread::set_pid(TThread::id() % warehouse_count);
      //Notice("ParID[load-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getGlobalPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    }
    reservation.committed = true;
  }

  static void
  benchmark_thread_end()
  {
    if (!tls_benchmark_thread_attached_) {
      throw std::logic_error("database thread was not initialized");
    }
    // A loader, worker, or helper that exits with its last transaction epoch
    // published would pin active_epoch forever. It has no further STO reads,
    // so remove its slot from the process-wide minimum before returning.
    Transaction::rcu_quiesce();
    if (TThread::sclient != nullptr) {
      mako::ShardClient *client = TThread::sclient;
      TThread::sclient = nullptr;
      delete client;
    }
    tls_benchmark_thread_attached_ = false;
  }

  // for the helper thread, loader == true, source == 1
  void
  thread_init(bool loader, int source)
  {
    (void)source;
    benchmark_thread_init(loader);
    try {
      // Shared with the Rust-facing local ABI. Starting two epoch advancers
      // is both wasteful and a data race on the process-global epoch callback.
      ALWAYS_ASSERT(mako::silo::ensure_epoch_runtime());
      mbta_table::thread_init();
    } catch (...) {
      Transaction::tinfo[TThread::id()].trans_start_callback = {};
      Transaction::tinfo[TThread::id()].trans_end_callback = {};
      benchmark_thread_end();
      throw;
    }
  }

  void
  thread_end()
  {
    // A lexical database-thread guard may unwind while its last transaction
    // is still open. Release its locks and remote state before retiring the
    // STO and Masstree RCU participants or destroying the shard client. This
    // includes an empty mode-1 participant transaction: shard_reset() leaves
    // an idle placeholder that has entered neither the STO epoch nor Masstree
    // RCU (its first storage access does, through ensure_epoch()), so ending
    // it owes no end callback, while one that did touch storage is stopped
    // exactly once here. If INSTALL has already made the participant commit
    // irreversible, silent_abort() instead completes committed unlock/cleanup
    // before ending the context.
    // An attempt that is no longer active sends nothing from here: if it
    // still owes a touched participant a terminal, ~ShardClient quarantines
    // that participant lane instead (benchmark_thread_end below).
    if (TThread::txn != nullptr && TThread::txn->has_active_state()) {
      abort_txn(nullptr);
    }
    // This worker owns every callback in its Masstree limbo queue. Run each
    // one whose grace period is already complete, so an uncontended detach
    // leaves none behind. Never wait for an older active reader: that reader
    // may be blocked on this thread (a scan callback that joins a writer), so
    // a blocking drain here deadlocks. Callbacks such a reader can still reach
    // stay queued on this thread's threadinfo, which no other thread
    // reclaims: they run only if this same OS thread later reattaches to the
    // same MasstreeContext, and otherwise leak (a spawned writer or a
    // per-connection worker that exits never reattaches).
    if (mbta_table::mythreadinfo.ti != nullptr) {
      (void)mbta_table::mythreadinfo.ti->rcu_try_drain();
    }
    benchmark_thread_end();
  }

  size_t
  sizeof_txn_object(uint64_t txn_flags) const
  {
    // Reject retired original-Silo/NDB flags before a transaction is created.
    ALWAYS_ASSERT(txn_flags == 0);
    return sizeof(Transaction);
  }

  static __thread str_arena *thr_arena;
  void *new_txn(
                uint64_t txn_flags,
                str_arena &arena,
                void *buf,
                TxnProfileHint hint = HINT_DEFAULT) {
    // Fail closed if a caller tries to revive an original-Silo/NDB flag.
    // The current STO/MassTrans wrapper does not consume the profile hint.
    ALWAYS_ASSERT(txn_flags == 0);
    (void) hint;
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

  // @unsafe - Shared legacy cleanup closes the complete touched-participant set.
  void abort_txn(void *txn) {
    (void)txn;
    Sto::abort_without_throw();
  }

  void abort_txn_local(void *txn) {
    Sto::silent_abort();
  }

  void shard_reset() {
    // Reset is a transaction boundary. Resolve a partially handled request
    // first: pre-INSTALL work aborts, while an irreversible INSTALL decision
    // completes committed cleanup through Transaction::silent_abort().
    if (TThread::txn != nullptr && TThread::txn->has_active_state()) {
      TThread::txn->silent_abort();
    }
    Sto::start_transaction();
  }

  // @unsafe - Bridges admitted wire operations to the retained legacy STO set.
  void shard_mutate(uint16_t table_id, lcdf::Str key,
                    const std::string &value, uint8_t operation) override {
    if (static_cast<size_t>(table_id) >= global_table_instances.size())
      throw abstract_db::abstract_abort_exception();
    mbta_ordered_index *index = global_table_instances[table_id];
    if (index == nullptr || index->get_is_remote())
      throw abstract_db::abstract_abort_exception();
    // The receiver executes on the owner: every case admits the point, stages
    // and locks its write-set entry here (shard_put / oi_mbta_shard_insert /
    // oi_mbta_shard_remove) instead of re-entering the coordinator-side
    // resolver behind tx_insert/tx_remove. server.cc stage_batch_write runs
    // the same three kernels on the receiver's registered index.
    switch (operation) {
      case 0:
        index->shard_put(key, value);
        return;
      case 1:
        // Stages a real insert of an engine-owned value and locks it.
        oi_mbta_shard_insert(index->mbta, key, value);
        return;
      case 2:
        // Stages a real delete (including an absent-key read) and locks it.
        oi_mbta_shard_remove(index->mbta, key);
        return;
      default:
        throw abstract_db::abstract_abort_exception();
    }
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
    // Dynamic allocation through the historical hint-based API is retired.
    // Report ordinary failure rather than terminating a facade caller.
    (void)name;
    (void)value_size_hint;
    (void)mostly_append;
    (void)use_hashtable;
    return nullptr;
  }


  abstract_ordered_index *
  open_index(const std::string &name, int shard_index) {
    std::lock_guard<std::mutex> lock(table_catalog_mutex_);
    return open_index_locked(name, shard_index);
  }

private:
  abstract_ordered_index *
  open_index_locked(const std::string &name, int shard_index) {
    auto& benchConfig = BenchmarkConfig::getInstance();

    if (shard_index == -1) {
      shard_index = benchConfig.getShardIndex() ;
    }
    if (shard_index < 0 || shard_index >= benchConfig.getNshards())
      return nullptr;

    const auto table_key = std::make_tuple(name, shard_index);
    auto existing = tables_taken.find(table_key);
    if (existing != tables_taken.end()) {
      int table_id = existing->second;
      auto tbl = get_index_by_table_id(table_id) ;
      std::cout << "existing table is created with name: " << name 
              << ", table-id: " << tbl->get_table_id()
              << ", on shard-server id:" << shard_index << std::endl;
      return tbl ;
    }

    auto available = availableTable_id.find(shard_index);
    if (available == availableTable_id.end())
      return nullptr;
    const int available_table_id = available->second;

    // Table ids are in [shard * N + 1, shard * N + N]. Capacity
    // exhaustion is a normal catalog failure: do not consume another id or
    // terminate the process.
    if (!(available_table_id >= shard_index*mako::NUM_TABLES_PER_SHARD+1 
        && available_table_id <= (shard_index*mako::NUM_TABLES_PER_SHARD+mako::NUM_TABLES_PER_SHARD))) {
      return nullptr;
    }

    auto tbl = global_table_instances[available_table_id];
    if (tbl == nullptr)
      return nullptr;
    tbl->set_table_name(name) ;
    // Register table in global registry for policy-based shard routing
    mako::get_table_registry().register_table(
        available_table_id, name, shard_index, !tbl->get_is_remote(), tbl);
    // Record this table to prevent duplicate creation for the same (name, shard)
    tables_taken[table_key] = available_table_id;
    ++available->second;
    std::cout << "new table is created with name: " << name 
              << ", table-id: " << tbl->get_table_id()
              << ", on shard-server id:" << shard_index << std::endl;
    // Publish the new index to the helper receivers already running for this
    // shard. setup_update_table holds the helper lifecycle lock and each
    // receiver's table map is mutex-guarded, so this needs no quiescence. The
    // static profile still reaches it only while every process constructs the
    // same deterministic schema before helper or serving threads start (peers
    // address tables by numeric id); native sharding also reaches it at run
    // time when a governed warehouse index or origin-side proxy is opened
    // lazily. A request naming an index its receiver has not yet learned
    // fails as an aborted request.
    mako::setup_update_table(available_table_id, tbl);
    return tbl;
  }

public:
  mbta_sharded_ordered_index *
  open_sharded_index(const std::string &name) override {
    auto &benchConfig = BenchmarkConfig::getInstance();
    const size_t shard_count = static_cast<size_t>(benchConfig.getNshards());
    std::lock_guard<std::mutex> lock(table_catalog_mutex_);

    // One logical name consumes one of the fixed 200 IDs on every shard.
    // Preflight every shard so capacity failure cannot leave a partially
    // registered logical table.
    for (size_t shard = 0; shard != shard_count; ++shard) {
      const int shard_index = static_cast<int>(shard);
      if (tables_taken.find(std::make_tuple(name, shard_index)) !=
          tables_taken.end())
        continue;
      auto available = availableTable_id.find(shard_index);
      const int last_id =
          shard_index * mako::NUM_TABLES_PER_SHARD +
          mako::NUM_TABLES_PER_SHARD;
      if (available == availableTable_id.end() ||
          available->second > last_id)
        return nullptr;
    }

    return mbta_sharded_build(
        name,
        shard_count,
        [this, &name](size_t shard) {
          return open_index_locked(name, static_cast<int>(shard));
        });
  }

  // replay will use this function, otherwise NO; get table back;
  abstract_ordered_index *
  get_index_by_table_id(unsigned short table_id) {
    if (static_cast<size_t>(table_id) >= global_table_instances.size())
      return nullptr;
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
      if (multi_shard_mode && !benchConfig.getIsReplicated()) {
        // Only unreplicated colocated tables may bypass the owning participant.
        const auto& local_shards = config->local_shard_indices;
        is_local = (std::find(local_shards.begin(), local_shards.end(), shard_index) != local_shards.end());
      } else {
        // Replicated tables retain shard ownership regardless of address space.
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
