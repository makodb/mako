#ifndef _BENCHMARK_MBTA_WRAPPER_H_
#define _BENCHMARK_MBTA_WRAPPER_H_
#pragma once
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

class mbta_wrapper;

// PS: we don't use mbta_wraper_norm.hh anymore (in rolis, mbta_wraper_norm is the alias of mbta_wraper)
class mbta_ordered_index : public abstract_ordered_index {    // weihshen, the mbta_ordered_index of Masstrans we need
public:
  mbta_ordered_index(const std::string &name, mbta_wrapper *db, bool is_remote=false) : mbta(), db(db) {
    std::exit(EXIT_FAILURE);
  }

  mbta_ordered_index(const std::string &name, const long table_id, mbta_wrapper *db, bool is_remote=false) : mbta(), db(db) {
      mbta.set_table_id(table_id);
      mbta.set_is_remote(is_remote);
      mbta.set_table_name(name);
  }

  std::string *arena(void);

  bool get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
    if (!mbta.get_is_remote()) {
      STD_OP({
        bool ret = mbta.transGet(key, value);
        if (ret) {
          UPDATE_VS(value.data(),value.length())
          if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
        }
        return ret;
      });
    } else {
      int ret=TThread::sclient->remoteGet(mbta.get_table_id(), key, value);
      if (ret>0) {
        throw abstract_db::abstract_abort_exception();
      }
      if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
        UPDATE_VS(value.data(),value.length())
        value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
      }
      return true;
    }
  }

  bool get_is_remote() {
    return mbta.get_is_remote();
  }

  void set_is_remote(bool s) {
    mbta.set_is_remote(s) ;
  }

  int get_table_id() { // mbta_ordered_index
    return mbta.get_table_id();
  }

  const std::string& get_table_name() const { return mbta.get_table_name(); }

  void set_table_name(const std::string& t) { mbta.set_table_name(t); }

  // handle get request from a remote shard
  bool shard_get(lcdf::Str key, std::string &value, size_t max_bytes_read) {
    STD_OP({
      bool ret = mbta.transGet(key, value);
      return ret;
    });
  }

  bool shard_scan(const std::string &start_key,
    const std::string *end_key,
    scan_callback &callback,
    str_arena *arena = nullptr) {
    mbta_type::Str end = end_key ? mbta_type::Str(*end_key) : mbta_type::Str();

    STD_OP(mbta.transQuery(start_key, end, [&] (mbta_type::Str key, std::string& value) {
    return callback.invoke(key.data(), key.length(), value);
    }, arena));
    return true;
  }

  const char *shard_put(lcdf::Str key, const std::string &value) {
    STD_OP({
        mbta.transPut(key, StringWrapper(value));
        if (!Sto::shard_try_lock_last_writeset()) {
          throw Transaction::Abort();
        }
        return 0;
    });
  }

  const char *put(void* txn,
                  lcdf::Str key,
                  const std::string &value)
  {
#if OP_LOGGING
    mt_put++;
#endif
    STD_OP({
        mbta.transPut(key, StringWrapper(value));
        return 0;
    });
  }

  const char *put_mbta(void *txn,
                     const lcdf::Str key,
                     bool(*compar)(const std::string& newValue,const std::string& oldValue),
                     const std::string &value) {
    STD_OP({
        mbta.transPutMbta(key, StringWrapper(value), compar);
        return 0;
    });
  }
  
const char *insert(void *txn,
	     lcdf::Str key,
	     const std::string &value)
{
STD_OP(mbta.transInsert(key, StringWrapper(value)); return 0;)
}

void remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
mt_del++;
#endif
STD_OP(mbta.transDelete(key));
}

void scan(void *txn,
    const std::string &start_key,
    const std::string *end_key,
    scan_callback &callback,
    str_arena *arena = nullptr) {
#if OP_LOGGING
mt_scan++;
#endif    
mbta_type::Str end = end_key ? mbta_type::Str(*end_key) : mbta_type::Str();
STD_OP(mbta.transQuery(start_key, end, [&] (mbta_type::Str key, std::string& value) {

  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);;
  return callback.invoke(key.data(), key.length(), value);
}, arena));
}

void scanRemoteOne(void *txn,
    const std::string &start_key,
    const std::string &end_key,
    std::string &value) {
  int ret=TThread::sclient->remoteScan(mbta.get_table_id(), start_key, end_key, value);
  if (ret>0) {
    throw abstract_db::abstract_abort_exception();
  }
  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) {
    UPDATE_VS(value.data(),value.length())
    value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);
  }
}

void rscan(void *txn,
     const std::string &start_key,
     const std::string *end_key,
     scan_callback &callback,
     str_arena *arena = nullptr) {
#if 1
#if OP_LOGGING
mt_rscan++;
#endif
mbta_type::Str end = end_key ? mbta_type::Str(*end_key) : mbta_type::Str();
STD_OP(mbta.transRQuery(start_key, end, [&] (mbta_type::Str key, std::string& value) {

  if (value.length() >= mako::EXTRA_BITS_FOR_VALUE) value.resize(value.length() - mako::EXTRA_BITS_FOR_VALUE);;
  return callback.invoke(key.data(), key.length(), value);
}, arena));
#endif
}

size_t size() const
{
return mbta.approx_size();
}

// TODO: unclear if we need to implement, apparently this should clear the tree and possibly return some stats
std::map<std::string, uint64_t>
clear() {
throw 2;
}

#if STO_OPACITY
typedef MassTrans<std::string, versioned_str_struct, true/*opacity*/> mbta_type;
#else
typedef MassTrans<std::string, versioned_str_struct, false/*opacity*/> mbta_type;
#endif

private:
friend class mbta_wrapper;
mbta_type mbta;
std::map<std::string, std::string> properties;

mbta_wrapper *db;
};

/*
class ht_ordered_index_string : public abstract_ordered_index {
public:
ht_ordered_index_string(const std::string &name, mbta_wrapper *db) : ht(), name(name), db(db) {}

std::string *arena(void);

bool get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
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

const char *put(
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
        return 0;
          });
  }

  const char *insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
	ht.transPut(key, StringWrapper(value)); return 0;
	});
  }

  void remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
	ht.transDelete(key);
    });
  }

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             scan_callback &callback,
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

  bool get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
    return false;
  }

  bool get(
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


  const char *put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
    return 0;
  }

  const char *put(
      void* txn,
      int32_t key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value));
        return 0;
          });
  }

  
  const char *insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
    return 0;
  }

  const char *insert(void *txn,
                     int32_t key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value)); return 0;});
  }


  void remove(void *txn, lcdf::Str key) {
      return;
  }

  void remove(void *txn, int32_t key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        ht.transDelete(key);});
  }     

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             scan_callback &callback,
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

  bool get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
    return false;
  }

  bool get(
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


  const char *put(
      void* txn,
      lcdf::Str key,
      const std::string &value)
  {
    return 0;
  }

  const char *put(
      void* txn,
      customer_key key,
      const std::string &value)
  {
#if OP_LOGGING
    ht_put++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value));
        return 0;
          });
  }

  
  const char *insert(void *txn,
                     lcdf::Str key,
                     const std::string &value)
  {
    return 0;
  }

  const char *insert(void *txn,
                     customer_key key,
                     const std::string &value)
  {
#if OP_LOGGING
    ht_insert++;
#endif
    STD_OP({
        ht.transPut(key, StringWrapper(value)); return 0;});
  }


  void remove(void *txn, lcdf::Str key) {
      return;
  }

  void remove(void *txn, customer_key key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        ht.transDelete(key);});
  }     

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             scan_callback &callback,
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

  bool get(
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
  
  const char *put(
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

  const char *insert(void *txn,
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

  void remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        assert(key.length() == sizeof(history_key));
        const history_key& k = *(reinterpret_cast<const history_key*>(key.data()));
        ht.transDelete(k);});
  }     

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             scan_callback &callback,
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

  bool get(
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
  
  const char *put(
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

  const char *insert(void *txn,
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

  void remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        assert(key.length() == sizeof(oorder_key));
        const oorder_key& k = *(reinterpret_cast<const oorder_key*>(key.data()));
        ht.transDelete(k);});
  }     

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             scan_callback &callback,
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

  bool get(
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
  
  const char *put(
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

  const char *insert(void *txn,
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

  void remove(void *txn, lcdf::Str key) {
#if OP_LOGGING
    ht_del++;
#endif    
    STD_OP({
        assert(key.length() == sizeof(stock_key));
        const stock_key& k = *(reinterpret_cast<const stock_key*>(key.data()));
        ht.transDelete(k);});
  }     

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            scan_callback &callback,
            str_arena *arena = nullptr) {
    NDB_UNIMPLEMENTED("scan");
  }

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             scan_callback &callback,
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
    static int partition_id = 0; // to distinguish different worker thread
    TThread::set_id(__sync_fetch_and_add(&tidcounter, 1));
    TThread::set_mode(0); // checking in-progress
    TThread::set_num_eprc_server(BenchmarkConfig::getInstance().getNumErpcServer());
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
      size_t old = __sync_fetch_and_add(&partition_id, 1);
      TThread::set_pid (old);

      TThread::sclient = new mako::ShardClient(BenchmarkConfig::getInstance().getConfig()->configFile,
                                                 BenchmarkConfig::getInstance().getCluster(),
                                                 BenchmarkConfig::getInstance().getShardIndex(),
                                                 old);
      //Notice("ParID[worker-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    } else {
      TThread::set_pid(TThread::id()%BenchmarkConfig::getInstance().getConfig()->warehouses);
      //Notice("ParID[load-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    }
    
    if (TThread::id() == 0) {
      // someone has to do this (they don't provide us with a general init callback)
      mbta_ordered_index::mbta_type::static_init();
      // need this too
      pthread_t advancer;
      pthread_create(&advancer, NULL, Transaction::epoch_advancer, NULL);
      pthread_detach(advancer);
    }
    mbta_ordered_index::mbta_type::thread_init();
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
    std::cout << "deprecated function!" << std::endl;
    std::exit(EXIT_FAILURE);

    // We only actually create tables in preallocate_open_index now!
    auto ret = new mbta_ordered_index(name, this, name.find("dummy") != std::string::npos);
    std::cout << "new table is createded with name: " << name << ", id: " << ret->get_table_id() << std::endl;
    return ret;
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
      std::cout << "existing table is createded with name: " << name 
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
    // Record this table to prevent duplicate creation for the same (name, shard)
    tables_taken[std::make_tuple(name, shard_index)] = available_table_id;
    std::cout << "new table is createded with name: " << name 
              << ", table-id: " << tbl->get_table_id()
              << ", on shard-server id:" << shard_index << std::endl;
    mako::setup_update_table(available_table_id, tbl);
    return tbl;
  }

  mbta_sharded_ordered_index *
  open_sharded_index(const std::string &name) override {
    auto &benchConfig = BenchmarkConfig::getInstance();
    const size_t shard_count = static_cast<size_t>(benchConfig.getNshards());
    return mbta_sharded_ordered_index::build(
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
    for (int i=0; i<=mako::NUM_TABLES_PER_SHARD * benchConfig.getNshards(); i++) {
      int table_id = i;
      auto tbl = new mbta_ordered_index(std::to_string(table_id), table_id, this);
      int shard_index = (table_id - 1) / mako::NUM_TABLES_PER_SHARD;
      if (table_id==0) {
        shard_index = 0;  // table id 0 is not used!
      }
      if (shard_index == benchConfig.getShardIndex()) {
        tbl->set_is_remote(false) ;
      } else {
        tbl->set_is_remote(true) ;
      }
      global_table_instances.push_back(tbl);
    }
  }

 void
 close_index(abstract_ordered_index *idx) {
   delete idx;
 }

};

__thread str_arena* mbta_wrapper::thr_arena;

std::string *mbta_ordered_index::arena() {
  return (*db->thr_arena)();
}

#endif
