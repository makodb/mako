#ifndef _ABSTRACT_ORDERED_INDEX_H_
#define _ABSTRACT_ORDERED_INDEX_H_

#include <stdint.h>
#include <string>
#include <utility>
#include <map>
#include "masstree/str.hh"

#include "../macros.h"
#include "../str_arena.h"

namespace mako {
// Scan conflict-adaptivity seam between the engine's non-txn scan (the
// retry-until-success one-op scan in mbta_wrapper.hh) and callers that can
// SHRINK their window: a caller sets the cap around a scan; the engine scan
// gives up after that many OCC-aborted attempts, discards the partial rows,
// and raises the flag instead of retrying a hopeless window forever. A
// window whose scan takes longer than the conflicting writers' inter-write
// gap NEVER wins no matter how it retries (core-proven live: a 4096-row
// migration chunk scan of a hot stock index pinned its service thread
// indefinitely); the adaptive caller halves the window until it fits the
// gap. Thread-local: scans are single-threaded per call site; 0 = retry
// forever (the historical contract, every caller that does not opt in).
inline thread_local int g_oi_scan_attempt_cap = 0;
inline thread_local bool g_oi_scan_conflicted = false;
// Forensics: rows the LAST aborted scan attempt had collected when it threw.
// Discriminates abort causes: a constant K < window means every attempt dies
// at the SAME row (one poisoned/locked row); K == window means the scan body
// finished and commit-time validation failed (read-set contention); K == 0
// means it died before reading anything (thread/txn state).
inline thread_local size_t g_oi_scan_abort_progress = 0;
// Bounded-attempt seam for the ONE-OP get/put/insert kernels (same opt-in
// shape as the scan cap above; 0 = retry forever, the historical contract).
// The RPC backend's poll thread serves remote non-txn ops INLINE, and an
// unbounded zero-backoff retry on a row held by a parked writer wedges that
// thread -- which also serves GetTimestamp, the very RPC the parked writer's
// commit is waiting on. Two shards doing this to each other latch a
// distributed livelock ring (core-proven live: both shards' workers stuck in
// commit_txn waiting cross-shard timestamps, one customer row locked for
// minutes, the migration scan dying at that row thousands of times). A capped
// kernel throws instead; the server maps it to SERVER_BUSY and the remote
// caller backs off and retries -- the poll thread stays live and the ring
// cannot close.
inline thread_local int g_oi_oneop_attempt_cap = 0;
}  // namespace mako
// mako::oi_scan_wedged, the scan-deadline throwable (micro-header so the
// data-plane service can catch it without this header's weight).
#include "oi_scan_wedged.h"

/**
 * The storage-table interface, authored as rusty-cpp inline-Rust
 * traits (docs/storage-interface.md). The `#if RUSTYCPP_RUST`
 * block below is the source of truth; the transpiler regenerates the
 * committed `/*RUSTYCPP:GEN-BEGIN ...*​/` block via
 *
 *   rusty-cpp-transpiler inline-rust --rewrite --files <this file>
 *
 * (tool built from rusty-cpp upstream main — see the plan's P0 notes;
 * the generated code is plain C++ and needs nothing from the rusty
 * runtime, so the submodule pin is unaffected).
 *
 * Three role traits, mirroring what backends can actually do:
 *   OrderedIndex     — the non-transactional KV surface + bookkeeping;
 *                      every backend implements this.
 *   TxnOrderedIndex  — the transactional ops (caller-managed txn
 *                      handle); mbta and the sharded router implement
 *                      this. Masstree has no transaction runtime.
 *   ShardParticipant — cross-shard 2PC RPC-handler ops that stage
 *                      into the serving thread's ambient Sto
 *                      transaction (no start/commit here; the
 *                      coordinator drives those over later RPCs).
 *
 * `abstract_ordered_index` (below the generated block) is the
 * combined bridge the existing tree consumes; consumers migrate to
 * the narrowest interface they need (plan P3).
 */

// The scan callback, at namespace scope so the DSL traits can name it
// (it was oi_scan_callback; the bridge keeps
// that spelling alive via a member alias).
class oi_scan_callback {
public:
  virtual ~oi_scan_callback() {}
  // Called at the start of EVERY attempt of a whole-scan-retrying scan
  // (oi_mbta_nontxn_scan): an aborted one-op OCC attempt's collected rows
  // are exactly the reads that failed validation -- torn values, plus dupes
  // on retry -- so an accumulating collector must discard them or the scan
  // returns a mix of torn and fresh rows (observed live: the migration
  // catch-up copy left payment-mutated customer fields at stale values).
  // Default no-op preserves the historical behavior of workload collectors.
  virtual void restart() {}
  // XXX(stephentu): key is passed as (const char *, size_t) pair
  // because it really should be the string_type of the underlying
  // tree, but since the interface is not templated we can't
  // really do better than this for now
  //
  // we keep value as std::string b/c we have more control over how those
  // strings are generated
  virtual bool invoke(const char *keyp, size_t keylen,
                      const std::string &value) = 0;
};

// Spellings for the DSL's raw-pointer types (`*mut c_void` etc.).
using c_void = void;
using c_char = char;
// Opaque spelling for clear()'s stats-map return.
using oi_stats_map = std::map<std::string, uint64_t>;

#if RUSTYCPP_RUST
// The non-transactional KV surface (Masstree-shape) + bookkeeping.
//
// Non-txn ops do NOT participate in a caller's transaction; each is
// per-key atomic on its own (see docs/storage-interface.md):
// get/put/insert/scan/rscan run inside an internal one-op OCC
// transaction on backends that have one; remove is a direct raw write
// (documented asymmetry). VALUES ARE RAW BYTES in both directions:
// backends needing a storage encoding apply it internally (mbta
// Encodes on writes; reads/scans come back stripped). Must NOT be
// called from a thread with an open transaction.
//
// put returns "newly inserted"; insert is put-if-absent and returns
// "inserted"; remove returns "existed". scan covers [start, *end) or
// [start, +inf) when end is null; rscan is the descending mirror.
// size() is an estimate; get_table_id/get_is_remote are backend
// identity used by routing.
pub trait OrderedIndex {
    fn get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool;
    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool;
    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool;
    fn remove(&mut self, key: lcdf::Str) -> bool;
    fn scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena);
    fn rscan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena);
    fn size(&self) -> usize;
    fn clear(&mut self) -> oi_stats_map;
    fn get_table_id(&mut self) -> i32;
    fn get_is_remote(&mut self) -> bool;
}

// The transactional ops. txn is the opaque handle from
// abstract_db::new_txn (thread-local Sto state in the mbta backend).
// Unlike the non-txn surface, values passed to put/insert here must
// be mako::Encode()'d by the caller and outlive the commit (the
// backend stores a pointer into the caller's buffer). get covers a
// point read; scan/rscan mirror the non-txn ranges; scanRemoteOne is
// the remote-table single-match range read used by the txn'd remote
// path.
pub trait TxnOrderedIndex: OrderedIndex {
    fn tx_get(&mut self, txn: *mut c_void, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool;
    fn tx_put(&mut self, txn: *mut c_void, key: lcdf::Str, value: &std::string);
    fn tx_insert(&mut self, txn: *mut c_void, key: lcdf::Str, value: &std::string);
    fn tx_remove(&mut self, txn: *mut c_void, key: lcdf::Str);
    fn tx_scan(&mut self, txn: *mut c_void, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena);
    fn tx_rscan(&mut self, txn: *mut c_void, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena);
    fn tx_scan_remote_one(&mut self, txn: *mut c_void, start_key: &std::string, end_key: &std::string, value: &mut std::string);
}

// Cross-shard 2PC (RPC-handler side): these stage reads/writes into
// the RPC thread's ambient Sto transaction for the coordinator-driven
// prepare/commit phases (shard_put also locks its write-set entry via
// Sto::shard_try_lock_last_writeset). They neither start nor commit a
// transaction. For self-contained ops, use OrderedIndex.
pub trait ShardParticipant {
    fn shard_get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool;
    fn shard_put(&mut self, key: lcdf::Str, value: &std::string) -> *const c_char;
    fn shard_scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) -> bool;
}

// The full-role combination: what a table serving transactional
// callers AND 2PC RPC handlers is. Lowered to a multi-base interface;
// DSL backends attach `#[cpp_inherit] impl FullOrderedIndex for X {}`
// and carry their methods in the inherent impl (merged members
// override the inherited virtuals by signature). The hand-written
// abstract_ordered_index bridge derives from this.
pub trait FullOrderedIndex: TxnOrderedIndex + ShardParticipant {
}
#endif
/*RUSTYCPP:GEN-BEGIN id=abstract_ordered_index.1 version=1 rust_sha256=c36262c7476b8b6a434a69826a128d044b17539ff63a8ad9ca5ff900b854ab47*/
class ShardParticipant {
public:
    virtual ~ShardParticipant() noexcept(false) {}
    virtual bool shard_get(lcdf::Str key, std::string& value, size_t max_bytes_read) = 0;
    virtual const c_char* shard_put(lcdf::Str key, const std::string& value) = 0;
    virtual bool shard_scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) = 0;
    ShardParticipant(const ShardParticipant&) = delete;
    ShardParticipant& operator=(const ShardParticipant&) = delete;
    ShardParticipant(ShardParticipant&&) = delete;
    ShardParticipant& operator=(ShardParticipant&&) = delete;
protected:
    ShardParticipant() = default;
};

template <class U> class ShardParticipantAdapter;
template <class U> class ShardParticipantAdapterRef;
template <class U> class ShardParticipantAdapterRefMut;

class OrderedIndex {
public:
    virtual ~OrderedIndex() noexcept(false) {}
    virtual bool get(lcdf::Str key, std::string& value, size_t max_bytes_read) = 0;
    virtual bool put(lcdf::Str key, const std::string& value) = 0;
    virtual bool insert(lcdf::Str key, const std::string& value) = 0;
    virtual bool remove(lcdf::Str key) = 0;
    virtual void scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) = 0;
    virtual void rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) = 0;
    virtual size_t size() const = 0;
    virtual oi_stats_map clear() = 0;
    virtual int32_t get_table_id() = 0;
    virtual bool get_is_remote() = 0;
    OrderedIndex(const OrderedIndex&) = delete;
    OrderedIndex& operator=(const OrderedIndex&) = delete;
    OrderedIndex(OrderedIndex&&) = delete;
    OrderedIndex& operator=(OrderedIndex&&) = delete;
protected:
    OrderedIndex() = default;
};

template <class U> class OrderedIndexAdapter;
template <class U> class OrderedIndexAdapterRef;
template <class U> class OrderedIndexAdapterRefMut;

class TxnOrderedIndex : public OrderedIndex {
public:
    virtual ~TxnOrderedIndex() noexcept(false) {}
    virtual bool tx_get(c_void* txn, lcdf::Str key, std::string& value, size_t max_bytes_read) = 0;
    virtual void tx_put(c_void* txn, lcdf::Str key, const std::string& value) = 0;
    virtual void tx_insert(c_void* txn, lcdf::Str key, const std::string& value) = 0;
    virtual void tx_remove(c_void* txn, lcdf::Str key) = 0;
    virtual void tx_scan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) = 0;
    virtual void tx_rscan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) = 0;
    virtual void tx_scan_remote_one(c_void* txn, const std::string& start_key, const std::string& end_key, std::string& value) = 0;
    TxnOrderedIndex(const TxnOrderedIndex&) = delete;
    TxnOrderedIndex& operator=(const TxnOrderedIndex&) = delete;
    TxnOrderedIndex(TxnOrderedIndex&&) = delete;
    TxnOrderedIndex& operator=(TxnOrderedIndex&&) = delete;
protected:
    TxnOrderedIndex() = default;
};

template <class U> class TxnOrderedIndexAdapter;
template <class U> class TxnOrderedIndexAdapterRef;
template <class U> class TxnOrderedIndexAdapterRefMut;

class FullOrderedIndex : public TxnOrderedIndex, public ShardParticipant {
public:
    virtual ~FullOrderedIndex() noexcept(false) {}
    FullOrderedIndex(const FullOrderedIndex&) = delete;
    FullOrderedIndex& operator=(const FullOrderedIndex&) = delete;
    FullOrderedIndex(FullOrderedIndex&&) = delete;
    FullOrderedIndex& operator=(FullOrderedIndex&&) = delete;
protected:
    FullOrderedIndex() = default;
};

template <class U> class FullOrderedIndexAdapter;
template <class U> class FullOrderedIndexAdapterRef;
template <class U> class FullOrderedIndexAdapterRefMut;
/*RUSTYCPP:GEN-END id=abstract_ordered_index.1*/

/**
 * The interface IS the three traits above — nothing else. The former
 * hand-written bridge (overload merging, default-arg forwarders,
 * string-key members, legacy default bodies) is gone: every name on
 * the class surface has exactly one spelling. `abstract_ordered_index`
 * survives as an alias so the ~125 declaration sites and the legacy
 * subclasses keep compiling verbatim.
 */
using abstract_ordered_index = FullOrderedIndex;

// ---------------------------------------------------------------------------
// Convenience FREE functions (deliberately not members: free-function
// overload sets involve no class-scope name hiding, need no
// using-declarations, and may carry default arguments — everything the
// bridge existed to fake). All dispatch through the trait virtuals.
// ---------------------------------------------------------------------------

inline bool tx_get(FullOrderedIndex *t, void *txn, lcdf::Str key,
                   std::string &value,
                   size_t max_bytes_read = std::string::npos) {
  return t->tx_get(txn, key, value, max_bytes_read);
}
inline bool tx_get(FullOrderedIndex *t, void *txn, const std::string &key,
                   std::string &value,
                   size_t max_bytes_read = std::string::npos) {
  return t->tx_get(txn, lcdf::Str(key.data(), key.size()), value,
                   max_bytes_read);
}
inline void tx_put(FullOrderedIndex *t, void *txn, lcdf::Str key,
                   const std::string &value) {
  t->tx_put(txn, key, value);
}
inline void tx_put(FullOrderedIndex *t, void *txn, const std::string &key,
                   const std::string &value) {
  t->tx_put(txn, lcdf::Str(key.data(), key.size()), value);
}
inline void tx_insert(FullOrderedIndex *t, void *txn, lcdf::Str key,
                      const std::string &value) {
  t->tx_insert(txn, key, value);
}
inline void tx_insert(FullOrderedIndex *t, void *txn, const std::string &key,
                      const std::string &value) {
  t->tx_insert(txn, lcdf::Str(key.data(), key.size()), value);
}
inline void tx_remove(FullOrderedIndex *t, void *txn, lcdf::Str key) {
  t->tx_remove(txn, key);
}
inline void tx_remove(FullOrderedIndex *t, void *txn, const std::string &key) {
  t->tx_remove(txn, lcdf::Str(key.data(), key.size()));
}
inline void tx_scan(FullOrderedIndex *t, void *txn,
                    const std::string &start_key, const std::string *end_key,
                    oi_scan_callback &callback, str_arena *arena = nullptr) {
  t->tx_scan(txn, start_key, end_key, callback, arena);
}
inline void tx_rscan(FullOrderedIndex *t, void *txn,
                     const std::string &start_key, const std::string *end_key,
                     oi_scan_callback &callback, str_arena *arena = nullptr) {
  t->tx_rscan(txn, start_key, end_key, callback, arena);
}

#endif /* _ABSTRACT_ORDERED_INDEX_H_ */
