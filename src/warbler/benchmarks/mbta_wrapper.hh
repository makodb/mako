#ifndef _BENCHMARK_MBTA_WRAPPER_H_
#define _BENCHMARK_MBTA_WRAPPER_H_
#pragma once
#include <atomic>
#include "abstract_db.h"
#include "abstract_ordered_index.h"
#include "sto/Transaction.hh"
#include "sto/MassTrans.hh"
#include "sto/Hashtable.hh"
#include "sto/simple_str.hh"
#include "sto/StringWrapper.hh"
#include <unordered_map> 
#include "benchmarks/tpcc.h"

// We have to do it on the coordinator instead of transaction.cc, because it only has a local copy of the readSet;
#define GET_NODE_POINTER(val,len) reinterpret_cast<srolis::Node *>((char*)(val+len-srolis::BITS_OF_NODE));
#define GET_NODE_EXTRA_POINTER(val,len) reinterpret_cast<uint32_t *>((char*)(val+len-srolis::EXTRA_BITS_FOR_VALUE));
#define MAX(a,b) ((a)>(b)?(a):(b))

#if defined(FAIL_NEW_VERSION)
// control_mode==4, If a value is in the old epoch while this transaction is from the new epoch,  if not stable, we put it in the queue.
// If control_mode==4
#define UPDATE_VS(val,len) \
  srolis::Node *header = GET_NODE_POINTER(val,len); \
  uint32_t *shardtimestamp = GET_NODE_EXTRA_POINTER(val,len); \
  if (MERGE_KEYS_GROUPS < SHARDS) { \
    int cnt_per_group = SHARDS / MERGE_KEYS_GROUPS; \
    for (int i=0; i<SHARDS;i++) { \
      int ii=i/cnt_per_group; \
      TThread::txn->timestampsReadSet[i]=MAX(TThread::txn->timestampsReadSet[i],header->timestamps[ii]); \
    } \
  } else { \
    for (int i=0;i<SHARDS;i++){\
      TThread::txn->timestampsReadSet[i]=MAX(TThread::txn->timestampsReadSet[i],header->timestamps[i]);\
    } \
  } \
  if (control_mode==4) { \
    if (*shardtimestamp % 10 < TThread::txn->current_term_ && sync_util::sync_logger::safety_check(header->timestamps)){ \
      TThread::transget_without_stable = true; \
      TThread::transget_without_throw = true; \
    } \
  }
#else
#define UPDATE_VS(val,len) \
  srolis::Node *header = GET_NODE_POINTER(val,len); \
  if (MERGE_KEYS_GROUPS < SHARDS) { \
    int cnt_per_group = SHARDS / MERGE_KEYS_GROUPS; \
    for (int i=0; i<SHARDS;i++) { \
      int ii=i/cnt_per_group; \
      TThread::txn->timestampsReadSet[i]=MAX(TThread::txn->timestampsReadSet[i],header->timestamps[ii]); \
    } \
  } else { \
    for (int i=0;i<SHARDS;i++){\
      TThread::txn->timestampsReadSet[i]=MAX(TThread::txn->timestampsReadSet[i],header->timestamps[i]);\
    } \
  } \
  if (control_mode==1){ \
    if (TThread::txn->timestampsReadSet[0]>sync_util::sync_logger::failed_shard_ts){ \
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

namespace table_logger {
	std::atomic<long> table_id (1);
	static std::map<std::string, long> table_map = {};
    static std::map<long, std::string> reversed_table_map = {};

    /*
	 * Generated new table id for each new table name
	 * */
	long GetNextTableID(const std::string &name){
	  long _table_id=0;
	  auto search = table_logger::table_map.find(name);
	  if(search == table_logger::table_map.end()){
		// not found
		table_logger::table_map[name] = table_logger::table_id++;
		_table_id = table_logger::table_map[name];
        std::cout << "Table: " << name << ", Id: " << _table_id << std::endl;
        table_logger::reversed_table_map[_table_id] = name;
	  } else {
		_table_id = search->second;
	  }
	  return _table_id;
	}

    std::string getTableNameById(int table_id) {
        return reversed_table_map[table_id];
    }
}

class mbta_wrapper;

// PS: we don't use mbta_wraper_norm.hh anymore (in rolis, mbta_wraper_norm is the alias of mbta_wraper)
class mbta_ordered_index : public abstract_ordered_index {    // weihshen, the mbta_ordered_index of Masstrans we need
public:
  mbta_ordered_index(const std::string &name, mbta_wrapper *db, bool is_dummy=false) : mbta(), name(name), db(db) {
    mbta.set_table_id(table_logger::GetNextTableID(name));
    mbta.set_is_dummy(is_dummy);
  }

  mbta_ordered_index(const std::string &name, const long table_id, mbta_wrapper *db, bool is_dummy=false) : mbta(), name(name), db(db) {
      mbta.set_table_id(table_id);
      mbta.set_is_dummy(is_dummy);
      table_logger::table_id=mbta.get_table_id()+1;
  }

  std::string *arena(void);

  bool get(void *txn, lcdf::Str key, std::string &value, size_t max_bytes_read) {
    if (!mbta.get_is_dummy()) {
      STD_OP({
        bool ret = mbta.transGet(key, value);
        UPDATE_VS(value.data(),value.length())
        return ret;
      });
    } else {
      int ret=TThread::sclient->remoteGet(mbta.get_table_id(), key, value);
      if (ret>0) {
        throw abstract_db::abstract_abort_exception();
      }
      UPDATE_VS(value.data(),value.length())
      return true;
    }
  }

  bool get_is_dummy() {
    return mbta.get_is_dummy();
  }

  int get_table_id() { // mbta_ordered_index
    return mbta.get_table_id();
  }

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
    // TODO: there's an overload of put that takes non-const std::string and silo seems to use move for those.
    // may be worth investigating if we can use that optimization to avoid copying keys
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
  UPDATE_VS(value.data(),value.length())
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
const std::string name;
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
  std::unordered_map<size_t,abstract_ordered_index *> table_instances ;
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
    TThread::set_num_eprc_server(num_erpc_server);
#if defined(DISABLE_MULTI_VERSION)
    TThread::disable_multiversion();
#else
    TThread::enable_multiverison();
#endif
    TThread::set_shard_index(shardIndex);
    TThread::set_nshards(nshards);
    TThread::set_warehouses(config->warehouses);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    TThread::the_debug_bit = 0;
    if (cluster.compare("localhost")==0){
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

      TThread::sclient = new srolis::ShardClient(config->configFile,
                                                 cluster,
                                                 shardIndex,
                                                 old,
                                                 workload_type); // only support tpcc for now
      //Notice("ParID[worker-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getPartitionID(),TThread::id(),config->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    } else {
      TThread::set_pid(TThread::id()%config->warehouses);
      //Notice("ParID[load-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getPartitionID(),TThread::id(),config->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
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
    return Sto::try_commit();
  }

  bool commit_txn_no_paxos(void *txn) {
    return Sto::try_commit_no_paxos();
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

  void shard_install(std::vector<uint32_t> vectorT) {
    Sto::shard_install(vectorT);
  }

  void shard_serialize_util(std::vector<uint32_t> vectorT)  {
    Sto::shard_serialize_util(vectorT); // it MUST be successful!!!
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
    /*
    if (use_hashtable) {
      if (name.find("customer") == 0) 
        return new ht_ordered_index_customer_key(name, this);
      if (name.find("history") == 0)
	return new ht_ordered_index_history_key(name, this);
      if (name.find("oorder") == 0)
	return new ht_ordered_index_oorder_key(name, this);
      if (name.find("stock") == 0)
        return new ht_ordered_index_stock_key(name, this);
      return new ht_ordered_index_int(name, this);
    }*/
    auto ret = new mbta_ordered_index(name, this, name.find("dummy") != std::string::npos);
    return ret;
  }

  // this function is not thread-safe, please pre-allocate all tables!!
  abstract_ordered_index *
  open_index(unsigned short table_id) {
    if (likely(table_instances.find(table_id) != table_instances.end())) {
      return table_instances[table_id] ;
    } else {
      table_instances[table_id] = new mbta_ordered_index(std::to_string(table_id), table_id, this);
      // grep -r 'new table is'|sort|cut -d ':' -f 3,3|sort -n
      std::cout << "new table is created: " << table_id << std::endl;
      return table_instances[table_id] ;
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
