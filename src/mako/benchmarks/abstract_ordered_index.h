#ifndef _ABSTRACT_ORDERED_INDEX_H_
#define _ABSTRACT_ORDERED_INDEX_H_

#include <stdint.h>
#include <string>
#include <utility>
#include <map>
#include "masstree/str.hh"

#include "../macros.h"
#include "../str_arena.h"
#include "tpcc_keys.h"

/**
 * The underlying index manages memory for keys/values, but
 * may choose to expose the underlying memory to callers
 * (see put() and inesrt()).
 */
class abstract_ordered_index {
public:

  virtual ~abstract_ordered_index() {}

  /**
   * Get a key of length keylen. The underlying DB does not manage
   * the memory associated with key. Returns true if found, false otherwise
   */
  virtual bool get(
      void *txn,
      lcdf::Str key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) = 0;

  // Cross-shard 2PC (RPC-handler side): read a key WITHOUT a
  // caller-supplied txn handle, adding it to the RPC thread's ambient
  // Sto transaction read-set for later 2PC validation. This does NOT
  // start or commit a transaction — it stages into whatever
  // distributed transaction the coordinator is driving across RPCs.
  // For self-contained non-transactional reads, use the Masstree-shape
  // get(key, value) further down instead.
  virtual bool shard_get(
      lcdf::Str key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) = 0;

  virtual bool get(
      void *txn,
      const std::string &key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
      return get(txn, lcdf::Str(key), value, max_bytes_read);
  }

  virtual bool get(
      void *txn,
      int32_t key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
      return get(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value, max_bytes_read);
  }
  
  virtual bool get(
      void *txn,
      customer_key key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
      return get(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value, max_bytes_read);
  }

  class scan_callback {
  public:
    virtual ~scan_callback() {}
    // XXX(stephentu): key is passed as (const char *, size_t) pair
    // because it really should be the string_type of the underlying
    // tree, but since abstract_ordered_index is not templated we can't
    // really do better than this for now
    //
    // we keep value as std::string b/c we have more control over how those
    // strings are generated
    virtual bool invoke(const char *keyp, size_t keylen,
                        const std::string &value) = 0;
  };

  /**
   * Search [start_key, *end_key) if end_key is not null, otherwise
   * search [start_key, +infty)
   */
  virtual void scan(
      void *txn,
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena = nullptr) = 0;

  // Cross-shard 2PC (RPC-handler side): scan staged into the RPC
  // thread's ambient Sto transaction, like shard_get. Not a
  // self-contained scan — see the non-txn scan(start, end, cb) below
  // for that.
  virtual bool shard_scan(
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena = nullptr) = 0;

  virtual void scanRemoteOne(
      void *txn,
      const std::string &start_key,
      const std::string &end_key,
      std::string &value) = 0;

  /**
   * Search (*end_key, start_key] if end_key is not null, otherwise
   * search (-infty, start_key] (starting at start_key and traversing
   * backwards)
   */
  virtual void rscan(
      void *txn,
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena = nullptr) = 0;

  /**
   * Put a key of length keylen, with mapping of length valuelen.
   * The underlying DB does not manage the memory pointed to by key or value
   * (a copy is made).
   *
   * If a record with key k exists, overwrites. Otherwise, inserts.
   *
   * If the return value is not NULL, then it points to the actual stable
   * location in memory where the value is located. Thus, [ret, ret+valuelen)
   * will be valid memory, bytewise equal to [value, value+valuelen), since the
   * implementations have immutable values for the time being. The value
   * returned is guaranteed to be valid memory until the key associated with
   * value is overriden.
   */
  virtual const char *
  put(void *txn,
      lcdf::Str key,
      const std::string &value) = 0;

   virtual const char *
   put_mbta(void *txn,
       lcdf::Str key,
       bool(*compar)(const std::string& newValue,const std::string& oldValue),
       const std::string &value) = 0;

  // Cross-shard 2PC (RPC-handler side): stage a write into the RPC
  // thread's ambient Sto transaction AND lock its write-set entry
  // (Sto::shard_try_lock_last_writeset) for the 2PC prepare phase.
  // The commit happens later when the coordinator drives it. Not a
  // self-contained write — see the non-txn put(key, value) below for
  // that.
  virtual const char *
  shard_put(lcdf::Str key,
      const std::string &value) = 0;

  virtual const char *
  put(void *txn,
      const std::string &key,
      const std::string &value) {
      return put(txn, lcdf::Str(key), value);
  }

  virtual const char *
  put_mbta(void *txn,
      const std::string &key,
      bool(*compar)(const std::string& newValue,const std::string& oldValue),
      const std::string &value) {
      return put_mbta(txn, lcdf::Str(key), compar, value);
  }

  virtual const char *
  put(void *txn,
      lcdf::Str key,
      std::string &&value)
  {
      return put(txn, key, static_cast<const std::string &>(value));
  }



  virtual const char *
  put(void *txn,
      const std::string &key,
      std::string &&value)
  {   
      return put(txn, key, static_cast<const std::string &>(value));
  }  


  virtual const char *
  put(void *txn,
         int32_t key,
         const std::string &value)
  {
      return put(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }

  virtual const char *
  put(void *txn,
         int32_t key,
         std::string &&value)
  {
      return put(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }


  virtual const char *
  put(void *txn,
         customer_key key,
         const std::string &value)
  {
      return put(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }

  virtual const char *
  put(void *txn,
         customer_key key,
         std::string &&value)
  {
      return put(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }



  /**
   * Insert a key of length keylen.
   *
   * If a record with key k exists, behavior is unspecified- this function
   * is only to be used when you can guarantee no such key exists (ie in loading phase)
   *
   * Default implementation calls put(). See put() for meaning of return value.
   */
  virtual const char *
  insert(void *txn,
         lcdf::Str key,
         const std::string &value)
  {
    return put(txn, key, value);
  }

  virtual const char *
  insert(void *txn,
         const std::string &key,
         const std::string &value)
  {      
    return insert(txn, lcdf::Str(key), value);
  } 

  virtual const char *
  insert(void *txn,
         lcdf::Str key,
         std::string &&value)
  {
      return insert(txn, key, static_cast<const std::string &>(value));
  }

  virtual const char *
  insert(void *txn,
         const std::string &key,
         std::string &&value)
  {      
      return insert(txn, key, static_cast<const std::string &>(value));
  }   

  virtual const char *
  insert(void *txn,
         int32_t key,
         const std::string &value)
  {
      return insert(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }

  virtual const char *
  insert(void *txn,
         int32_t key,
         std::string &&value)
  {
      return insert(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }

  virtual const char *
  insert(void *txn,
         customer_key key,
         const std::string &value)
  {      
      return insert(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }
  
  virtual const char *
  insert(void *txn,
         customer_key key,
         std::string &&value)
  {      
      return insert(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)), value);
  }


  /**
   * Default implementation calls put() with NULL (zero-length) value
   */
  virtual void remove(
      void *txn,
      lcdf::Str key)
  {
    put(txn, key, "");
  }

  virtual void remove(
      void *txn,
      const std::string &key)
  {   
    remove(txn, lcdf::Str(key));
  } 

  virtual void remove(
      void *txn,
      int32_t key)
  {
    remove(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)));
  }

  virtual void remove(
      void *txn,
      customer_key key)
  {
    remove(txn, lcdf::Str(reinterpret_cast<const char*>(&key), sizeof(key)));
  }

  // ==========================================================================
  // Non-transactional API (Masstree-shape).
  //
  // These ops do NOT participate in a caller's transaction; each is
  // per-key atomic on its own. See docs/silo-masstree-api-unification.md
  // for the semantic contract:
  //   - get/put/insert/scan/rscan run inside an internal one-op OCC
  //     transaction (safe under concurrent OCC readers/writers).
  //   - remove is a direct raw write (fast; documented asymmetry).
  //
  // VALUES ARE RAW BYTES on this surface, in both directions: backends
  // that need a storage encoding apply it internally (mbta applies
  // mako::Encode on writes; reads/scans already come back stripped).
  // This is what makes implementations interchangeable — callers hold
  // an abstract_ordered_index* and pick the backend at construction:
  // masstree_ordered_index (plain Masstree, no transactions),
  // mbta_ordered_index (Silo), or mbta_sharded_ordered_index (Mako
  // routing). Note the txn'd put/insert above KEEP the caller-Encodes
  // convention — they store a pointer into the caller's buffer until
  // commit, so the caller must own the encoded copy's lifetime.
  //
  // Default implementations abort loudly: only backends that support
  // non-txn access override them. Callers must not assume every
  // abstract_ordered_index supports this API.
  //
  // CONSTRAINT: these must NOT be called from a thread that has an
  // open transaction (the internal one-op txn would trip
  // Sto::start_transaction's in-progress assertion). Finish or abort
  // the thread's transaction first.
  // ==========================================================================

  // @unsafe - default aborts via NDB_UNIMPLEMENTED
  virtual bool get(
      lcdf::Str key,
      std::string &value,
      size_t max_bytes_read = std::string::npos) {
    (void)key; (void)value; (void)max_bytes_read;
    NDB_UNIMPLEMENTED("non-txn get");
  }

  // Overwrite semantics; returns true iff the key was newly inserted.
  // @unsafe - default aborts via NDB_UNIMPLEMENTED
  virtual bool put(
      lcdf::Str key,
      const std::string &value) {
    (void)key; (void)value;
    NDB_UNIMPLEMENTED("non-txn put");
  }

  // Put-if-absent; returns true iff the key was newly inserted.
  // @unsafe - default aborts via NDB_UNIMPLEMENTED
  virtual bool insert(
      lcdf::Str key,
      const std::string &value) {
    (void)key; (void)value;
    NDB_UNIMPLEMENTED("non-txn insert");
  }

  // Returns true iff the key existed. Direct raw write — see the
  // asymmetry note above.
  // @unsafe - default aborts via NDB_UNIMPLEMENTED
  virtual bool remove(lcdf::Str key) {
    (void)key;
    NDB_UNIMPLEMENTED("non-txn remove");
  }

  // Forward scan [start_key, *end_key), or [start_key, +infty) when
  // end_key is null. Same callback contract as the transactional scan.
  // @unsafe - default aborts via NDB_UNIMPLEMENTED
  virtual void scan(
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena = nullptr) {
    (void)start_key; (void)end_key; (void)callback; (void)arena;
    NDB_UNIMPLEMENTED("non-txn scan");
  }

  // Reverse scan (*end_key, start_key], or (-infty, start_key] when
  // end_key is null; descending order.
  // @unsafe - default aborts via NDB_UNIMPLEMENTED
  virtual void rscan(
      const std::string &start_key,
      const std::string *end_key,
      scan_callback &callback,
      str_arena *arena = nullptr) {
    (void)start_key; (void)end_key; (void)callback; (void)arena;
    NDB_UNIMPLEMENTED("non-txn rscan");
  }

  /**
   * Only an estimate, not transactional!
   */
  virtual size_t size() const = 0;

  /**
   * Not thread safe for now
   */
  virtual std::map<std::string, uint64_t> clear() = 0;

  virtual void print_stats() { }

  // mbta_ordered_index only has one mbta object which is the instance of MassTrans (TObject)
  virtual int get_table_id() = 0;
  virtual bool get_is_remote() = 0;
};

#endif /* _ABSTRACT_ORDERED_INDEX_H_ */
