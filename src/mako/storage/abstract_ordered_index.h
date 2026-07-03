#ifndef _ABSTRACT_ORDERED_INDEX_H_
#define _ABSTRACT_ORDERED_INDEX_H_

#include <stdint.h>
#include <string>
#include <utility>
#include <map>
#include "masstree/str.hh"

#include "../macros.h"
#include "../str_arena.h"

/**
 * The storage-table interface, authored as rusty-cpp inline-Rust
 * traits (docs/ordered-index-trait-plan.md). The `#if RUSTYCPP_RUST`
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
// (it was abstract_ordered_index::scan_callback; the bridge keeps
// that spelling alive via a member alias).
class oi_scan_callback {
public:
  virtual ~oi_scan_callback() {}
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

#if RUSTYCPP_RUST
// The non-transactional KV surface (Masstree-shape) + bookkeeping.
//
// Non-txn ops do NOT participate in a caller's transaction; each is
// per-key atomic on its own (see docs/silo-masstree-api-unification.md):
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
/*RUSTYCPP:GEN-BEGIN id=abstract_ordered_index.1 version=1 rust_sha256=d69fd6d42054573c9ad5fcc8e3474b6bfdbb93a1f64594abc19119f76fc7d6ac*/
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
 * abstract_ordered_index — the combined bridge over the generated
 * role interfaces. Existing code holds this type; new code should
 * prefer the narrowest role interface it needs. Carries what traits
 * cannot: default arguments (as non-virtual forwarders), the
 * string-key spellings (lcdf::Str has no implicit std::string ctor),
 * legacy default bodies, and the two bookkeeping virtuals whose
 * types aren't worth expressing in the DSL.
 */
class abstract_ordered_index : public FullOrderedIndex {
public:
  virtual ~abstract_ordered_index() {}

  // Compat spelling for the callback (pre-trait code says
  // abstract_ordered_index::scan_callback everywhere).
  using scan_callback = oi_scan_callback;

  // C++ name hiding: the bridge's tx_-named sugar (forwarders,
  // string-key forms) hides the inherited tx_ virtuals; re-expose
  // them. The plain (non-txn) verbs no longer collide with anything —
  // the tx_ rename dissolved the cross-trait overload families.
  using TxnOrderedIndex::tx_get;
  using TxnOrderedIndex::tx_put;
  using TxnOrderedIndex::tx_insert;
  using TxnOrderedIndex::tx_remove;
  using TxnOrderedIndex::tx_scan;
  using TxnOrderedIndex::tx_rscan;
  using ShardParticipant::shard_get;
  using ShardParticipant::shard_scan;

  // ------------------------------------------------------------------
  // Legacy default bodies (previously defaults on the base virtuals).
  // ------------------------------------------------------------------

  // txn'd insert: "behavior unspecified if key exists" — default is put.
  void tx_insert(void *txn, lcdf::Str key, const std::string &value) override {
    tx_put(txn, key, value);
  }

  // txn'd remove: default is put of an empty value.
  void tx_remove(void *txn, lcdf::Str key) override {
    tx_put(txn, key, "");
  }

  // Non-txn surface: only backends that support non-txn access
  // override these; the defaults abort loudly. Callers must not
  // assume every abstract_ordered_index supports this API.
  // @unsafe - defaults abort via NDB_UNIMPLEMENTED
  bool get(lcdf::Str key, std::string &value,
           size_t max_bytes_read) override {
    (void)key; (void)value; (void)max_bytes_read;
    NDB_UNIMPLEMENTED("non-txn get");
  }
  bool put(lcdf::Str key, const std::string &value) override {
    (void)key; (void)value;
    NDB_UNIMPLEMENTED("non-txn put");
  }
  bool insert(lcdf::Str key, const std::string &value) override {
    (void)key; (void)value;
    NDB_UNIMPLEMENTED("non-txn insert");
  }
  bool remove(lcdf::Str key) override {
    (void)key;
    NDB_UNIMPLEMENTED("non-txn remove");
  }
  void scan(const std::string &start_key, const std::string *end_key,
            oi_scan_callback &callback, str_arena *arena) override {
    (void)start_key; (void)end_key; (void)callback; (void)arena;
    NDB_UNIMPLEMENTED("non-txn scan");
  }
  void rscan(const std::string &start_key, const std::string *end_key,
             oi_scan_callback &callback, str_arena *arena) override {
    (void)start_key; (void)end_key; (void)callback; (void)arena;
    NDB_UNIMPLEMENTED("non-txn rscan");
  }

  // ------------------------------------------------------------------
  // Default-argument forwarders. Rust traits carry no default
  // arguments, so the generated virtuals take every parameter; these
  // non-virtual spellings restore the historical call shapes
  // (max_bytes_read = npos, arena = nullptr).
  // ------------------------------------------------------------------
  bool tx_get(void *txn, lcdf::Str key, std::string &value) {
    return tx_get(txn, key, value, std::string::npos);
  }
  bool shard_get(lcdf::Str key, std::string &value) {
    return shard_get(key, value, std::string::npos);
  }
  bool get(lcdf::Str key, std::string &value) {
    return get(key, value, std::string::npos);
  }
  void tx_scan(void *txn, const std::string &start_key,
               const std::string *end_key, oi_scan_callback &callback) {
    tx_scan(txn, start_key, end_key, callback, nullptr);
  }
  void tx_rscan(void *txn, const std::string &start_key,
                const std::string *end_key, oi_scan_callback &callback) {
    tx_rscan(txn, start_key, end_key, callback, nullptr);
  }
  bool shard_scan(const std::string &start_key,
                  const std::string *end_key, oi_scan_callback &callback) {
    return shard_scan(start_key, end_key, callback, nullptr);
  }
  void scan(const std::string &start_key, const std::string *end_key,
            oi_scan_callback &callback) {
    scan(start_key, end_key, callback, nullptr);
  }
  void rscan(const std::string &start_key, const std::string *end_key,
             oi_scan_callback &callback) {
    rscan(start_key, end_key, callback, nullptr);
  }

  // ------------------------------------------------------------------
  // Non-virtual string-key conveniences (lcdf::Str has no implicit
  // std::string constructor). Backends override only the lcdf::Str
  // core.
  // ------------------------------------------------------------------
  bool tx_get(void *txn, const std::string &key, std::string &value,
              size_t max_bytes_read = std::string::npos) {
    return tx_get(txn, lcdf::Str(key.data(), key.size()), value,
                  max_bytes_read);
  }
  void tx_put(void *txn, const std::string &key, const std::string &value) {
    tx_put(txn, lcdf::Str(key.data(), key.size()), value);
  }
  void tx_insert(void *txn, const std::string &key, const std::string &value) {
    tx_insert(txn, lcdf::Str(key.data(), key.size()), value);
  }
  void tx_remove(void *txn, const std::string &key) {
    tx_remove(txn, lcdf::Str(key.data(), key.size()));
  }

  // ------------------------------------------------------------------
  // Bookkeeping kept hand-written: clear()'s return type isn't worth
  // expressing in the DSL; print_stats is an optional hook.
  // ------------------------------------------------------------------

  /** Not thread safe for now */
  virtual std::map<std::string, uint64_t> clear() = 0;

  virtual void print_stats() {}
};

#endif /* _ABSTRACT_ORDERED_INDEX_H_ */
