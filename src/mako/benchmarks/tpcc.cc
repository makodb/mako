/**
 * An implementation of TPC-C based off of:
 * https://github.com/oltpbenchmark/oltpbench/tree/master/src/com/oltpbenchmark/benchmarks/tpcc
 * 
 * Specially,
 *   there is only one table in the micro-benchmark: items shared by all worker threads
 */

#include <sys/time.h>
#include <string>
#include <ctype.h>
#include <stdlib.h>
#include <malloc.h>

#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include <set>
#include <vector>

#include "../txn.h"
#include "../macros.h"
#include "../scopedperf.hh"
#include "../spinlock.h"

#include "bench.h"
#include "tpcc.h"
#include "lib/server.h"
#include "benchmarks/sto/Interface.hh"
#include "benchmarks/sto/Transaction.hh"
#include "common.h"
#include "deptran/s_main.h"
#include "benchmarks/sto/multiversion.hh"
#include "atomic"
#include <chrono>

using namespace std;
using namespace util;
using namespace srolis;

#define TPCC_TABLE_LIST(x) \
  x(customer) \
  x(customer_name_idx) \
  x(district) \
  x(history) \
  x(new_order) \
  x(oorder) \
  x(oorder_c_id_idx) \
  x(order_line) \
  x(stock) \
  x(stock_data) \
  x(warehouse) \
  x(item)

// several control parameters
static bool is_sampling_remote_calls = false;
static int sampling_number = 100;
static int f_mode = 0;
static std::vector<FastTransport *> server_transports;
static atomic<int> set_server_transport(0);

static inline ALWAYS_INLINE size_t 
NumWarehouses()
{
  return (size_t) scale_factor;  // scale_factor == nthreads
}

static inline ALWAYS_INLINE size_t 
NumWarehousesTotal()
{
  return  nshards * ((size_t) scale_factor);  // scale_factor == nthreads
}

static inline ALWAYS_INLINE size_t
ShardIndexFromGlobalWarehouse(int g_wid)
{
  return (g_wid-1) / NumWarehouses();
}

static inline ALWAYS_INLINE size_t
WarehouseLocal2Global(int l_wid) {
  return shardIndex * NumWarehouses() + l_wid;
}

static inline ALWAYS_INLINE size_t
WarehouseGlobal2Local(int g_wid) {
  return (g_wid-1) % NumWarehouses() + 1;
}

static inline ALWAYS_INLINE
void ALWAYS_ERROR(bool aa){
  if (likely(aa)){
  }else{
    if(TThread::transget_without_throw){
      //found the key, but invalid. 
      //  in the previous implementation, it throw an exception.
      //  but we don't want stack unwinding, just abort transaction

    }else{
      Panic("the error for ALWAYS ERROR!");
    }
  }
}

static inline ALWAYS_INLINE bool
WarehouseInShard(int g_w_id, int sIdx) {
  return g_w_id >= sIdx * NumWarehouses() + 1 && g_w_id <= sIdx * NumWarehouses() + NumWarehouses();
}
// #endif

// config constants

static constexpr inline ALWAYS_INLINE size_t
NumItems()
{
  return 100000;
}

static constexpr inline ALWAYS_INLINE size_t
NumDistrictsPerWarehouse()
{
  return 10;
}

static constexpr inline ALWAYS_INLINE size_t
NumCustomersPerDistrict()
{
  return 3000;
}

// T must implement lock()/unlock(). Both must *not* throw exceptions
template <typename T>
class scoped_multilock {
public:
  inline scoped_multilock()
    : did_lock(false)
  {
  }

  inline ~scoped_multilock()
  {
    if (did_lock)
      for (auto &t : locks)
        t->unlock();
  }

  inline void
  enq(T &t)
  {
    ALWAYS_ERROR(!did_lock);
    locks.emplace_back(&t);
  }

  inline void
  multilock()
  {
    ALWAYS_ERROR(!did_lock);
    if (locks.size() > 1)
      sort(locks.begin(), locks.end());
#ifdef CHECK_INVARIANTS
    if (set<T *>(locks.begin(), locks.end()).size() != locks.size()) {
      for (auto &t : locks)
        cerr << "lock: " << hexify(t) << endl;
      INVARIANT(false && "duplicate locks found");
    }
#endif
    for (auto &t : locks)
      t->lock();
    did_lock = true;
  }

private:
  bool did_lock;
  typename util::vec<T *, 64>::type locks;
};

// like a lock_guard, but has the option of not acquiring
template <typename T>
class scoped_lock_guard {
public:
  inline scoped_lock_guard(T &l)
    : l(&l)
  {
    this->l->lock();
  }

  inline scoped_lock_guard(T *l)
    : l(l)
  {
    if (this->l)
      this->l->lock();
  }

  inline ~scoped_lock_guard()
  {
    if (l)
      l->unlock();
  }

private:
  T *l;
};

// configuration flags
static int g_disable_xpartition_txn = 0;
static int g_disable_read_only_scans = 0;
static int g_enable_partition_locks = 0;
static int g_enable_separate_tree_per_partition = 0;
static int g_new_order_remote_item_pct = 1;  // remote ratio for txn_new_order
static int g_new_order_fast_id_gen = 1;
static int g_uniform_item_dist = 0;
static int g_order_status_scan_hack = 0;
#if defined(SIMPLE_WORKLOAD) || defined(MEGA_BENCHMARK)
static unsigned g_txn_workload_mix[] = { 100, 0, 0, 0, 0 };
#elif defined(SIMPLE_WORKLOAD) || defined(MEGA_BENCHMARK_MICRO)
static unsigned g_txn_workload_mix[] = { 50, 50, 0, 0, 0 };
#elif defined(MICRO_BENCHMARK)
static unsigned g_txn_workload_mix[] = { 50, 50, 0, 0, 0 };
#else
static unsigned g_txn_workload_mix[] = { 45, 43, 4, 4, 4 }; // default TPC-C workload mix
#endif
static aligned_padded_elem<spinlock> *g_partition_locks = nullptr;
static aligned_padded_elem<atomic<uint64_t>> *g_district_ids = nullptr;

// configuration microbenchmark
static int nkeys = 100 * 10000 ; // 1 million, TODO: it's better for in proportion to scale_factor
static const size_t g_micro_remote_item_pct = 5;
static const size_t RMW_count = 4; // the updated keys in a RMW transaction
static const bool drtm = false;
std::string intToString2(long long num) {
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << num;
    return ss.str();
}

long long getCurrentTimeMillis2() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// maps a wid => partition id
static inline ALWAYS_INLINE unsigned int
PartitionId(unsigned int wid)
{
  INVARIANT(wid >= 1 && wid <= NumWarehouses());
  wid -= 1; // 0-idx
  if (NumWarehouses() <= nthreads)
    // more workers than partitions, so its easy
    return wid;
  const unsigned nwhse_per_partition = NumWarehouses() / nthreads;
  const unsigned partid = wid / nwhse_per_partition;
  if (partid >= nthreads)
    return nthreads - 1;
  return partid;
}

static inline ALWAYS_INLINE spinlock &
LockForPartition(unsigned int wid)
{
  INVARIANT(g_enable_partition_locks);
  return g_partition_locks[PartitionId(wid)].elem;
}

static inline atomic<uint64_t> &
NewOrderIdHolder(unsigned warehouse, unsigned district)
{
  INVARIANT(warehouse >= 1 && warehouse <= NumWarehouses());
  INVARIANT(district >= 1 && district <= NumDistrictsPerWarehouse());
  const unsigned idx =
    (warehouse - 1) * NumDistrictsPerWarehouse() + (district - 1);
  return g_district_ids[idx].elem;
}

// SWH: (TODO) it's better to keep this information for taking over
static inline uint64_t
FastNewOrderIdGen(unsigned warehouse, unsigned district)
{
  return NewOrderIdHolder(warehouse, district).fetch_add(1, memory_order_acq_rel);
}

struct checker {
  // these sanity checks are just a few simple checks to make sure
  // the data is not entirely corrupted

  static inline ALWAYS_INLINE void
  SanityCheckCustomer(const customer::key *k, const customer::value *v)
  {
    INVARIANT(k->c_w_id >= 1 && static_cast<size_t>(k->c_w_id) <= NumWarehouses());
    INVARIANT(k->c_d_id >= 1 && static_cast<size_t>(k->c_d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(k->c_id >= 1 && static_cast<size_t>(k->c_id) <= NumCustomersPerDistrict());
    INVARIANT(v->c_credit == "BC" || v->c_credit == "GC");
    INVARIANT(v->c_middle == "OE");
  }

  static inline ALWAYS_INLINE void
  SanityCheckWarehouse(const warehouse::key *k, const warehouse::value *v)
  {
    INVARIANT(k->w_id >= 1 && static_cast<size_t>(k->w_id) <= NumWarehouses());
    INVARIANT(v->w_state.size() == 2);
    INVARIANT(v->w_zip == "123456789");
  }

  static inline ALWAYS_INLINE void
  SanityCheckDistrict(const district::key *k, const district::value *v)
  {
    INVARIANT(k->d_w_id >= 1 && static_cast<size_t>(k->d_w_id) <= NumWarehouses());
    INVARIANT(k->d_id >= 1 && static_cast<size_t>(k->d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(v->d_next_o_id >= 3001);
    INVARIANT(v->d_state.size() == 2);
    INVARIANT(v->d_zip == "123456789");
  }

  static inline ALWAYS_INLINE void
  SanityCheckItem(const item::key *k, const item::value *v)
  {
    INVARIANT(k->i_id >= 1 && static_cast<size_t>(k->i_id) <= NumItems());
    INVARIANT(v->i_price >= 1.0 && v->i_price <= 100.0);
  }

  static inline ALWAYS_INLINE void
  SanityCheckStock(const stock::key *k, const stock::value *v)
  {
    INVARIANT(k->s_w_id >= 1 && static_cast<size_t>(k->s_w_id) <= NumWarehouses());
    INVARIANT(k->s_i_id >= 1 && static_cast<size_t>(k->s_i_id) <= NumItems());
  }

  static inline ALWAYS_INLINE void
  SanityCheckNewOrder(const new_order::key *k, const new_order::value *v)
  {
    INVARIANT(k->no_w_id >= 1 && static_cast<size_t>(k->no_w_id) <= NumWarehouses());
    INVARIANT(k->no_d_id >= 1 && static_cast<size_t>(k->no_d_id) <= NumDistrictsPerWarehouse());
  }

  static inline ALWAYS_INLINE void
  SanityCheckOOrder(const oorder::key *k, const oorder::value *v)
  {
    INVARIANT(k->o_w_id >= 1 && static_cast<size_t>(k->o_w_id) <= NumWarehouses());
    INVARIANT(k->o_d_id >= 1 && static_cast<size_t>(k->o_d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(v->o_c_id >= 1 && static_cast<size_t>(v->o_c_id) <= NumCustomersPerDistrict());
    INVARIANT(v->o_carrier_id >= 0 && static_cast<size_t>(v->o_carrier_id) <= NumDistrictsPerWarehouse());
    INVARIANT(v->o_ol_cnt >= 5 && v->o_ol_cnt <= 15);
  }

  static inline ALWAYS_INLINE void
  SanityCheckOrderLine(const order_line::key *k, const order_line::value *v)
  {
    INVARIANT(k->ol_w_id >= 1 && static_cast<size_t>(k->ol_w_id) <= NumWarehouses());
    INVARIANT(k->ol_d_id >= 1 && static_cast<size_t>(k->ol_d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(k->ol_number >= 1 && k->ol_number <= 15);
    INVARIANT(v->ol_i_id >= 1 && static_cast<size_t>(v->ol_i_id) <= NumItems());
  }

};


struct _dummy {}; // exists so we can inherit from it, so we can use a macro in
                  // an init list...

class tpcc_worker_mixin : private _dummy {

#define DEFN_TBL_INIT_X(name) \
  , tbl_ ## name ## _vec(partitions.at(#name)) \
  , dummy_tbl_ ## name ## _dummy_vec(dummy_partitions.at(#name))

public:
  tpcc_worker_mixin(const map<string, vector<abstract_ordered_index *>> &partitions,
                    const map<string, vector<abstract_ordered_index *>> &dummy_partitions) :
    _dummy() // so hacky...
    TPCC_TABLE_LIST(DEFN_TBL_INIT_X)
  {
    ALWAYS_ERROR(NumWarehouses() >= 1);
  }

#undef DEFN_TBL_INIT_X

protected:

#define DEFN_TBL_ACCESSOR_X(name) \
private:  \
  vector<abstract_ordered_index *> tbl_ ## name ## _vec; \
  vector<abstract_ordered_index *> dummy_tbl_ ## name ## _dummy_vec; \
protected: \
  inline ALWAYS_INLINE abstract_ordered_index * \
  tbl_ ## name (unsigned int wid) \
  { \
    INVARIANT(wid >= 1 && wid <= NumWarehouses()); \
    INVARIANT(tbl_ ## name ## _vec.size() == NumWarehouses()); \
    return tbl_ ## name ## _vec[wid - 1]; \
  } \
  inline ALWAYS_INLINE abstract_ordered_index * \
  dummy_tbl_ ## name (unsigned int wid) \
  { \
    return dummy_tbl_ ## name ## _dummy_vec[wid - 1]; \
  }

  TPCC_TABLE_LIST(DEFN_TBL_ACCESSOR_X)

#undef DEFN_TBL_ACCESSOR_X

  // only TPCC loaders need to call this- workers are automatically
  // pinned by their worker id (which corresponds to warehouse id
  // in TPCC)
  //
  // pins the *calling* thread
  static void
  PinToWarehouseId(unsigned int wid)
  {
    const unsigned int partid = PartitionId(wid);
    ALWAYS_ERROR(partid < nthreads);
    const unsigned int pinid  = partid;
    if (verbose)
      cerr << "PinToWarehouseId(): coreid=" << coreid::core_id()
           << " pinned to whse=" << wid << " (partid=" << partid << ")"
           << endl;
    rcu::s_instance.pin_current_thread(pinid);
    rcu::s_instance.fault_region();
  }

public:

  static inline uint32_t
  GetCurrentTimeMillis()
  {
    //struct timeval tv;
    //ALWAYS_ERROR(gettimeofday(&tv, 0) == 0);
    //return tv.tv_sec * 1000;

    // XXX(stephentu): implement a scalable GetCurrentTimeMillis()
    // for now, we just give each core an increasing number

    static __thread uint32_t tl_hack = 0;
    return tl_hack++;
  }

  // utils for generating random #s and strings

  static inline ALWAYS_INLINE int
  CheckBetweenInclusive(int v, int lower, int upper)
  {
    INVARIANT(v >= lower);
    INVARIANT(v <= upper);
    return v;
  }

  static inline ALWAYS_INLINE int
  RandomNumber(fast_random &r, int min, int max)
  {
    return CheckBetweenInclusive((int) (r.next_uniform() * (max - min + 1) + min), min, max);
  }

  static inline ALWAYS_INLINE int
  NonUniformRandom(fast_random &r, int A, int C, int min, int max)
  {
    return (((RandomNumber(r, 0, A) | RandomNumber(r, min, max)) + C) % (max - min + 1)) + min;
  }

  static inline ALWAYS_INLINE int
  GetItemId(fast_random &r)
  {
    return CheckBetweenInclusive(
        g_uniform_item_dist ?
          RandomNumber(r, 1, NumItems()) :
          NonUniformRandom(r, 8191, 7911, 1, NumItems()),
        1, NumItems());
  }

  static inline ALWAYS_INLINE int
  GetCustomerId(fast_random &r)
  {
    return CheckBetweenInclusive(NonUniformRandom(r, 1023, 259, 1, NumCustomersPerDistrict()), 1, NumCustomersPerDistrict());
  }

  // pick a number between [start, end)
  static inline ALWAYS_INLINE unsigned
  PickWarehouseId(fast_random &r, unsigned start, unsigned end)
  {
    INVARIANT(start < end);
    const unsigned diff = end - start;
    if (diff == 1)
      return start;
    return (r.next() % diff) + start;
  }

  static string NameTokens[];

  // all tokens are at most 5 chars long
  static const size_t CustomerLastNameMaxSize = 5 * 3;

  static inline size_t
  GetCustomerLastName(uint8_t *buf, fast_random &r, int num)
  {
    const string &s0 = NameTokens[num / 100];
    const string &s1 = NameTokens[(num / 10) % 10];
    const string &s2 = NameTokens[num % 10];
    uint8_t *const begin = buf;
    const size_t s0_sz = s0.size();
    const size_t s1_sz = s1.size();
    const size_t s2_sz = s2.size();
    NDB_MEMCPY(buf, s0.data(), s0_sz); buf += s0_sz;
    NDB_MEMCPY(buf, s1.data(), s1_sz); buf += s1_sz;
    NDB_MEMCPY(buf, s2.data(), s2_sz); buf += s2_sz;
    return buf - begin;
  }

  static inline ALWAYS_INLINE size_t
  GetCustomerLastName(char *buf, fast_random &r, int num)
  {
    return GetCustomerLastName((uint8_t *) buf, r, num);
  }

  static inline string
  GetCustomerLastName(fast_random &r, int num)
  {
    string ret;
    ret.resize(CustomerLastNameMaxSize);
    ret.resize(GetCustomerLastName((uint8_t *) &ret[0], r, num));
    return ret;
  }

  static inline ALWAYS_INLINE string
  GetNonUniformCustomerLastNameLoad(fast_random &r)
  {
    return GetCustomerLastName(r, NonUniformRandom(r, 255, 157, 0, 999));
  }

  static inline ALWAYS_INLINE size_t
  GetNonUniformCustomerLastNameRun(uint8_t *buf, fast_random &r)
  {
    return GetCustomerLastName(buf, r, NonUniformRandom(r, 255, 223, 0, 999));
  }

  static inline ALWAYS_INLINE size_t
  GetNonUniformCustomerLastNameRun(char *buf, fast_random &r)
  {
    return GetNonUniformCustomerLastNameRun((uint8_t *) buf, r);
  }

  static inline ALWAYS_INLINE string
  GetNonUniformCustomerLastNameRun(fast_random &r)
  {
    return GetCustomerLastName(r, NonUniformRandom(r, 255, 223, 0, 999));
  }

  // following oltpbench, we really generate strings of len - 1...
  static inline string
  RandomStr(fast_random &r, uint len)
  {
    // this is a property of the oltpbench implementation...
    if (!len)
      return "";

    uint i = 0;
    string buf(len - 1, 0);
    while (i < (len - 1)) {
      const char c = (char) r.next_char();
      // XXX(stephentu): oltpbench uses java's Character.isLetter(), which
      // is a less restrictive filter than isalnum()
      if (!isalnum(c))
        continue;
      buf[i++] = c;
    }
    return buf;
  }

  // RandomNStr() actually produces a string of length len
  static inline string
  RandomNStr(fast_random &r, uint len)
  {
    const char base = '0';
    string buf(len, 0);
    for (uint i = 0; i < len; i++)
      buf[i] = (char)(base + (r.next() % 10));
    return buf;
  }
};

string tpcc_worker_mixin::NameTokens[] =
  {
    string("BAR"),
    string("OUGHT"),
    string("ABLE"),
    string("PRI"),
    string("PRES"),
    string("ESE"),
    string("ANTI"),
    string("CALLY"),
    string("ATION"),
    string("EING"),
  };

STATIC_COUNTER_DECL(scopedperf::tsc_ctr, tpcc_txn, tpcc_txn_cg)

template <typename KeyType, typename ValueType>
class generic_scan_callback: public abstract_ordered_index::scan_callback {
public:
  generic_scan_callback()
  { 
    values.reserve(10);
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const std::string &value)
  {
    std::string kk = std::string(keyp, keylen);
    KeyType k_temp;
    const KeyType *k_temp_a = Decode(kk, k_temp);
    KeyType k_new(*k_temp_a);

    ValueType v_temp;
    const ValueType *v_temp_a = Decode(value, v_temp);
    ValueType v_new(*v_temp_a);

    if (values.size()>=10) { return false; }
    values.emplace_back(std::make_pair(k_new, v_new));
    return true;
  }

  inline void print_warehouse_info() {
    std::cout << "# of records in the warehouse table: " << values.size() << std::endl;
    for (int i=0; i<values.size(); i++) {
      std::cout << "  " << "id: " << ((warehouse::key)(values[i].first)).w_id << std::endl;
    }
    std::cout << std::endl;
  }

  inline int64_t print_customer_info() {
    std::cout << "# of records in the customer table: " << values.size() << std::endl;
    int64_t total_balance = 0;
    int total_remote_udpate = 0;
    for (int i=0; i<values.size(); i++) {
        std::cout << "  " << "key: " << ((customer::key)(values[i].first))
                  << ", balance: " <<((customer::value)(values[i].second)).c_payment_cnt << std::endl;
      total_balance += ((customer::value)(values[i].second)).c_payment_cnt;
      total_remote_udpate += ((customer::value)(values[i].second)).c_delivery_cnt;
    }
    std::cout << "  " << "total balance is " << total_balance;
    std::cout << "  " << "total remote update is " << total_remote_udpate;
    std::cout << std::endl;
    return total_balance;
  }

  inline int64_t print_oorder_info() {
    std::cout << "# of records in the oorder table: " << values.size() << std::endl;
    int64_t total_cost = 0;
    for (int i=0; i<values.size(); i++) {
      std::cout << "  " << "key: " << ((oorder::key)(values[i].first))
                << ", item-id: " <<((oorder::value)(values[i].second)).o_c_id 
                << ", customer-id: " <<((oorder::value)(values[i].second)).o_carrier_id 
                << ", value: " << (oorder::value)(values[i].second)
                << std::endl;
      total_cost += ((oorder::value)(values[i].second)).o_c_id;
    }
    std:: cout << "  " << "total cost is " << total_cost;
    std::cout << std::endl;
    return total_cost;
  }

  inline size_t
  size() const
  {
    return values.size();
  }

private:
  vector<std::pair<KeyType, ValueType>> values;
};

class tpcc_worker : public bench_worker, public tpcc_worker_mixin {
public:
  // resp for [warehouse_id_start, warehouse_id_end)
  tpcc_worker(unsigned int worker_id,
              unsigned long seed, abstract_db *db,
              const map<string, abstract_ordered_index *> &open_tables,
              const map<string, vector<abstract_ordered_index *>> &partitions,
              const map<string, vector<abstract_ordered_index *>> &dummy_partitions,
              spin_barrier *barrier_a, spin_barrier *barrier_b,
              uint warehouse_id_start, uint warehouse_id_end)
    : bench_worker(worker_id, true, seed, db,
                   open_tables, barrier_a, barrier_b),
      tpcc_worker_mixin(partitions,dummy_partitions),
      warehouse_id_start(warehouse_id_start),
      warehouse_id_end(warehouse_id_end)
  {
    INVARIANT(warehouse_id_start >= 1);
    INVARIANT(warehouse_id_start <= NumWarehouses());
    INVARIANT(warehouse_id_end > warehouse_id_start);
    INVARIANT(warehouse_id_end <= (NumWarehouses() + 1));
    NDB_MEMSET(&last_no_o_ids[0], 0, sizeof(last_no_o_ids));
    cerr << "tpcc: worker id " << worker_id
        << " => warehouses [" << warehouse_id_start
        << ", " << warehouse_id_end << ")"
        << endl;
    obj_key0.reserve(str_arena::MinStrReserveLength);
    obj_key1.reserve(str_arena::MinStrReserveLength);
    obj_v.reserve(str_arena::MinStrReserveLength);

    local_oorder_id = 0;
    counter_new_order_failed = 0;
    counter_payment_failed = 0;
  }

  // ~tpcc_worker() {
  //   std::cout<<"worker "<<worker_id<<" new order failed "<<counter_new_order_failed<<std::endl;
  //   std::cout<<"worker "<<worker_id<<" payment failed "<<counter_payment_failed<<std::endl;
  // }

  // XXX(stephentu): tune this
  static const size_t NMaxCustomerIdxScanElems = 512;

  void scan_entire_warehouses(int);
  txn_result txn_new_order();
  txn_result txn_new_order_mega();
  txn_result txn_new_order_simple();
  txn_result txn_new_order_micro();
  txn_result txn_new_order_micro_mega();
  txn_result txn_new_order_micro_drtm();

  static txn_result
  TxnNewOrder(bench_worker *w)
  {
    // if (shardIndex == 1) { return txn_result(true, 0); }
    ANON_REGION("TxnNewOrder:", &tpcc_txn_cg);
  #if defined(MEGA_BENCHMARK)
    return static_cast<tpcc_worker *>(w)->txn_new_order_mega();
  #elif defined(MEGA_BENCHMARK_MICRO)
    return static_cast<tpcc_worker *>(w)->txn_new_order_micro_mega();
  #else
    return static_cast<tpcc_worker *>(w)->txn_new_order();
  #endif
  }

  txn_result txn_delivery();

  static txn_result
  TxnDelivery(bench_worker *w)
  {
    //if (shardIndex == 1) { return txn_result(true, 0); }
    ANON_REGION("TxnDelivery:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_delivery();
  }

  txn_result txn_payment();
  txn_result txn_payment_micro();
  txn_result txn_payment_micro_mega();
  txn_result txn_payment_micro_drtm();

  static txn_result
  TxnPayment(bench_worker *w)
  {
    //if (shardIndex == 1) { return txn_result(true, 0); }
    ANON_REGION("TxnPayment:", &tpcc_txn_cg);
  #if defined(MEGA_BENCHMARK_MICRO)
    return static_cast<tpcc_worker *>(w)->txn_payment_micro_mega();
  #else
    return static_cast<tpcc_worker *>(w)->txn_payment();
  #endif
  }

  txn_result txn_order_status();

  static txn_result
  TxnOrderStatus(bench_worker *w)
  {
    ANON_REGION("TxnOrderStatus:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_order_status();
  }

  txn_result txn_stock_level();

  static txn_result
  TxnStockLevel(bench_worker *w)
  {
    ANON_REGION("TxnStockLevel:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_stock_level();
  }

  virtual workload_desc_vec
  get_workload() const
  {
    workload_desc_vec w;

#if defined(SIMPLE_WORKLOAD)
    w.push_back(workload_desc("NewOrder", 1.0, TxnNewOrder));
#else
    unsigned m = 0;
    for (size_t i = 0; i < ARRAY_NELEMS(g_txn_workload_mix); i++)
      m += g_txn_workload_mix[i];
    ALWAYS_ERROR(m == 100);
    if (g_txn_workload_mix[0])
      w.push_back(workload_desc("NewOrder", double(g_txn_workload_mix[0])/100.0, TxnNewOrder));
    if (g_txn_workload_mix[1])
      w.push_back(workload_desc("Payment", double(g_txn_workload_mix[1])/100.0, TxnPayment));
    if (g_txn_workload_mix[2])
      w.push_back(workload_desc("Delivery", double(g_txn_workload_mix[2])/100.0, TxnDelivery));
    if (g_txn_workload_mix[3])
      w.push_back(workload_desc("OrderStatus", double(g_txn_workload_mix[3])/100.0, TxnOrderStatus));
    if (g_txn_workload_mix[4])
      w.push_back(workload_desc("StockLevel", double(g_txn_workload_mix[4])/100.0, TxnStockLevel));
#endif
    return w;
  }

protected:

  virtual void
  on_run_setup() OVERRIDE
  {
    if (!pin_cpus)
      return;
    const size_t a = worker_id % coreid::num_cpus_online();
    const size_t b = a % nthreads;
    rcu::s_instance.pin_current_thread(b);
    rcu::s_instance.fault_region();

    //warmup connections on learner-0 (assume the leader-0 will be killed)
#if defined(PAXOS_LIB_ENABLED)
    if (clusterRole==srolis::LOCALHOST_CENTER_INT){ // disable it if 10 shards
      #if !defined(MEGA_BENCHMARK)
      for (int i=0;i<=100;i++) {
         ///*printf("start - pid:%d-i:%d", TThread::getPartitionID(), i);
         //std::cout<<std::endl;
         //uint32_t dstShardIndex=0;
         //for (int i=0; i<nshards; i++) {
         //  dstShardIndex |= (1 << i);
         //}*/
        uint32_t dstShardIndex=(1<<0);
        std::vector<uint32_t> ret_values(nshards);
        uint32_t req_val = i;
        TThread::sclient->warmupRequest(req_val, srolis::LEARNER_CENTER_INT, ret_values, dstShardIndex);
        int ret=0;
        for (int j=0;j<nshards;j++)
          ret += ret_values[j];
      }
      Warning("DONE a warmup on leader:%d to the learner-0\n", TThread::getPartitionID());
      #endif
    }
#endif
  }

  inline ALWAYS_INLINE string &
  str()
  {
    return *arena.next();
  }

private:
  const uint warehouse_id_start;
  const uint warehouse_id_end;
  int32_t last_no_o_ids[10]; // XXX(stephentu): hack

  // some scratch buffer space
  string obj_key0;
  string obj_key1;
  string obj_v;

  int counter_new_order_failed;
  int counter_payment_failed;

  int32_t local_oorder_id;
};

class tpcc_warehouse_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_warehouse_loader(unsigned long seed,
                        abstract_db *db,
                        const map<string, abstract_ordered_index *> &open_tables,
                        const map<string, vector<abstract_ordered_index *>> &partitions,
                        const map<string, vector<abstract_ordered_index *>> &dummy_partitions)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(partitions,dummy_partitions)
  {}

protected:
 
  void load_simple() {
    // load warehouses
    string obj_buf;
    void *txn = db->new_txn(txn_flags, arena, txn_buf());
    try {
      for (uint i = 1; i <= NumWarehouses(); i++) {
        const warehouse::key k(i);
        warehouse::value v;
        tbl_warehouse(i)->insert(txn, Encode(k), Encode(obj_buf, v));
      }
      ALWAYS_ERROR(db->commit_txn(txn));

      // load 10 customers per warehouse
      for (uint w_id = 1; w_id <= NumWarehouses(); w_id++)
        for (uint c_id = 1; c_id <= 10; c_id++) {
          arena.reset();
          void *txn_1 = db->new_txn(txn_flags, arena, txn_buf());
          const customer::key k(w_id, 1, c_id);
          customer::value v;
          v.c_payment_cnt = 10000 * 10000;
          v.c_delivery_cnt = 0;
          tbl_customer(w_id)->insert(txn_1, Encode(k), Encode(obj_buf, v));
          ALWAYS_ERROR(db->commit_txn(txn_1));
        }
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ERROR(false);
    }
  }

  // load the micro-benchmark workload (no parallel load)
  void load_micro() {
    const size_t batchsize = 10000;
    uint64_t keystart = 0;
    uint64_t keyend = nkeys;

    const size_t nbatches = nkeys < batchsize ? 1 : (nkeys / batchsize);
    string obj_buf;

    const string i_name = "cccccccc";
    for (size_t batchid = 0; batchid < nbatches;) {
      scoped_str_arena s_arena(arena);
      void * const txn = db->new_txn(txn_flags, arena, txn_buf());
      try {
        const size_t rend = (batchid + 1 == nbatches) ?
          keyend : keystart + ((batchid + 1) * batchsize);
        for (size_t i = batchid * batchsize + keystart; i < rend; i++) {
          const item_micro::key k(i);
          item_micro::value v;
          v.i_name.assign(i_name);
          tbl_item(1)->insert(txn, EncodeK(k), Encode(obj_buf, v));
        }
        Notice("[load] micro-benchmark insert: [%d,%d]",batchid * batchsize + keystart, rend);
        if (db->commit_txn(txn))
          batchid++;
        else
          db->abort_txn(txn);
      } catch (abstract_db::abstract_abort_exception &ex) {
        db->abort_txn(txn);
      }
    }
  }

  // load tpcc-benchmark workload for the warehouse section
  virtual void
  load()
  {
#if defined(SIMPLE_WORKLOAD)
    load_simple();
#elif defined(MICRO_BENCHMARK)
    load_micro();
#else
    string obj_buf;
    void *txn = db->new_txn(txn_flags, arena, txn_buf());
    uint64_t warehouse_total_sz = 0, n_warehouses = 0;
    try {
      vector<warehouse::value> warehouses;
      for (uint i = 1; i <= NumWarehouses(); i++) {
        const warehouse::key k(i);

        const string w_name = RandomStr(r, RandomNumber(r, 6, 10));
        const string w_street_1 = RandomStr(r, RandomNumber(r, 10, 20));
        const string w_street_2 = RandomStr(r, RandomNumber(r, 10, 20));
        const string w_city = RandomStr(r, RandomNumber(r, 10, 20));
        const string w_state = RandomStr(r, 3);
        const string w_zip = "123456789";

        warehouse::value v;
        v.w_ytd = 300000;
        v.w_tax = (float) RandomNumber(r, 0, 2000) / 10000.0;
        v.w_name.assign(w_name);
        v.w_street_1.assign(w_street_1);
        v.w_street_2.assign(w_street_2);
        v.w_city.assign(w_city);
        v.w_state.assign(w_state);
        v.w_zip.assign(w_zip);

        checker::SanityCheckWarehouse(&k, &v);
        const size_t sz = Size(v);
        warehouse_total_sz += sz;
        n_warehouses++;
        tbl_warehouse(i)->insert(txn, Encode(k), Encode(obj_buf, v));

        warehouses.push_back(v);
      }
      ALWAYS_ERROR(db->commit_txn(txn));
      arena.reset();
      txn = db->new_txn(txn_flags, arena, txn_buf());
      for (uint i = 1; i <= NumWarehouses(); i++) {
        const warehouse::key k(i);
        string warehouse_v;
        // TODO: for the decoder, it should get all the timestamps as well
        // let's use fetch_and_add!!!!
        //   but the problem here is add more code to obtain the fetch_and_add
        ALWAYS_ERROR(tbl_warehouse(i)->get(txn, Encode(k), warehouse_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return;}
        warehouse::value warehouse_temp;
        const warehouse::value *v = Decode(warehouse_v, warehouse_temp);
        ALWAYS_ERROR(warehouses[i - 1] == *v);

        checker::SanityCheckWarehouse(&k, v);
      }
      ALWAYS_ERROR(db->commit_txn(txn));
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ERROR(false);
    }
    if (verbose) {
      cerr << "[INFO] finished loading warehouse" << endl;
      cerr << "[INFO]   * average warehouse record length: "
           << (double(warehouse_total_sz)/double(n_warehouses)) << " bytes" << endl;
    }
#endif
  }
};

class tpcc_item_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_item_loader(unsigned long seed,
                   abstract_db *db,
                   const map<string, abstract_ordered_index *> &open_tables,
                   const map<string, vector<abstract_ordered_index *>> &partitions,
                   const map<string, vector<abstract_ordered_index *>> &dummy_partitions)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(partitions,dummy_partitions)
  {}

protected:
  virtual void
  load()
  {
    string obj_buf;
    const ssize_t bsize = db->txn_max_batch_size();
    void *txn = db->new_txn(txn_flags, arena, txn_buf());
    uint64_t total_sz = 0;
    try {
      for (uint i = 1; i <= NumItems(); i++) {
        // items don't "belong" to a certain warehouse, so no pinning
        const item::key k(i);

        item::value v;
        const string i_name = RandomStr(r, RandomNumber(r, 14, 24));
        v.i_name.assign(i_name);
        v.i_price = (float) RandomNumber(r, 100, 10000) / 100.0;
        const int len = RandomNumber(r, 26, 50);
        if (RandomNumber(r, 1, 100) > 10) {
          const string i_data = RandomStr(r, len);
          v.i_data.assign(i_data);
        } else {
          const int startOriginal = RandomNumber(r, 2, (len - 8));
          const string i_data = RandomStr(r, startOriginal + 1) + "ORIGINAL" + RandomStr(r, len - startOriginal - 7);
          v.i_data.assign(i_data);
        }
        v.i_im_id = RandomNumber(r, 1, 10000);

        checker::SanityCheckItem(&k, &v);
        const size_t sz = Size(v);
        total_sz += sz;
        tbl_item(1)->insert(txn, EncodeK(k), Encode(obj_buf, v)); // this table is shared, so any partition is OK

        if (bsize != -1 && !(i % bsize)) {
          ALWAYS_ERROR(db->commit_txn(txn));
          txn = db->new_txn(txn_flags, arena, txn_buf());
          arena.reset();
        }
      }
      ALWAYS_ERROR(db->commit_txn(txn));
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ERROR(false);
    }
    if (verbose) {
      cerr << "[INFO] finished loading item" << endl;
      cerr << "[INFO]   * average item record length: "
           << (double(total_sz)/double(NumItems())) << " bytes" << endl;
    }
  }
};

class tpcc_stock_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_stock_loader(unsigned long seed,
                    abstract_db *db,
                    const map<string, abstract_ordered_index *> &open_tables,
                    const map<string, vector<abstract_ordered_index *>> &partitions,const map<string, vector<abstract_ordered_index *>> &dummy_partitions,
                    ssize_t warehouse_id)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(partitions,dummy_partitions),
      warehouse_id(warehouse_id)
  {
    ALWAYS_ERROR(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

protected:
  virtual void
  load()
  {
    string obj_buf, obj_buf1;

    uint64_t stock_total_sz = 0, n_stocks = 0;
    const uint w_start = (warehouse_id == -1) ?
      1 : static_cast<uint>(warehouse_id);
    const uint w_end   = (warehouse_id == -1) ?
      NumWarehouses() : static_cast<uint>(warehouse_id);

    for (uint w = w_start; w <= w_end; w++) {
      const size_t batchsize =
        (db->txn_max_batch_size() == -1) ? NumItems() : db->txn_max_batch_size();
      const size_t nbatches = (batchsize > NumItems()) ? 1 : (NumItems() / batchsize);

      if (pin_cpus)
        PinToWarehouseId(w);

      for (uint b = 0; b < nbatches;) {
        scoped_str_arena s_arena(arena);
        void * const txn = db->new_txn(txn_flags, arena, txn_buf());
        try {
          const size_t iend = std::min((b + 1) * batchsize + 1, NumItems());
          //Warning("insert stock: w:%d, range:(%d-%d)",w,(b * batchsize + 1),iend);
          for (uint i = (b * batchsize + 1); i <= iend; i++) {
            const stock::key k(w, i);
            const stock_data::key k_data(w, i);

            stock::value v;
            v.s_quantity = RandomNumber(r, 10, 100);
            v.s_ytd = 0;
            v.s_order_cnt = 0;
            v.s_remote_cnt = 0;

            stock_data::value v_data;
            const int len = RandomNumber(r, 26, 50);
            if (RandomNumber(r, 1, 100) > 10) {
              const string s_data = RandomStr(r, len);
              v_data.s_data.assign(s_data);
            } else {
              const int startOriginal = RandomNumber(r, 2, (len - 8));
              const string s_data = RandomStr(r, startOriginal + 1) + "ORIGINAL" + RandomStr(r, len - startOriginal - 7);
              v_data.s_data.assign(s_data);
            }
            v_data.s_dist_01.assign(RandomStr(r, 24));
            v_data.s_dist_02.assign(RandomStr(r, 24));
            v_data.s_dist_03.assign(RandomStr(r, 24));
            v_data.s_dist_04.assign(RandomStr(r, 24));
            v_data.s_dist_05.assign(RandomStr(r, 24));
            v_data.s_dist_06.assign(RandomStr(r, 24));
            v_data.s_dist_07.assign(RandomStr(r, 24));
            v_data.s_dist_08.assign(RandomStr(r, 24));
            v_data.s_dist_09.assign(RandomStr(r, 24));
            v_data.s_dist_10.assign(RandomStr(r, 24));

            checker::SanityCheckStock(&k, &v);
            const size_t sz = Size(v);
            stock_total_sz += sz;
            n_stocks++;
            tbl_stock(w)->insert(txn, EncodeK(k), Encode(obj_buf, v));
            tbl_stock_data(w)->insert(txn, EncodeK(k_data), Encode(obj_buf1, v_data));
          }
          if (db->commit_txn(txn)) {
            b++;
          } else {
            db->abort_txn(txn);
            if (verbose)
              cerr << "[WARNING] stock loader loading abort" << endl;
          }
        } catch (abstract_db::abstract_abort_exception &ex) {
          db->abort_txn(txn);
          ALWAYS_ERROR(warehouse_id != -1);
          if (verbose)
            cerr << "[WARNING] stock loader loading abort" << endl;
        }
      }
    }

    if (verbose) {
      if (warehouse_id == -1) {
        cerr << "[INFO] finished loading stock" << endl;
        cerr << "[INFO]   * average stock record length: "
             << (double(stock_total_sz)/double(n_stocks)) << " bytes" << endl;
      } else {
        cerr << "[INFO] finished loading stock (w=" << warehouse_id << ")" << endl;
      }
    }
  }

private:
  ssize_t warehouse_id;
};

class tpcc_district_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_district_loader(unsigned long seed,
                       abstract_db *db,
                       const map<string, abstract_ordered_index *> &open_tables,
                       const map<string, vector<abstract_ordered_index *>> &partitions,
                       const map<string, vector<abstract_ordered_index *>> &dummy_partitions)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(partitions,dummy_partitions)
  {}

protected:
  virtual void
  load()
  {
    string obj_buf;

    const ssize_t bsize = db->txn_max_batch_size();
    void *txn = db->new_txn(txn_flags, arena, txn_buf());
    uint64_t district_total_sz = 0, n_districts = 0;
    try {
      uint cnt = 0;
      for (uint w = 1; w <= NumWarehouses(); w++) {
        if (pin_cpus)
          PinToWarehouseId(w);
        for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++, cnt++) {
          const district::key k(w, d);

          district::value v;
          v.d_ytd = 30000;
          v.d_tax = (float) (RandomNumber(r, 0, 2000) / 10000.0);
          v.d_next_o_id = 3001;
          v.d_name.assign(RandomStr(r, RandomNumber(r, 6, 10)));
          v.d_street_1.assign(RandomStr(r, RandomNumber(r, 10, 20)));
          v.d_street_2.assign(RandomStr(r, RandomNumber(r, 10, 20)));
          v.d_city.assign(RandomStr(r, RandomNumber(r, 10, 20)));
          v.d_state.assign(RandomStr(r, 3));
          v.d_zip.assign("123456789");

          checker::SanityCheckDistrict(&k, &v);
          const size_t sz = Size(v);
          district_total_sz += sz;
          n_districts++;
          tbl_district(w)->insert(txn, Encode(k), Encode(obj_buf, v));

          if (bsize != -1 && !((cnt + 1) % bsize)) {
            ALWAYS_ERROR(db->commit_txn(txn));
            txn = db->new_txn(txn_flags, arena, txn_buf());
            arena.reset();
          }
        }
      }
      ALWAYS_ERROR(db->commit_txn(txn));
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ERROR(false);
    }
    if (verbose) {
      cerr << "[INFO] finished loading district" << endl;
      cerr << "[INFO]   * average district record length: "
           << (double(district_total_sz)/double(n_districts)) << " bytes" << endl;
    }
  }
};

class tpcc_customer_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_customer_loader(unsigned long seed,
                       abstract_db *db,
                       const map<string, abstract_ordered_index *> &open_tables,
                       const map<string, vector<abstract_ordered_index *>> &partitions,
                       const map<string, vector<abstract_ordered_index *>> &dummy_partitions,
                       ssize_t warehouse_id)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(partitions,dummy_partitions),
      warehouse_id(warehouse_id)
  {
    ALWAYS_ERROR(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

protected:
  virtual void
  load()
  {
    string obj_buf;

    const uint w_start = (warehouse_id == -1) ?
      1 : static_cast<uint>(warehouse_id);
    const uint w_end   = (warehouse_id == -1) ?
      NumWarehouses() : static_cast<uint>(warehouse_id);
    const size_t batchsize =
      (db->txn_max_batch_size() == -1) ?
        NumCustomersPerDistrict() : db->txn_max_batch_size();
    const size_t nbatches =
      (batchsize > NumCustomersPerDistrict()) ?
        1 : (NumCustomersPerDistrict() / batchsize);
    cerr << "num batches: " << nbatches << endl;

    uint64_t total_sz = 0;

    for (uint w = w_start; w <= w_end; w++) {
      if (pin_cpus)
        PinToWarehouseId(w);
      for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
        for (uint batch = 0; batch < nbatches;) {
          scoped_str_arena s_arena(arena);
          void * const txn = db->new_txn(txn_flags, arena, txn_buf());
          const size_t cstart = batch * batchsize;
          const size_t cend = std::min((batch + 1) * batchsize, NumCustomersPerDistrict());
          try {
            for (uint cidx0 = cstart; cidx0 < cend; cidx0++) {
              const uint c = cidx0 + 1;
              const customer::key k(w, d, c);
              const customer::key k_balance(w, d + 200, c);

              customer::value v;
              customer_balance::value v_balance;
              v.c_discount = (float) (RandomNumber(r, 1, 5000) / 10000.0);
              if (RandomNumber(r, 1, 100) <= 10)
                v.c_credit.assign("BC");
              else
                v.c_credit.assign("GC");

              if (c <= 1000)
                v.c_last.assign(GetCustomerLastName(r, c - 1));
              else
                v.c_last.assign(GetNonUniformCustomerLastNameLoad(r));

              v.c_first.assign(RandomStr(r, RandomNumber(r, 8, 16)));
              v.c_credit_lim = 50000;

              v.c_balance = -10;
              v_balance.c_balance = -10;
              v.c_ytd_payment = 10;
              v.c_payment_cnt = 1;
              v.c_delivery_cnt = 0;

              v.c_street_1.assign(RandomStr(r, RandomNumber(r, 10, 20)));
              v.c_street_2.assign(RandomStr(r, RandomNumber(r, 10, 20)));
              v.c_city.assign(RandomStr(r, RandomNumber(r, 10, 20)));
              v.c_state.assign(RandomStr(r, 3));
              v.c_zip.assign(RandomNStr(r, 4) + "11111");
              v.c_phone.assign(RandomNStr(r, 16));
              v.c_since = GetCurrentTimeMillis();
              v.c_middle.assign("OE");

              checker::SanityCheckCustomer(&k, &v);
              const size_t sz = Size(v);
              total_sz += sz;
              tbl_customer(w)->insert(txn, EncodeK(k), Encode(obj_buf, v));
              tbl_customer(w)->insert(txn, EncodeK(k_balance), Encode(obj_buf, v_balance));

              const customer_data::key k_data(w, d+100, c);
              customer_data::value v_data;
              v_data.c_data.assign(RandomStr(r, RandomNumber(r, 100, 300)));
              tbl_customer(w)->insert(txn, EncodeK(k_data), Encode(obj_buf, v_data));

              // customer name index
              const customer_name_idx::key k_idx(k.c_w_id, k.c_d_id, v.c_last.str(true), v.c_first.str(true));
              const customer_name_idx::value v_idx(k.c_id);
              // index structure is:
              // (c_w_id, c_d_id, c_last, c_first) -> (c_id)

              tbl_customer_name_idx(w)->insert(txn, Encode(k_idx), Encode(obj_buf, v_idx));

              history::key k_hist;
              k_hist.h_c_id = c;
              k_hist.h_c_d_id = d;
              k_hist.h_c_w_id = w;
              k_hist.h_d_id = d;
              k_hist.h_w_id = w;
              k_hist.h_date = GetCurrentTimeMillis();

              history::value v_hist;
              v_hist.h_amount = 10;
              v_hist.h_data.assign(RandomStr(r, RandomNumber(r, 10, 24)));

              tbl_history(w)->insert(txn, EncodeK(k_hist), Encode(obj_buf, v_hist));
            }
            if (db->commit_txn(txn)) {
              batch++;
            } else {
              db->abort_txn(txn);
              if (verbose)
                cerr << "[WARNING] customer loader loading abort" << endl;
            }
          } catch (abstract_db::abstract_abort_exception &ex) {
            db->abort_txn(txn);
            if (verbose)
              cerr << "[WARNING] customer loader loading abort" << endl;
          }
        }
      }
    }

    if (verbose) {
      if (warehouse_id == -1) {
        cerr << "[INFO] finished loading customer" << endl;
        cerr << "[INFO]   * average customer record length: "
             << (double(total_sz)/double(NumWarehouses()*NumDistrictsPerWarehouse()*NumCustomersPerDistrict()))
             << " bytes " << endl;
      } else {
        cerr << "[INFO] finished loading customer (w=" << warehouse_id << ")" << endl;
      }
    }
  }

private:
  ssize_t warehouse_id;
};

class tpcc_order_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_order_loader(unsigned long seed,
                    abstract_db *db,
                    const map<string, abstract_ordered_index *> &open_tables,
                    const map<string, vector<abstract_ordered_index *>> &partitions,
                    const map<string, vector<abstract_ordered_index *>> &dummy_partitions,
                    ssize_t warehouse_id)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(partitions,dummy_partitions),
      warehouse_id(warehouse_id)
  {
    ALWAYS_ERROR(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

protected:
  virtual void
  load()
  {
    string obj_buf;

    uint64_t order_line_total_sz = 0, n_order_lines = 0;
    uint64_t oorder_total_sz = 0, n_oorders = 0;
    uint64_t new_order_total_sz = 0, n_new_orders = 0;

    const uint w_start = (warehouse_id == -1) ?
      1 : static_cast<uint>(warehouse_id);
    const uint w_end   = (warehouse_id == -1) ?
      NumWarehouses() : static_cast<uint>(warehouse_id);

    for (uint w = w_start; w <= w_end; w++) {
      if (pin_cpus)
        PinToWarehouseId(w);
      for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
        set<uint> c_ids_s;
        vector<uint> c_ids;
        while (c_ids.size() != NumCustomersPerDistrict()) {
          const auto x = (r.next() % NumCustomersPerDistrict()) + 1;
          if (c_ids_s.count(x))
            continue;
          c_ids_s.insert(x);
          c_ids.emplace_back(x);
        }
        for (uint c = 1; c <= NumCustomersPerDistrict();) {
          scoped_str_arena s_arena(arena);
          void * const txn = db->new_txn(txn_flags, arena, txn_buf());
          try {
            const oorder::key k_oo(w, d, c);

            oorder::value v_oo;
            v_oo.o_c_id = c_ids[c - 1];
            if (k_oo.o_id < 2101)
              v_oo.o_carrier_id = RandomNumber(r, 1, 10);
            else
              v_oo.o_carrier_id = 0;
            v_oo.o_ol_cnt = RandomNumber(r, 5, 15);
            v_oo.o_all_local = 1;
            v_oo.o_entry_d = GetCurrentTimeMillis();

            checker::SanityCheckOOrder(&k_oo, &v_oo);
            const size_t sz = Size(v_oo);
            oorder_total_sz += sz;
            n_oorders++;
            tbl_oorder(w)->insert(txn, EncodeK(k_oo), Encode(obj_buf, v_oo));

            const oorder_c_id_idx::key k_oo_idx(k_oo.o_w_id, k_oo.o_d_id, v_oo.o_c_id, k_oo.o_id);
            const oorder_c_id_idx::value v_oo_idx(0);

            tbl_oorder_c_id_idx(w)->insert(txn, Encode(k_oo_idx), Encode(obj_buf, v_oo_idx));

            if (c >= 2101) {
              const new_order::key k_no(w, d, c);
              const new_order::value v_no;

              checker::SanityCheckNewOrder(&k_no, &v_no);
              const size_t sz = Size(v_no);
              new_order_total_sz += sz;
              n_new_orders++;
              tbl_new_order(w)->insert(txn, Encode(k_no), Encode(obj_buf, v_no));
            }

            for (uint l = 1; l <= uint(v_oo.o_ol_cnt); l++) {
              const order_line::key k_ol(w, d, c, l);

              order_line::value v_ol;
              v_ol.ol_i_id = RandomNumber(r, 1, 100000);
              if (k_ol.ol_o_id < 2101) {
                v_ol.ol_delivery_d = v_oo.o_entry_d;
                v_ol.ol_amount = 0;
              } else {
                v_ol.ol_delivery_d = 0;
                // random within [0.01 .. 9,999.99]
                v_ol.ol_amount = (float) (RandomNumber(r, 1, 999999) / 100.0);
              }

              v_ol.ol_supply_w_id = k_ol.ol_w_id;
              v_ol.ol_quantity = 5;
              // v_ol.ol_dist_info comes from stock_data(ol_supply_w_id, ol_o_id)
              //v_ol.ol_dist_info = RandomStr(r, 24);

              checker::SanityCheckOrderLine(&k_ol, &v_ol);
              const size_t sz = Size(v_ol);
              order_line_total_sz += sz;
              n_order_lines++;
              tbl_order_line(w)->insert(txn, Encode(k_ol), Encode(obj_buf, v_ol));
            }
            if (db->commit_txn(txn)) {
              c++;
            } else {
              db->abort_txn(txn);
              ALWAYS_ERROR(warehouse_id != -1);
              if (verbose)
                cerr << "[WARNING] order loader loading abort" << endl;
            }
          } catch (abstract_db::abstract_abort_exception &ex) {
            db->abort_txn(txn);
            ALWAYS_ERROR(warehouse_id != -1);
            if (verbose)
              cerr << "[WARNING] order loader loading abort" << endl;
          }
        }
      }
    }

    if (verbose) {
      if (warehouse_id == -1) {
        cerr << "[INFO] finished loading order" << endl;
        cerr << "[INFO]   * average order_line record length: "
             << (double(order_line_total_sz)/double(n_order_lines)) << " bytes" << endl;
        cerr << "[INFO]   * average oorder record length: "
             << (double(oorder_total_sz)/double(n_oorders)) << " bytes" << endl;
        cerr << "[INFO]   * average new_order record length: "
             << (double(new_order_total_sz)/double(n_new_orders)) << " bytes" << endl;
      } else {
        cerr << "[INFO] finished loading order (w=" << warehouse_id << ")" << endl;
      }
    }
  }

private:
  ssize_t warehouse_id;
};

static event_counter evt_tpcc_cross_partition_new_order_txns("tpcc_cross_partition_new_order_txns");
static event_counter evt_tpcc_cross_partition_payment_txns("tpcc_cross_partition_payment_txns");

void tpcc_worker::scan_entire_warehouses(int w_id) {
    // ASCII code: 0 ~ 255, range search all potential keys
    // char WS = static_cast<char>(0);
    // std::string startKey(1, WS);
    // char WE = static_cast<char>(255);
    // std::string endKey(1, WE);
    // arena.reset();
    // void *txn_0 = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    // generic_scan_callback<warehouse::key, warehouse::value> calloc_0;
    // tbl_warehouse(w_id)->scan(txn_0, startKey, &endKey, calloc_0);
    // ALWAYS_ERROR(db->commit_txn(txn_0));
    // calloc_0.print_warehouse_info();

    Warning("scan warehouse-id: %d", w_id);
    char WS_1 = static_cast<char>(0);
    std::string startKey_1(1, WS_1);
    char WE_1 = static_cast<char>(255);
    std::string endKey_1(1, WE_1);
    arena.reset();
    void *txn_1 = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    generic_scan_callback<customer::key, customer::value> calloc_1;
    tbl_customer(w_id)->scan(txn_1, startKey_1, &endKey_1, calloc_1);
    int64_t a = calloc_1.print_customer_info();
    ALWAYS_ERROR(db->commit_txn(txn_1));

    /*
    char WS_2 = static_cast<char>(0);
    std::string startKey_2(1, WS_2);
    char WE_2 = static_cast<char>(255);
    std::string endKey_2(1, WE_2);
    generic_scan_callback<oorder::key, oorder::value> calloc_2;
    tbl_oorder(w_id)->scan(txn_1, startKey_2, &endKey_2, calloc_2);
    ALWAYS_ERROR(db->commit_txn(txn_1));
    int64_t b = calloc_2.print_oorder_info();

    std::cout << "  balance + cost: " << a + b << std::endl;
    if ((a+b)!=1000000000) {
      Warning("it is not equal, %d!=%d", (a+b), 1000000000);
    }*/
}

// workload: https://docs.google.com/document/d/11DqnIV0aC5aSV-uVg8SVST_5Ij4BCutfOQJdH7Fdr1s/edit?usp=sharing
tpcc_worker::txn_result
tpcc_worker::txn_new_order_simple() {
  bool isRemote = false;
  bool isAbort = false;

  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
  uint remote_warehouse_id = 0;
  if (NumWarehousesTotal() > 1 && TThread::get_nshards()>1)
    do {
     remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
   // } while (WarehouseInShard(remote_warehouse_id, shardIndex));
    } while (remote_warehouse_id == warehouse_id);

  std::vector<int> ids;
  {
    for (int c_id=1; c_id<=10; c_id++) {
      local_oorder_id ++;
      int oorder_id = local_oorder_id * 100 + TThread::getPartitionID();
      ids.push_back(oorder_id);
      bool is_remote=false;
      retry:
      arena.reset();
      void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
      try {
        // Tranasction-1
        {
          scoped_str_arena s_arena(arena);
          customer::key k_c(warehouse_id, 1, c_id);
          ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
          if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
          if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,is_remote?1:0);}
          customer::value v_c_temp;
          const customer::value *v_c = Decode(obj_v, v_c_temp);
          customer::value v_c_new(*v_c);
          v_c_new.c_payment_cnt -= 10;
          tbl_customer(warehouse_id)->put(txn, Encode(str(), k_c), Encode(str(), v_c_new));
          const oorder::key k_oo(warehouse_id, 1, oorder_id);
          oorder::value v_oo_new;
          v_oo_new.o_c_id = 10;
          v_oo_new.o_carrier_id = c_id;
          tbl_oorder(warehouse_id)->insert(txn, Encode(k_oo), Encode(str(), v_oo_new));

          if (remote_warehouse_id > 0 && !WarehouseInShard(remote_warehouse_id, shardIndex) && (c_id == 4||c_id == 5)) {
            customer::key r_k_c(WarehouseGlobal2Local(remote_warehouse_id), 1, c_id);
            //Warning("[DEBUG]client tries to get, w_id:%d,d:%d,c_id:%d,oorder_id:%d,len of obj_v(garbage):%d",WarehouseGlobal2Local(remote_warehouse_id), 1, c_id,oorder_id,obj_v.size());
            ALWAYS_ERROR(dummy_tbl_customer(remote_warehouse_id)->get(txn, Encode(obj_key0, r_k_c), obj_v));
            customer::value r_v_c_temp;
            //Warning("# of obj_v:%d,r_v_c_temp:%d,(w:%d,d:%d,o:%d)",obj_v.size(),Size(r_v_c_temp),WarehouseGlobal2Local(remote_warehouse_id), 1, c_id);
            const customer::value *r_v_c = Decode(obj_v, r_v_c_temp);
            customer::value r_v_c_new(*r_v_c);
            r_v_c_new.c_delivery_cnt = r_v_c_new.c_delivery_cnt + 1;
            dummy_tbl_customer(remote_warehouse_id)->put(txn, EncodeK(str(), r_k_c), Encode(str(), r_v_c_new));
            is_remote=true;
          }
        }
        bool ret = db->commit_txn(txn);
        if (!ret) {
          Warning("fail to transaction-1(false):%d,local_oorder_id:%d,par_id:%d", warehouse_id,local_oorder_id, TThread::getPartitionID());
          isAbort = true;
          //db->abort_txn(txn);
          //goto retry;
        } else {
          //Warning("succeed to transaction-1:%d,local_oorder_id:%d,par_id:%d,c_id:%d,is_remote:%d", warehouse_id,local_oorder_id, TThread::getPartitionID(),c_id,is_remote);
        }
      } catch (abstract_db::abstract_abort_exception &ex) {
        Warning("fail to transaction-1(abort):%d,local_oorder_id:%d,par_id:%d", warehouse_id,local_oorder_id, TThread::getPartitionID());
        db->abort_txn(txn);
        isAbort = true;
      } catch (int n) {
        db->abort_txn(txn);
        Warning("timeout abort,c_id:%d,oorder_id:%d",c_id,oorder_id);
      }
      usleep((rand()%100)*1000);

      //Transaction-2
      {
        arena.reset();
        if (c_id%2==1) continue;
        void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
        try {
          scoped_str_arena s_arena(arena);
          customer::key k_c(warehouse_id, 1, c_id);
          ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
          if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
          if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(is_remote,0);}
          customer::value v_c_temp;
          const customer::value *v_c = Decode(obj_v, v_c_temp);
          customer::value v_c_new(*v_c);
          v_c_new.c_payment_cnt += 10;
          tbl_customer(warehouse_id)->put(txn, Encode(str(), k_c), Encode(str(), v_c_new)); 

          const oorder::key k_oo(warehouse_id, 1, oorder_id);
          tbl_oorder(warehouse_id)->remove(txn, Encode(str(), k_oo));

          bool ret = db->commit_txn(txn);
          if (!ret) {
            Warning("fail to transaction-2(false):%d,local_oorder_id:%d,par_id:%d", warehouse_id,local_oorder_id,TThread::getPartitionID());
            //db->abort_txn(txn);
            isAbort = true;
          }else {
            Warning("succeed to transaction-2:%d,local_oorder_id:%d,par_id:%d", warehouse_id,local_oorder_id,TThread::getPartitionID());
          }
        } catch (abstract_db::abstract_abort_exception &ex) {
          Warning("fail to transaction-2(abort):%d,local_oorder_id:%d,par_id:%d", warehouse_id,local_oorder_id,TThread::getPartitionID());
          db->abort_txn(txn);
          isAbort = true;
        } catch (int n) {
          db->abort_txn(txn);
          Warning("timeout abort,c_id:%d,oorder_id:%d",c_id,oorder_id);
        }
      }
    }
  }
  usleep((rand()%100)*1000);  

  // Transaction-3
  if (isAbort || local_oorder_id % 10 == 0) {
    Warning("current oorder id: %d, w-id: %d,par_id:%d", local_oorder_id, warehouse_id,TThread::getPartitionID());
    try {
      usleep((rand()%100)*1000);
      scan_entire_warehouses(warehouse_id);
    } catch (abstract_db::abstract_abort_exception &ex) {
      Warning("abort scan operation");
    } catch (int n) {
      db->abort_txn(nullptr);
      Warning("timeout abort for warehouse-scan operation");
    }
  }

  /*
  for (int i=0; i<ids.size(); i++) {
    arena.reset();
    void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    const oorder::key k_oo(warehouse_id, 1, ids.at(i));
    bool tag = tbl_oorder(warehouse_id)->get(txn, EncodeK(obj_key0, k_oo), obj_v);
    std::vector<string> allVersion = MultiVersionValue::getAllVersion<oorder::value>(obj_v);
    std::cout << std::endl;
    int counter=0;
    std::cout << "key: " << k_oo << std::endl;
    std::cout << "value: " << std::endl;
    for (int i=0;i<allVersion.size();i++) {
      counter ++;
      if (allVersion[i].compare("DEL")==0)
        std::cout << std::to_string(counter) + ". " + "DEL" << std::endl;
      else {
        oorder::value o_v;
        const oorder::value *o_v_n = Decode(allVersion[i], o_v);
        std::cout << std::to_string(counter) + ". " << *o_v_n << std::endl;
      }
    }
    std::cout << std::endl;
    
    db->commit_txn(txn);
  }*/

  return txn_result(true, isRemote);
} 

/*
tpcc_worker::txn_result
tpcc_worker::txn_new_order_simple() {
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int n=10;
  // Insert 10 keys into oorder [0-9]
  for (int oorder_id=0; oorder_id<n; oorder_id++){
    arena.reset();
    void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    {
      scoped_str_arena s_arena(arena);
      const oorder::key k_oo(warehouse_id, 1, oorder_id);
      oorder::value v_oo_new;
      v_oo_new.o_c_id = 1;
      tbl_oorder(warehouse_id)->insert(txn, Encode(k_oo), Encode(str(), v_oo_new));
      bool ret = db->commit_txn(txn);
      if (!ret){
        Warning("fail to insert oorder_id:%d",oorder_id);
      }
    }
  }

  // remove 4, 6, 9
  for (int oorder_id=0; oorder_id<n; oorder_id++) {
    if (oorder_id!=4&&oorder_id!=6&&oorder_id!=9) continue;

    arena.reset();
    void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    const oorder::key k_oo(warehouse_id, 1, oorder_id);
    tbl_oorder(warehouse_id)->remove(txn, Encode(str(), k_oo));
    bool ret = db->commit_txn(txn);
    if (!ret){
      Warning("fail to remove oorder_id:%d",oorder_id);
    }
  }

  // Print all current values
  for (int oorder_id=0; oorder_id<n; oorder_id++){
    arena.reset();
    void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    {
      const oorder::key k_oo(warehouse_id, 1, oorder_id);
      bool tag = tbl_oorder(warehouse_id)->get(txn, EncodeK(obj_key0, k_oo), obj_v);
      std::vector<string> allVersion = MultiVersionValue::getAllVersion<oorder::value>(obj_v);
      std::cout << std::endl;
      int counter=0;
      std::cout << "key: " << k_oo << std::endl;
      std::cout << "value: " << std::endl;
      for (int i=0;i<allVersion.size();i++) {
        counter ++;
        if (allVersion[i].compare("DEL")==0)
          std::cout << std::to_string(counter) + ". " + "DEL" << std::endl;
        else {
          oorder::value o_v;
          const oorder::value *o_v_n = Decode(allVersion[i], o_v);
          std::cout << std::to_string(counter) + ". " << *o_v_n << std::endl;
        }
      }
      std::cout << std::endl;
      db->commit_txn(txn);
    }
  }

  // scan the range: [3,7)
  {
    arena.reset();
    void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
    scoped_str_arena s_arena(arena);
    const oorder::key k_no_0(warehouse_id, 1, 3);
    const oorder::key k_no_1(warehouse_id, 1, 7);
    generic_scan_callback<oorder::key, oorder::value> calloc;
    tbl_oorder(warehouse_id)->scan(txn, Encode(obj_key0, k_no_0), &Encode(obj_key1, k_no_1), calloc, s_arena.get());
    ALWAYS_ERROR(db->commit_txn(txn));
    int64_t b = calloc.print_oorder_info();
    std::cout << " count: " << b << std::endl;
  }
  
  exit(0);
  return txn_result(true, isRemote);
} */

tpcc_worker::txn_result
tpcc_worker::txn_new_order_micro_drtm() {
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int remote_warehouse_id = warehouse_id;
  if (NumWarehousesTotal() > 1 && 
        TThread::get_nshards()>1 &&
        RandomNumber(r, 1, 100) <= g_micro_remote_item_pct) {
          do {
            remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
          } while (WarehouseInShard(remote_warehouse_id, shardIndex));
          isRemote = true;
        }

  // TODO: the event driven might is the bottleneck for the microbenchmark (much high throughput)
  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_KV_RMW);
  scoped_str_arena s_arena(arena);
  try {
    //const string new_i_name = "aaaaaaaa";
    bool first=false;
    for (int i=0; i<RMW_count; i++) {
      uint64_t key = r.next() % nkeys;
      const item_micro::key k(key);

      if (isRemote&&!first) {
        first=true;
        ALWAYS_ERROR(dummy_tbl_item(remote_warehouse_id)->get(txn, EncodeK(k), obj_v));
        {  // optimistic replication
          item_micro::value v_c_temp;
          const item_micro::value *v_c = Decode(obj_v, v_c_temp);
          item_micro::value v_c_new(*v_c);
          string tt=intToString2(getCurrentTimeMillis2()%100000000);
          v_c_new.i_name.assign(tt.c_str());
          dummy_tbl_item(remote_warehouse_id)->put(txn, Encode(str(), k), Encode(str(), v_c_new));
          if (strncmp(v_c_new.i_name.data(), "cccccccc", 8) != 0){
            long long ts = std::stoll(std::string(v_c_new.i_name.data(),0,8));
            long long et = getCurrentTimeMillis2()%100000000;
            if (et-ts<45){
              usleep((45-(et-ts))*1000);
            }
          }
        } 
      } else {
        ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(k), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(isRemote,0);}
        {
          item_micro::value v_c_temp;
          const item_micro::value *v_c = Decode(obj_v, v_c_temp);
          item_micro::value v_c_new(*v_c);
          string tt=intToString2(getCurrentTimeMillis2()%100000000);
          v_c_new.i_name.assign(tt.c_str());
          tbl_item(1)->put(txn, Encode(str(), k), Encode(str(), v_c_new));
        }
      }
    }
    if (likely(db->commit_txn(txn)))
        return txn_result(true, 0 + (isRemote?1:0));
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, isRemote?1:0);
}


// This is the RW transaction
tpcc_worker::txn_result
tpcc_worker::txn_new_order_micro() {
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int remote_warehouse_id = warehouse_id;
  if (NumWarehousesTotal() > 1 && 
        TThread::get_nshards()>1 &&
        RandomNumber(r, 1, 100) <= g_micro_remote_item_pct) {
          do {
            remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
          } while (WarehouseInShard(remote_warehouse_id, shardIndex));
          isRemote = true;
        }

  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_KV_RMW);
  scoped_str_arena s_arena(arena);
  try {
    const string new_i_name = "aaaaaaaa";
    bool first=false;
    for (int i=0; i<RMW_count; i++) {
      uint64_t key = r.next() % nkeys;
      const item_micro::key k(key);
      item_micro::value v;
      v.i_name.assign(new_i_name);
      if (isRemote&&!first) {
        first=true;
        ALWAYS_ERROR(dummy_tbl_item(remote_warehouse_id)->get(txn, EncodeK(k), obj_v));
        dummy_tbl_item(remote_warehouse_id)->put(txn, Encode(str(), k), Encode(str(), v));
      } else {
        ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(k), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(isRemote,0);}
        tbl_item(1)->put(txn, Encode(str(), k), Encode(str(), v));
      }
    }
    if (likely(db->commit_txn(txn)))
        return txn_result(true, 0 + (isRemote?1:0));
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, isRemote?1:0);
}

tpcc_worker::txn_result
tpcc_worker::txn_new_order_micro_mega()
{
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int batch_size = srolis::mega_batch_size;

  int remote_warehouse_id = warehouse_id;
  if (NumWarehousesTotal() > 1 && 
        TThread::get_nshards()>1 &&
        RandomNumber(r, 1, 100) <= g_micro_remote_item_pct) {
          do {
            remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
          } while (WarehouseInShard(remote_warehouse_id, shardIndex));
          isRemote = true;
        }
  
  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_KV_RMW);
  scoped_str_arena s_arena(arena);
  try {
    const string new_i_name = "aaaaaaaa";
    bool first=false;
    for (int i=0; i<RMW_count; i++) {
      uint64_t key = r.next() % nkeys;
      const item_micro::key k(key);
      item_micro::value v;
      v.i_name.assign(new_i_name);
      if (isRemote&&!first) {
        first=true;
        ALWAYS_ERROR(dummy_tbl_item(remote_warehouse_id)->get(txn, EncodeK(k), obj_v));
        dummy_tbl_item(remote_warehouse_id)->put(txn, Encode(str(), k), Encode(str(), v));
      } else {
        for (int i=0; i<batch_size; i++) {
          uint64_t bkey = (key + i) % nkeys;
          const item_micro::key bk(bkey);
          item_micro::value bv;
          ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(bk), obj_v));
          if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
          if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(isRemote,0);}
          tbl_item(1)->put(txn, Encode(str(), bk), Encode(str(), bv));
        }
      }
    }
    if (likely(db->commit_txn(txn)))
        return txn_result(true, 0 + (isRemote?1:0));
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, isRemote?1:0);
}

tpcc_worker::txn_result
tpcc_worker::txn_new_order_mega()
{
  const uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  // if (cluster.compare("learner")==0){
  //  scan_entire_warehouses(warehouse_id);
  // }
  const uint districtID = RandomNumber(r, 1, 10);
  const uint customerID = GetCustomerId(r); // 1-3000
  int batch_size = srolis::mega_batch_size;
  const uint numItems = RandomNumber(r, 5, 15);
  // supplierWarehouseIDs ==> global warehouse-id
  uint itemIDs[15], supplierWarehouseIDs[15], orderQuantities[15];
  bool allLocal = true;  // not on the local warehouse, but it's possible still on the same shard server
  bool isRemote = false; // on the remote shard server
  int reallyRemote = 0;
  for (uint i = 0; i < numItems; i++) {
    itemIDs[i] = GetItemId(r); // 100000
    if (likely(g_disable_xpartition_txn ||
               NumWarehousesTotal() == 1 ||
               //i > 0 ||
               RandomNumber(r, 1, 100) > g_new_order_remote_item_pct)) {
      supplierWarehouseIDs[i] = WarehouseLocal2Global(warehouse_id);
    } else {
      do {
       supplierWarehouseIDs[i] = RandomNumber(r, 1, NumWarehousesTotal());
      } while (supplierWarehouseIDs[i] == WarehouseLocal2Global(warehouse_id) || ((!WarehouseInShard(supplierWarehouseIDs[i], shardIndex)) && reallyRemote >= 1));
      if (!WarehouseInShard(supplierWarehouseIDs[i], shardIndex)) {
        reallyRemote++;
      }
      allLocal = false;
      if (!WarehouseInShard(supplierWarehouseIDs[i], shardIndex)) { isRemote = true; } 
    }
    orderQuantities[i] = RandomNumber(r, 1, 10);
  }

  // XXX(stephentu): implement rollback
  //
  // worst case txn profile:
  //   1 customer get
  //   1 warehouse get
  //   1 district get
  //   1 new_order insert
  //   1 district put
  //   1 oorder insert
  //   1 oorder_cid_idx insert
  //   15 times:
  //      1 item get
  //      1 stock get
  //      1 stock put
  //      1 order_line insert
  //
  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 0
  //   max_read_set_size : 15
  //   max_write_set_size : 15
  //   num_txn_contexts : 9
  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
  scoped_str_arena s_arena(arena);

  scoped_multilock<spinlock> mlock;
  if (g_enable_partition_locks) {  // partition-locks is disable by default
    if (allLocal) {
      mlock.enq(LockForPartition(warehouse_id));
    } else {
      small_unordered_map<unsigned int, bool, 64> lockset;
      mlock.enq(LockForPartition(warehouse_id));
      lockset[PartitionId(warehouse_id)] = 1;
      for (uint i = 0; i < numItems; i++) {
        if (lockset.find(PartitionId(supplierWarehouseIDs[i])) == lockset.end()) {
          //mlock.enq(LockForPartition(supplierWarehouseIDs[i]));
          lockset[PartitionId(supplierWarehouseIDs[i])] = 1;
        }
      }
    }
    mlock.multilock();
  }
  try {
    ssize_t ret = 0;

    for (int i=0;i<batch_size;i++){
        const customer::key k_c(warehouse_id, districtID, customerID+i>3000?customerID+i-3000:customerID+i);
        ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    }

    // for warehouse_id and districtID, just do once
    const warehouse::key k_w(warehouse_id);
    ALWAYS_ERROR(tbl_warehouse(warehouse_id)->get(txn, Encode(obj_key0, k_w), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}


    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ERROR(tbl_district(warehouse_id)->get(txn, Encode(obj_key0, k_d), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}

    int32_t no_o_ids[batch_size];
    for (int i=0;i<batch_size;i++){
        const uint64_t my_next_o_id = FastNewOrderIdGen(warehouse_id, districtID);
        no_o_ids[i] = my_next_o_id;
        const new_order::key k_no(warehouse_id, districtID, my_next_o_id);
        const new_order::value v_no;
        const size_t new_order_sz = Size(v_no);
        tbl_new_order(warehouse_id)->insert(txn, Encode(str(), k_no), Encode(str(), v_no));
        ret += new_order_sz;

        // always enable fast-gen
        const oorder::key k_oo(warehouse_id, districtID, k_no.no_o_id);
        oorder::value v_oo;
        v_oo.o_c_id = int32_t(customerID);
        v_oo.o_carrier_id = 0; // seems to be ignored
        v_oo.o_ol_cnt = int8_t(numItems);
        v_oo.o_all_local = allLocal;
        v_oo.o_entry_d = GetCurrentTimeMillis();

        const size_t oorder_sz = Size(v_oo);
        tbl_oorder(warehouse_id)->insert(txn, EncodeK(str(), k_oo), Encode(str(), v_oo));
        ret += oorder_sz;

        const oorder_c_id_idx::key k_oo_idx(warehouse_id, districtID, customerID, k_no.no_o_id);
        const oorder_c_id_idx::value v_oo_idx(0);

        tbl_oorder_c_id_idx(warehouse_id)->insert(txn, Encode(str(), k_oo_idx), Encode(str(), v_oo_idx));
    }

    for (uint ol_number = 1; ol_number <= numItems; ol_number++) {
      const uint ol_supply_w_id = supplierWarehouseIDs[ol_number - 1];
      uint ol_i_id = itemIDs[ol_number - 1];
      const uint ol_quantity = orderQuantities[ol_number - 1];

      int32_t base_ol_i_id = ol_i_id;
      for (int i=0;i<batch_size;i++){
        ol_i_id = base_ol_i_id+i>100000?base_ol_i_id+i-100000:base_ol_i_id+i;
        const item::key k_i(ol_i_id);
        ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(obj_key0, k_i), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
        item::value v_i_temp;
        const item::value *v_i = Decode(obj_v, v_i_temp);
        checker::SanityCheckItem(&k_i, v_i);
    
        // local operations, for remote operations, we do it outside this for-loop
        if (WarehouseInShard(ol_supply_w_id, shardIndex)){
          const stock::key k_s(WarehouseGlobal2Local(ol_supply_w_id), ol_i_id);
          ALWAYS_ERROR(tbl_stock(WarehouseGlobal2Local(ol_supply_w_id))->get(txn, EncodeK(obj_key0, k_s), obj_v));
          if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
          if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    
          stock::value v_s_temp;
          const stock::value *v_s = Decode(obj_v, v_s_temp);
          checker::SanityCheckStock(&k_s, v_s);
    
          stock::value v_s_new(*v_s);
          if (v_s_new.s_quantity - ol_quantity >= 10)
            v_s_new.s_quantity -= ol_quantity;
          else
            v_s_new.s_quantity += -int32_t(ol_quantity) + 91;
          v_s_new.s_ytd += ol_quantity;
          v_s_new.s_remote_cnt += (ol_supply_w_id == WarehouseLocal2Global(warehouse_id)) ? 0 : 1;
    
          tbl_stock(WarehouseGlobal2Local(ol_supply_w_id))->put(txn, EncodeK(str(), k_s), Encode(str(), v_s_new));
        } 
          const order_line::key k_ol(warehouse_id, districtID, no_o_ids[i], ol_number);
          order_line::value v_ol;
          v_ol.ol_i_id = int32_t(ol_i_id);
          v_ol.ol_delivery_d = 0; // not delivered yet
          v_ol.ol_amount = float(ol_quantity) * v_i->i_price;
          v_ol.ol_supply_w_id = WarehouseGlobal2Local(int32_t(ol_supply_w_id));
          v_ol.ol_quantity = int8_t(ol_quantity);
    
          const size_t order_line_sz = Size(v_ol);
          tbl_order_line(warehouse_id)->insert(txn, Encode(str(), k_ol), Encode(str(), v_ol));
          ret += order_line_sz;
      }
    
      // for remote operations, not using loop on batch-size
      // using batch operations: implement batch-operations
      if (!WarehouseInShard(ol_supply_w_id, shardIndex)) {
        const stock::key k_s(WarehouseGlobal2Local(ol_supply_w_id), base_ol_i_id);
        dummy_tbl_stock(ol_supply_w_id)->get(txn, EncodeK(obj_key0, k_s), obj_v);
        //std::cout<<"send base:"<<base_ol_i_id<<", tid:"<<TThread::getPartitionID()<<", v-len:"<<obj_v.length()<< std::endl;
    
        // batch-read
        for (int i=0; i<srolis::mega_batch_size; i++){
          // cast away const
          //stock::value *v_s= const_cast<stock::value*>(reinterpret_cast<const stock::value*>(obj_v.data()+i*srolis::size_per_stock_value));
          stock::value *v_s= const_cast<stock::value*>(reinterpret_cast<const stock::value*>(obj_v.data()));
          if (v_s->s_quantity - ol_quantity >= 10)
            v_s->s_quantity -= ol_quantity;
          else
            v_s->s_quantity += -int32_t(ol_quantity) + 91;
          v_s->s_ytd += ol_quantity;
          v_s->s_remote_cnt += (ol_supply_w_id == WarehouseLocal2Global(warehouse_id)) ? 0 : 1;
        }
    
        stock::value v_s_temp;
        const stock::value *v_s = Decode(obj_v, v_s_temp);
        stock::value v_s_new(*v_s); 
        // why obj_v not work? it would be ok as value doesn't matter!
        // batch-write
        dummy_tbl_stock(ol_supply_w_id)->put(txn, EncodeK(str(), k_s), Encode(str(), v_s_new)); 
      }
    
    } // END of loop items

    if (likely(db->commit_txn(txn))){
      // if (control_mode==2 && counter_new_order_failed>0 && shardIndex>0){
      //   Warning("counter_new_order_failed success:%d\n",counter_new_order_failed);
      // }
      return txn_result(true, ret * 10 + (isRemote?1:0));
    }
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    if (n==1002) { // same as fasttransport.cc
      counter_new_order_failed+=1;
    }
    db->abort_txn_local(txn);
  }
  return txn_result(false, 0 + (isRemote?1:0));
}


tpcc_worker::txn_result
tpcc_worker::txn_new_order()
{
#if defined(SIMPLE_WORKLOAD)
  usleep(rand()%10 * 1000);
  return txn_new_order_simple();
#elif defined(MICRO_BENCHMARK)
  if (drtm)
    return txn_new_order_micro_drtm();
  return txn_new_order_micro();
#else
  const uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  // if (cluster.compare("learner")==0){
  //  scan_entire_warehouses(warehouse_id);
  // }
  const uint districtID = RandomNumber(r, 1, 10);
  const uint customerID = GetCustomerId(r);
  const uint numItems = RandomNumber(r, 5, 15);
  // supplierWarehouseIDs ==> global warehouse-id
  uint itemIDs[15], supplierWarehouseIDs[15], orderQuantities[15];
  bool allLocal = true;  // not on the local warehouse, but it's possible still on the same shard server
  bool isRemote = false; // on the remote shard server
  for (uint i = 0; i < numItems; i++) {
    itemIDs[i] = GetItemId(r);
    if (likely(g_disable_xpartition_txn ||
               NumWarehousesTotal() == 1 ||
               //i > 0 ||
               RandomNumber(r, 1, 100) > g_new_order_remote_item_pct)) {
      supplierWarehouseIDs[i] = WarehouseLocal2Global(warehouse_id);
    } else {
      do {
       supplierWarehouseIDs[i] = RandomNumber(r, 1, NumWarehousesTotal());
      } while (supplierWarehouseIDs[i] == WarehouseLocal2Global(warehouse_id));
      allLocal = false;
      if (TThread::skipBeforeRemoteNewOrder-g_new_order_remote_item_pct>0){
        if (ShardIndexFromGlobalWarehouse(supplierWarehouseIDs[i])==0){ // fixed 0
          return txn_result(false, 0 + (isRemote?1:0));
        }else{
          TThread::skipBeforeRemoteNewOrder -= 1;
        }
      }

#if defined(FAIL_NEW_VERSION)
      if (control_mode==4){ // distributed transaction without failed shard
        if (ShardIndexFromGlobalWarehouse(supplierWarehouseIDs[i])==sync_util::sync_logger::failed_shard_index){
          counter_new_order_failed+=1;
          return txn_result(false, 0 + (isRemote?1:0));
        }
      }
#else
      if (control_mode==1){ // distributed transaction without failed shard
        if (ShardIndexFromGlobalWarehouse(supplierWarehouseIDs[i])==sync_util::sync_logger::failed_shard_index){
          counter_new_order_failed+=1;
          return txn_result(false, 0 + (isRemote?1:0));
        }
      }else if (control_mode==3){ // local transactions
        if (ShardIndexFromGlobalWarehouse(supplierWarehouseIDs[i])!=shardIndex){
          return txn_result(false, 0 + (isRemote?1:0));
        }
      }
#endif
      if (!WarehouseInShard(supplierWarehouseIDs[i], shardIndex)) { 
        isRemote = true; 
        TThread::isRemoteShard = true;
      } 
      TThread::isHomeWarehouse = false;
    }
    orderQuantities[i] = RandomNumber(r, 1, 10);
  }

#if defined(FAIL_NEW_VERSION)
  if (control_mode==5 && counter_new_order_failed>0 && shardIndex>0){
    if (rand()%20==0){
      counter_new_order_failed-=1;
      supplierWarehouseIDs[0]=RandomNumber(r, 1, NumWarehouses()); // always assume the shard-0 failed
      isRemote = true;
    }
  }
#else
  if (control_mode==2 && counter_new_order_failed>0 && shardIndex>0){
    if (rand()%20==0){
      counter_new_order_failed-=1;
      supplierWarehouseIDs[0]=RandomNumber(r, 1, NumWarehouses()); // always assume the shard-0 failed
      isRemote = true;
    }
  }
#endif
  INVARIANT(!g_disable_xpartition_txn || allLocal);
  if (!allLocal)
    ++evt_tpcc_cross_partition_new_order_txns;

  // XXX(stephentu): implement rollback
  //
  // worst case txn profile:
  //   1 customer get
  //   1 warehouse get
  //   1 district get
  //   1 new_order insert
  //   1 district put
  //   1 oorder insert
  //   1 oorder_cid_idx insert
  //   15 times:
  //      1 item get
  //      1 stock get
  //      1 stock put
  //      1 order_line insert
  //
  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 0
  //   max_read_set_size : 15
  //   max_write_set_size : 15
  //   num_txn_contexts : 9
  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
  scoped_str_arena s_arena(arena);

  scoped_multilock<spinlock> mlock;
  if (g_enable_partition_locks) {  // partition-locks is disable by default
    if (allLocal) {
      mlock.enq(LockForPartition(warehouse_id));
    } else {
      small_unordered_map<unsigned int, bool, 64> lockset;
      mlock.enq(LockForPartition(warehouse_id));
      lockset[PartitionId(warehouse_id)] = 1;
      for (uint i = 0; i < numItems; i++) {
        if (lockset.find(PartitionId(supplierWarehouseIDs[i])) == lockset.end()) {
          //mlock.enq(LockForPartition(supplierWarehouseIDs[i]));
          lockset[PartitionId(supplierWarehouseIDs[i])] = 1;
        }
      }
    }
    mlock.multilock();
  }
  try {
    ssize_t ret = 0;
    const customer::key k_c(warehouse_id, districtID, customerID);
    ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_new_order_failed+=1;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    customer::value v_c_temp;
    const customer::value *v_c = Decode(obj_v, v_c_temp);
    checker::SanityCheckCustomer(&k_c, v_c);

    const warehouse::key k_w(warehouse_id);
    ALWAYS_ERROR(tbl_warehouse(warehouse_id)->get(txn, Encode(obj_key0, k_w), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_new_order_failed+=1;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    warehouse::value v_w_temp;
    const warehouse::value *v_w = Decode(obj_v, v_w_temp);
    checker::SanityCheckWarehouse(&k_w, v_w);

    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ERROR(tbl_district(warehouse_id)->get(txn, Encode(obj_key0, k_d), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_new_order_failed+=1;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    district::value v_d_temp;
    const district::value *v_d = Decode(obj_v, v_d_temp);
    checker::SanityCheckDistrict(&k_d, v_d);

    const uint64_t my_next_o_id = g_new_order_fast_id_gen ?
        FastNewOrderIdGen(warehouse_id, districtID) : v_d->d_next_o_id;

    const new_order::key k_no(warehouse_id, districtID, my_next_o_id);
    const new_order::value v_no;
    const size_t new_order_sz = Size(v_no);
    tbl_new_order(warehouse_id)->insert(txn, Encode(str(), k_no), Encode(str(), v_no));
    ret += new_order_sz;

    if (!g_new_order_fast_id_gen) {
      district::value v_d_new(*v_d);
      v_d_new.d_next_o_id++;
      tbl_district(warehouse_id)->put(txn, Encode(str(), k_d), Encode(str(), v_d_new));
    }

    const oorder::key k_oo(warehouse_id, districtID, k_no.no_o_id);
    oorder::value v_oo;
    v_oo.o_c_id = int32_t(customerID);
    v_oo.o_carrier_id = 0; // seems to be ignored
    v_oo.o_ol_cnt = int8_t(numItems);
    v_oo.o_all_local = allLocal;
    v_oo.o_entry_d = GetCurrentTimeMillis();

    const size_t oorder_sz = Size(v_oo);
    tbl_oorder(warehouse_id)->insert(txn, EncodeK(str(), k_oo), Encode(str(), v_oo));
    ret += oorder_sz;

    const oorder_c_id_idx::key k_oo_idx(warehouse_id, districtID, customerID, k_no.no_o_id);
    const oorder_c_id_idx::value v_oo_idx(0);

    tbl_oorder_c_id_idx(warehouse_id)->insert(txn, Encode(str(), k_oo_idx), Encode(str(), v_oo_idx));

    for (uint ol_number = 1; ol_number <= numItems; ol_number++) {
      const uint ol_supply_w_id = supplierWarehouseIDs[ol_number - 1];
      const uint ol_i_id = itemIDs[ol_number - 1];
      const uint ol_quantity = orderQuantities[ol_number - 1];

      const item::key k_i(ol_i_id);
      ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(obj_key0, k_i), obj_v));
      if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_new_order_failed+=1;}
      if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      item::value v_i_temp;
      const item::value *v_i = Decode(obj_v, v_i_temp);
      checker::SanityCheckItem(&k_i, v_i);
      const stock::key k_s(WarehouseGlobal2Local(ol_supply_w_id), ol_i_id);
      if (WarehouseInShard(ol_supply_w_id, shardIndex)) {
        ALWAYS_ERROR(tbl_stock(WarehouseGlobal2Local(ol_supply_w_id))->get(txn, EncodeK(obj_key0, k_s), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_new_order_failed+=1;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      } else {
        bool ret=dummy_tbl_stock(ol_supply_w_id)->get(txn, EncodeK(obj_key0, k_s), obj_v);
        // if (is_sampling_remote_calls && rand() % sampling_number == 0)
        //   sampling_remote_calls.push_back(tmp);
        ALWAYS_ERROR(ret);
      }
      stock::value v_s_temp;
      const stock::value *v_s = Decode(obj_v, v_s_temp);
      checker::SanityCheckStock(&k_s, v_s);

      stock::value v_s_new(*v_s);
      if (v_s_new.s_quantity - ol_quantity >= 10)
        v_s_new.s_quantity -= ol_quantity;
      else
        v_s_new.s_quantity += -int32_t(ol_quantity) + 91;
      v_s_new.s_ytd += ol_quantity;
      v_s_new.s_remote_cnt += (ol_supply_w_id == WarehouseLocal2Global(warehouse_id)) ? 0 : 1;
      if (WarehouseInShard(ol_supply_w_id, shardIndex)) {
        tbl_stock(WarehouseGlobal2Local(ol_supply_w_id))->put(txn, EncodeK(str(), k_s), Encode(str(), v_s_new));
      } else {
        dummy_tbl_stock(ol_supply_w_id)->put(txn, EncodeK(str(), k_s), Encode(str(), v_s_new));
        // if (is_sampling_remote_calls && rand() % sampling_number == 0)
        //   sampling_remote_calls.push_back(tmp);
      }
      const order_line::key k_ol(warehouse_id, districtID, k_no.no_o_id, ol_number);
      order_line::value v_ol;
      v_ol.ol_i_id = int32_t(ol_i_id);
      v_ol.ol_delivery_d = 0; // not delivered yet
      v_ol.ol_amount = float(ol_quantity) * v_i->i_price;
      v_ol.ol_supply_w_id = WarehouseGlobal2Local(int32_t(ol_supply_w_id));
      v_ol.ol_quantity = int8_t(ol_quantity);

      const size_t order_line_sz = Size(v_ol);
      tbl_order_line(warehouse_id)->insert(txn, Encode(str(), k_ol), Encode(str(), v_ol));
      ret += order_line_sz;
    }

    if (likely(db->commit_txn(txn))){
      // if (control_mode==2 && counter_new_order_failed>0 && shardIndex>0){
      //   Warning("counter_new_order_failed success:%d\n",counter_new_order_failed);
      // }
      return txn_result(true, ret * 10 + (isRemote?1:0));
    }
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    if (n==1002) { // same as fasttransport.cc
      counter_new_order_failed+=1;
    }
    db->abort_txn_local(txn);
  }
  return txn_result(false, 0 + (isRemote?1:0));
#endif
}

class new_order_scan_callback : public abstract_ordered_index::scan_callback {
public:
  new_order_scan_callback() : k_no(0) {}
  virtual bool invoke(
      const char *keyp, size_t keylen,
      const string &value)
  {
    INVARIANT(keylen == sizeof(new_order::key));
    INVARIANT(value.size() == sizeof(new_order::value));
    k_no = Decode(keyp, k_no_temp);
#ifdef CHECK_INVARIANTS
    new_order::value v_no_temp;
    const new_order::value *v_no = Decode(value, v_no_temp);
    checker::SanityCheckNewOrder(k_no, v_no);
#endif
    return false;
  }
  inline const new_order::key *
  get_key() const
  {
    return k_no;
  }
private:
  new_order::key k_no_temp;
  const new_order::key *k_no;
};

STATIC_COUNTER_DECL(scopedperf::tod_ctr, delivery_probe0_tod, delivery_probe0_cg)

tpcc_worker::txn_result
tpcc_worker::txn_delivery()
{
  const uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
  const uint o_carrier_id = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  const uint32_t ts = GetCurrentTimeMillis();

  // worst case txn profile:
  //   10 times:
  //     1 new_order scan node
  //     1 oorder get
  //     2 order_line scan nodes
  //     15 order_line puts
  //     1 new_order remove
  //     1 oorder put
  //     1 customer get
  //     1 customer put
  //
  // output from counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 21
  //   max_read_set_size : 133
  //   max_write_set_size : 133
  //   num_txn_contexts : 4
  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_TPCC_DELIVERY);
  scoped_str_arena s_arena(arena);
  scoped_lock_guard<spinlock> slock(
      g_enable_partition_locks ? &LockForPartition(warehouse_id) : nullptr);
  try {
    ssize_t ret = 0;
    for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
      // SWH: (TODO) it's better to keep last_no_o_ids for the take over
      const new_order::key k_no_0(warehouse_id, d, last_no_o_ids[d - 1]);
      const new_order::key k_no_1(warehouse_id, d, numeric_limits<int32_t>::max());
      new_order_scan_callback new_order_c;
      {
        ANON_REGION("DeliverNewOrderScan:", &delivery_probe0_cg);
        tbl_new_order(warehouse_id)->scan(txn, Encode(obj_key0, k_no_0), &Encode(obj_key1, k_no_1), new_order_c, s_arena.get());
      }

      const new_order::key *k_no = new_order_c.get_key();
      if (unlikely(!k_no))
        continue;
      last_no_o_ids[d - 1] = k_no->no_o_id + 1; // XXX: update last seen

      const oorder::key k_oo(warehouse_id, d, k_no->no_o_id);
      if (unlikely(!tbl_oorder(warehouse_id)->get(txn, EncodeK(obj_key0, k_oo), obj_v))) {
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}
        // even if we read the new order entry, there's no guarantee
        // we will read the oorder entry: in this case the txn will abort,
        // but we're simply bailing out early
        db->abort_txn(txn);
        return txn_result(false, 0);
      }
      if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
      if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}

      oorder::value v_oo_temp;
      const oorder::value *v_oo = Decode(obj_v, v_oo_temp);
      checker::SanityCheckOOrder(&k_oo, v_oo);

      static_limit_callback<15> c(s_arena.get(), false); // never more than 15 order_lines per order
      const order_line::key k_oo_0(warehouse_id, d, k_no->no_o_id, 0);
      const order_line::key k_oo_1(warehouse_id, d, k_no->no_o_id, numeric_limits<int32_t>::max());

      // XXX(stephentu): mutable scans would help here
      tbl_order_line(warehouse_id)->scan(txn, Encode(obj_key0, k_oo_0), &Encode(obj_key1, k_oo_1), c, s_arena.get());
      float sum = 0.0;
      for (size_t i = 0; i < c.size(); i++) {
        order_line::value v_ol_temp;
        const order_line::value *v_ol = Decode(*c.values[i].second, v_ol_temp);

#ifdef CHECK_INVARIANTS
        order_line::key k_ol_temp;
        const order_line::key *k_ol = Decode(*c.values[i].first, k_ol_temp);
        checker::SanityCheckOrderLine(k_ol, v_ol);
#endif

        sum += v_ol->ol_amount;
        order_line::value v_ol_new(*v_ol);
        v_ol_new.ol_delivery_d = ts;
        INVARIANT(s_arena.get()->manages(c.values[i].first));
        tbl_order_line(warehouse_id)->put(txn, *c.values[i].first, Encode(str(), v_ol_new));
      }

      // delete new order
      tbl_new_order(warehouse_id)->remove(txn, Encode(str(), *k_no));
      ret -= 0 /*new_order_c.get_value_size()*/;

      // update oorder
      oorder::value v_oo_new(*v_oo);
      v_oo_new.o_carrier_id = o_carrier_id;
      tbl_oorder(warehouse_id)->put(txn, EncodeK(str(), k_oo), Encode(str(), v_oo_new));

      const uint c_id = v_oo->o_c_id;
      const float ol_total = sum;

      // const customer::key k_c(warehouse_id, d, c_id);
      // ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
      // if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}

      // customer::value v_c_temp;
      // const customer::value *v_c = Decode(obj_v, v_c_temp);
      // customer::value v_c_new(*v_c);
      // v_c_new.c_balance += ol_total;
      // tbl_customer(warehouse_id)->put(txn, EncodeK(str(), k_c), Encode(str(), v_c_new));

      const customer::key k_c_new(warehouse_id, d + 200, c_id);
      ALWAYS_ASSERT(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c_new), obj_v));
      if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
      if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}

      customer_balance::value v_c_b_temp;
      const customer_balance::value *v_c_b = Decode(obj_v, v_c_b_temp);
      customer_balance::value v_c_b_new(*v_c_b);
      v_c_b_new.c_balance += ol_total;
      tbl_customer(warehouse_id)->put(txn, EncodeK(str(), k_c_new), Encode(str(), v_c_b_new));
    }
    //measure_txn_counters(txn, "txn_delivery");
    if (likely(db->commit_txn(txn)))
      return txn_result(true, ret * 10);
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, 0);
}

static event_avg_counter evt_avg_cust_name_idx_scan_size("avg_cust_name_idx_scan_size");

tpcc_worker::txn_result
tpcc_worker::txn_payment_micro_drtm() {
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int remote_warehouse_id = warehouse_id;
  if (NumWarehousesTotal() > 1 && 
        TThread::get_nshards()>1 &&
        RandomNumber(r, 1, 100) <= g_micro_remote_item_pct) {
          do {
            remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
          } while (WarehouseInShard(remote_warehouse_id, shardIndex));
          isRemote = true;
        }

  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_KV_RMW);
  scoped_str_arena s_arena(arena);
  try {
    //const string new_i_name = "aaaaaaaa";
    bool first=false;
    for (int i=0; i<RMW_count; i++) {
      uint64_t key = r.next() % nkeys;
      const item_micro::key k(key);
      item_micro::value v;
      string tt1=intToString2(getCurrentTimeMillis2()%100000000);
      v.i_name.assign(tt1.c_str());
      if (isRemote&&!first) {
        first=true;
        ALWAYS_ERROR(dummy_tbl_item(remote_warehouse_id)->get(txn, EncodeK(k), obj_v));
        { // optimistic replication
          item_micro::value v_c_temp;
          const item_micro::value *v_c = Decode(obj_v, v_c_temp);
          item_micro::value v_c_new(*v_c);
          if (strncmp(v_c_new.i_name.data(), "cccccccc", 8) != 0){
              long long ts = std::stoll(std::string(v_c_new.i_name.data(),0,8));
              long long et = getCurrentTimeMillis2()%100000000;
              if (et-ts<45){
                usleep((45-(et-ts))*1000);
              }
          }
        }
      } else {
        ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(k), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      }
    }
    if (likely(db->commit_txn(txn)))
        return txn_result(true, 0 + (isRemote?1:0));
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, isRemote?1:0);
}

tpcc_worker::txn_result
tpcc_worker::txn_payment_micro_mega() {
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int batch_size = srolis::mega_batch_size;

  int remote_warehouse_id = warehouse_id;
  if (NumWarehousesTotal() > 1 && 
        TThread::get_nshards()>1 &&
        RandomNumber(r, 1, 100) <= g_micro_remote_item_pct) {
          do {
            remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
          } while (WarehouseInShard(remote_warehouse_id, shardIndex));
          isRemote = true;
        }

  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_KV_RMW);
  scoped_str_arena s_arena(arena);
  try {
    const string new_i_name = "aaaaaaaa";
    bool first=false;
    for (int i=0; i<RMW_count; i++) {
      uint64_t key = r.next() % nkeys;
      const item_micro::key k(key);
      item_micro::value v;
      v.i_name.assign(new_i_name);
      if (isRemote&&!first) {
        first=true;
        ALWAYS_ERROR(dummy_tbl_item(remote_warehouse_id)->get(txn, EncodeK(k), obj_v));
      } else {
        for (int i=0; i<batch_size; i++) {
          uint64_t bkey = (key + i) % nkeys; 
          const item_micro::key bk(bkey);
          ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(bk), obj_v));
          if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
          if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
        }
      }
    }
    if (likely(db->commit_txn(txn)))
        return txn_result(true, 0 + (isRemote?1:0));
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, isRemote?1:0);
}

// This is the Read-only transaction
tpcc_worker::txn_result
tpcc_worker::txn_payment_micro() {
  bool isRemote = false;
  uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);

  int remote_warehouse_id = warehouse_id;
  if (NumWarehousesTotal() > 1 && 
        TThread::get_nshards()>1 &&
        RandomNumber(r, 1, 100) <= g_micro_remote_item_pct) {
          do {
            remote_warehouse_id = RandomNumber(r, 1, NumWarehousesTotal());
          } while (WarehouseInShard(remote_warehouse_id, shardIndex));
          isRemote = true;
        }

  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_KV_RMW);
  scoped_str_arena s_arena(arena);
  try {
    const string new_i_name = "aaaaaaaa";
    bool first=false;
    for (int i=0; i<RMW_count; i++) {
      uint64_t key = r.next() % nkeys;
      const item_micro::key k(key);
      item_micro::value v;
      v.i_name.assign(new_i_name);
      if (isRemote&&!first) {
        first=true;
        ALWAYS_ERROR(dummy_tbl_item(remote_warehouse_id)->get(txn, EncodeK(k), obj_v));
      } else {
        ALWAYS_ERROR(tbl_item(1)->get(txn, EncodeK(k), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      }
    }
    if (likely(db->commit_txn(txn)))
        return txn_result(true, 0 + (isRemote?1:0));
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, isRemote?1:0);
}

tpcc_worker::txn_result
tpcc_worker::txn_payment()
{
#if defined(MICRO_BENCHMARK)
  if (drtm)
    return txn_payment_micro_drtm();
  return txn_payment_micro();
#else
  const uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
  const uint districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  uint customerDistrictID, customerWarehouseID;
  bool allLocal = true;  // not on the local warehouse, but it's possible still on the same shard server
  bool isRemote = false; // on the remote shard server
  if (likely(g_disable_xpartition_txn ||
             NumWarehousesTotal() == 1 ||
             RandomNumber(r, 1, 100) <= 85)) {
    customerDistrictID = districtID;
    customerWarehouseID = WarehouseLocal2Global(warehouse_id);
  } else {
    customerDistrictID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
    do {
      customerWarehouseID = RandomNumber(r, 1, NumWarehousesTotal());
    } while (customerWarehouseID == WarehouseLocal2Global(warehouse_id));
    allLocal = false;
    TThread::isHomeWarehouse = false;
  }

#if defined(FAIL_NEW_VERSION)
  if (control_mode==4) {
    if (ShardIndexFromGlobalWarehouse(customerWarehouseID)==sync_util::sync_logger::failed_shard_index){
      counter_payment_failed+=1;
      return txn_result(false, 0 + (isRemote?1:0));
    }
  }
#else
  if (control_mode==1){ // distributed transactions without failed shard
    if (ShardIndexFromGlobalWarehouse(customerWarehouseID)==sync_util::sync_logger::failed_shard_index){
      counter_payment_failed+=1;
      return txn_result(false, 0 + (isRemote?1:0));
    }
  }else if (control_mode==3){ // local transactions
    if (ShardIndexFromGlobalWarehouse(customerWarehouseID)!=shardIndex){
      return txn_result(false, 0 + (isRemote?1:0));
    }
  }
#endif

  if (TThread::skipBeforeRemotePayment>0){
    if (ShardIndexFromGlobalWarehouse(customerWarehouseID)==0){
      return txn_result(false, 0 + (isRemote?1:0));
    } else {
      TThread::skipBeforeRemotePayment -= 1;
    }
  }

#if defined(FAIL_NEW_VERSION)
  if (control_mode==5 && counter_payment_failed>0 && shardIndex>0){
    if (rand()%20==0){
      counter_payment_failed-=1;
      customerWarehouseID=RandomNumber(r, 1, NumWarehouses());
      isRemote=true;
    }
  }
#else
  if (control_mode==2 && counter_payment_failed>0 && shardIndex>0){
    if (rand()%20==0){
      counter_payment_failed-=1;
      customerWarehouseID=RandomNumber(r, 1, NumWarehouses());
      isRemote=true;
    }
  }
#endif
  if (!WarehouseInShard(customerWarehouseID, shardIndex)) { 
    isRemote = true; 
    TThread::isRemoteShard = true;
  }
  const float paymentAmount = (float) (RandomNumber(r, 100, 500000) / 100.0);
  const uint32_t ts = GetCurrentTimeMillis();
  INVARIANT(!g_disable_xpartition_txn || customerWarehouseID == warehouse_id);

  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 10
  //   max_read_set_size : 71
  //   max_write_set_size : 1
  //   num_txn_contexts : 5
  void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_TPCC_PAYMENT);
  scoped_str_arena s_arena(arena);

  scoped_multilock<spinlock> mlock;
  if (g_enable_partition_locks) {
    mlock.enq(LockForPartition(warehouse_id));
    if (PartitionId(customerWarehouseID) != PartitionId(warehouse_id))
      mlock.enq(LockForPartition(customerWarehouseID));
    mlock.multilock();
  }
  if (customerWarehouseID != WarehouseLocal2Global(warehouse_id))
    ++evt_tpcc_cross_partition_payment_txns;
  try {
    ssize_t ret = 0;

    const warehouse::key k_w(warehouse_id);
    ALWAYS_ERROR(tbl_warehouse(warehouse_id)->get(txn, Encode(obj_key0, k_w), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_payment_failed+=1;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    warehouse::value v_w_temp;
    const warehouse::value *v_w = Decode(obj_v, v_w_temp);
    checker::SanityCheckWarehouse(&k_w, v_w);

    warehouse::value v_w_new(*v_w);
    v_w_new.w_ytd += paymentAmount;
    tbl_warehouse(warehouse_id)->put(txn, Encode(str(), k_w), Encode(str(), v_w_new));

    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ERROR(tbl_district(warehouse_id)->get(txn, Encode(obj_key0, k_d), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_payment_failed+=1;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
    district::value v_d_temp;
    const district::value *v_d = Decode(obj_v, v_d_temp);
    checker::SanityCheckDistrict(&k_d, v_d);

    district::value v_d_new(*v_d);
    v_d_new.d_ytd += paymentAmount;
    tbl_district(warehouse_id)->put(txn, Encode(str(), k_d), Encode(str(), v_d_new));

    customer::key k_c;
    customer::value v_c;
    if (RandomNumber(r, 1, 100) <= 60) {
      // cust by name
      uint8_t lastname_buf[CustomerLastNameMaxSize + 1];
      static_assert(sizeof(lastname_buf) == 16, "xx");
      NDB_MEMSET(lastname_buf, 0, sizeof(lastname_buf));
      GetNonUniformCustomerLastNameRun(lastname_buf, r);

      static const string zeros(16, 0);
      static const string ones(16, 255);

      customer_name_idx::key k_c_idx_0;
      k_c_idx_0.c_w_id = WarehouseGlobal2Local(customerWarehouseID);
      k_c_idx_0.c_d_id = customerDistrictID;
      k_c_idx_0.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_0.c_first.assign(zeros);

      customer_name_idx::key k_c_idx_1;
      k_c_idx_1.c_w_id = WarehouseGlobal2Local(customerWarehouseID);
      k_c_idx_1.c_d_id = customerDistrictID;
      k_c_idx_1.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_1.c_first.assign(ones);

      static_limit_callback<NMaxCustomerIdxScanElems> c(s_arena.get(), true); // probably a safe bet for now
      // this huge c_data might cause so many ISSUES - on real machines
      // NO need to worry about at this moment
      if (WarehouseInShard(customerWarehouseID, shardIndex)) {
        tbl_customer_name_idx(WarehouseGlobal2Local(customerWarehouseID))->scan(txn, Encode(obj_key0, k_c_idx_0), &Encode(obj_key1, k_c_idx_1), c, s_arena.get());
        ALWAYS_ERROR(c.size() > 0);
        INVARIANT(c.size() < NMaxCustomerIdxScanElems); // we should detect this
        int index = c.size() / 2;
        if (c.size() % 2 == 0)
          index--;
        customer_name_idx::value v_c_idx_temp;
        const customer_name_idx::value *v_c_idx = Decode(*c.values[index].second, v_c_idx_temp);
        k_c.c_id = v_c_idx->c_id;
      } else {
        dummy_tbl_customer_name_idx(customerWarehouseID)->scanRemoteOne(txn, Encode(obj_key0, k_c_idx_0), Encode(obj_key1, k_c_idx_1), obj_v);
        customer_name_idx::value v_c_idx_temp;
        const customer_name_idx::value *v_c_idx = Decode(obj_v, v_c_idx_temp);
        k_c.c_id = v_c_idx->c_id;
      }

      k_c.c_w_id = WarehouseGlobal2Local(customerWarehouseID);
      k_c.c_d_id = customerDistrictID;
      
      if (WarehouseInShard(customerWarehouseID, shardIndex)) {
        ALWAYS_ERROR(tbl_customer(WarehouseGlobal2Local(customerWarehouseID))->get(txn, EncodeK(obj_key0, k_c), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_payment_failed+=1;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      } else {
        ALWAYS_ERROR(dummy_tbl_customer(customerWarehouseID)->get(txn, EncodeK(obj_key0, k_c), obj_v));
      }

      Decode(obj_v, v_c);
    } /*60% END*/ else {
      // cust by ID
      const uint customerID = GetCustomerId(r);
      k_c.c_w_id = WarehouseGlobal2Local(customerWarehouseID);
      k_c.c_d_id = customerDistrictID;
      k_c.c_id = customerID;
      
      if (WarehouseInShard(customerWarehouseID, shardIndex)) {
        ALWAYS_ERROR(tbl_customer(WarehouseGlobal2Local(customerWarehouseID))->get(txn, EncodeK(obj_key0, k_c), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_payment_failed+=1;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      } else {
        ALWAYS_ERROR(dummy_tbl_customer(customerWarehouseID)->get(txn, EncodeK(obj_key0, k_c), obj_v));
      }
      Decode(obj_v, v_c);
    } /*40% END*/
    checker::SanityCheckCustomer(&k_c, &v_c);
    customer::value v_c_new(v_c);

    v_c_new.c_balance -= paymentAmount;
    v_c_new.c_ytd_payment += paymentAmount;
    v_c_new.c_payment_cnt++;

    if (WarehouseInShard(customerWarehouseID, shardIndex)) {
      tbl_customer(WarehouseGlobal2Local(customerWarehouseID))->put(txn, EncodeK(str(), k_c), Encode(str(), v_c_new));
    } else {
      dummy_tbl_customer(customerWarehouseID)->put(txn, EncodeK(str(), k_c), Encode(str(), v_c_new));
    }
    const history::key k_h(k_c.c_d_id, k_c.c_w_id, k_c.c_id, districtID, warehouse_id, ts);
    history::value v_h;
    v_h.h_amount = paymentAmount;
    v_h.h_data.resize_junk(v_h.h_data.max_size());
    int n = snprintf((char *) v_h.h_data.data(), v_h.h_data.max_size() + 1,
                     "%.10s    %.10s",
                     v_w->w_name.c_str(),
                     v_d->d_name.c_str());
    v_h.h_data.resize_junk(min(static_cast<size_t>(n), v_h.h_data.max_size()));

    const size_t history_sz = Size(v_h);
    tbl_history(warehouse_id)->insert(txn, EncodeK(str(), k_h), Encode(str(), v_h));
    ret += history_sz;

    if (strncmp(v_c.c_credit.data(), "BC", 2) == 0) {
      k_c.c_d_id = k_c.c_d_id+100; // update key in the customer data
      customer_data::value v_c_data;

      if (WarehouseInShard(customerWarehouseID, shardIndex)) {
        ALWAYS_ERROR(tbl_customer(WarehouseGlobal2Local(customerWarehouseID))->get(txn, EncodeK(obj_key0, k_c), obj_v));
        if(TThread::transget_without_stable){TThread::transget_without_stable=false;counter_payment_failed+=1;}
        if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,isRemote?1:0);}
      } else {
        ALWAYS_ERROR(dummy_tbl_customer(customerWarehouseID)->get(txn, EncodeK(obj_key0, k_c), obj_v));
      }
      Decode(obj_v, v_c_data);
      customer_data::value v_c_data_new(v_c_data);
      v_c_data_new.c_data.trim();
      char buf[301]; 
      // int n = snprintf(buf, 6, "%d", k_c.c_id);
      // n += snprintf(buf+n, 6, " %d", k_c.c_d_id);
      // n += snprintf(buf+n, 6, " %d", k_c.c_w_id);
      // n += snprintf(buf+n, 6, " %d", districtID);
      // n += snprintf(buf+n, 6, " %d", warehouse_id);
      // n += snprintf(buf+n, 6, " %f", paymentAmount);
      // n += snprintf(buf+n, 500-36, " %s", v_c_data.c_data.c_str());

      int n = snprintf(buf, sizeof(buf), "%d %d %d %d %d %d | %s",
                       k_c.c_id,
                       k_c.c_d_id,
                       k_c.c_w_id,
                       districtID,
                       warehouse_id,
                       (int)paymentAmount,
                       v_c_data.c_data.c_str());
      v_c_data_new.c_data.resize_junk(
          min(static_cast<size_t>(n), v_c_data_new.c_data.max_size()));
      NDB_MEMCPY((void *) v_c_data_new.c_data.data(), &buf[0], v_c_data_new.c_data.size());

      if (WarehouseInShard(customerWarehouseID, shardIndex)) {
        tbl_customer(WarehouseGlobal2Local(customerWarehouseID))->put(txn, EncodeK(str(), k_c), Encode(str(), v_c_data_new));
      } else {
        dummy_tbl_customer(customerWarehouseID)->put(txn, EncodeK(str(), k_c), Encode(str(), v_c_data_new));
      }
    }

    if (likely(db->commit_txn(txn))){
      return txn_result(true, ret * 10 + (isRemote?1:0));
    }
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    if (n==1002) { // same as fasttransport.cc
      counter_payment_failed+=1;
    }
    db->abort_txn_local(txn);
  }
  return txn_result(false, 0 + (isRemote?1:0));
#endif
}

class order_line_nop_callback : public abstract_ordered_index::scan_callback {
public:
  order_line_nop_callback() : n(0) {}
  virtual bool invoke(
      const char *keyp, size_t keylen,
      const string &value)
  {
    INVARIANT(keylen == sizeof(order_line::key));
    order_line::value v_ol_temp;
    const order_line::value *v_ol UNUSED = Decode(value, v_ol_temp);
#ifdef CHECK_INVARIANTS
    order_line::key k_ol_temp;
    const order_line::key *k_ol = Decode(keyp, k_ol_temp);
    checker::SanityCheckOrderLine(k_ol, v_ol);
#endif
    ++n;
    return true;
  }
  size_t n;
};

STATIC_COUNTER_DECL(scopedperf::tod_ctr, order_status_probe0_tod, order_status_probe0_cg)

tpcc_worker::txn_result
tpcc_worker::txn_order_status()
{
  const uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
  const uint districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());

  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 13
  //   max_read_set_size : 81
  //   max_write_set_size : 0
  //   num_txn_contexts : 4
  const uint64_t read_only_mask =
    g_disable_read_only_scans ? 0 : transaction_base::TXN_FLAG_READ_ONLY;
  const abstract_db::TxnProfileHint hint =
    g_disable_read_only_scans ?
      abstract_db::HINT_TPCC_ORDER_STATUS :
      abstract_db::HINT_TPCC_ORDER_STATUS_READ_ONLY;
  void *txn = db->new_txn(txn_flags | read_only_mask, arena, txn_buf(), hint);
  scoped_str_arena s_arena(arena);
  // NB: since txn_order_status() is a RO txn, we assume that
  // locking is un-necessary (since we can just read from some old snapshot)
  try {

    customer::key k_c;
    customer::value v_c;
    if (RandomNumber(r, 1, 100) <= 60) {
      // cust by name
      uint8_t lastname_buf[CustomerLastNameMaxSize + 1];
      static_assert(sizeof(lastname_buf) == 16, "xx");
      NDB_MEMSET(lastname_buf, 0, sizeof(lastname_buf));
      GetNonUniformCustomerLastNameRun(lastname_buf, r);

      static const string zeros(16, 0);
      static const string ones(16, 255);

      customer_name_idx::key k_c_idx_0;
      k_c_idx_0.c_w_id = warehouse_id;
      k_c_idx_0.c_d_id = districtID;
      k_c_idx_0.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_0.c_first.assign(zeros);

      customer_name_idx::key k_c_idx_1;
      k_c_idx_1.c_w_id = warehouse_id;
      k_c_idx_1.c_d_id = districtID;
      k_c_idx_1.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_1.c_first.assign(ones);

      static_limit_callback<NMaxCustomerIdxScanElems> c(s_arena.get(), true); // probably a safe bet for now
      tbl_customer_name_idx(warehouse_id)->scan(txn, Encode(obj_key0, k_c_idx_0), &Encode(obj_key1, k_c_idx_1), c, s_arena.get());
      ALWAYS_ERROR(c.size() > 0);
      INVARIANT(c.size() < NMaxCustomerIdxScanElems); // we should detect this
      int index = c.size() / 2;
      if (c.size() % 2 == 0)
        index--;
      //evt_avg_cust_name_idx_scan_size.offer(c.size());

      customer_name_idx::value v_c_idx_temp;
      const customer_name_idx::value *v_c_idx = Decode(*c.values[index].second, v_c_idx_temp);

      k_c.c_w_id = warehouse_id;
      k_c.c_d_id = districtID;
      k_c.c_id = v_c_idx->c_id;
      ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
      if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
      if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}
      Decode(obj_v, v_c);

    } else {
      // cust by ID
      const uint customerID = GetCustomerId(r);
      k_c.c_w_id = warehouse_id;
      k_c.c_d_id = districtID;
      k_c.c_id = customerID;
      ALWAYS_ERROR(tbl_customer(warehouse_id)->get(txn, EncodeK(obj_key0, k_c), obj_v));
      if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
      if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}
      Decode(obj_v, v_c);
    }
    checker::SanityCheckCustomer(&k_c, &v_c);

    string *newest_o_c_id = s_arena.get()->next();
    if (g_order_status_scan_hack) {
      // XXX(stephentu): HACK- we bound the # of elems returned by this scan to
      // 15- this is because we don't have reverse scans. In an ideal system, a
      // reverse scan would only need to read 1 btree node. We could simulate a
      // lookup by only reading the first element- but then we would *always*
      // read the first order by any customer.  To make this more interesting, we
      // randomly select which elem to pick within the 1st or 2nd btree nodes.
      // This is obviously a deviation from TPC-C, but it shouldn't make that
      // much of a difference in terms of performance numbers (in fact we are
      // making it worse for us)
      latest_key_callback c_oorder(*newest_o_c_id, (r.next() % 15) + 1);
      const oorder_c_id_idx::key k_oo_idx_0(warehouse_id, districtID, k_c.c_id, 0);
      const oorder_c_id_idx::key k_oo_idx_1(warehouse_id, districtID, k_c.c_id, numeric_limits<int32_t>::max());
      {
        ANON_REGION("OrderStatusOOrderScan:", &order_status_probe0_cg);
        tbl_oorder_c_id_idx(warehouse_id)->scan(txn, Encode(obj_key0, k_oo_idx_0), &Encode(obj_key1, k_oo_idx_1), c_oorder, s_arena.get());
      }
      ALWAYS_ERROR(c_oorder.size());
    } else {
      latest_key_callback c_oorder(*newest_o_c_id, 1);
      const oorder_c_id_idx::key k_oo_idx_hi(warehouse_id, districtID, k_c.c_id, numeric_limits<int32_t>::max());
      tbl_oorder_c_id_idx(warehouse_id)->rscan(txn, Encode(obj_key0, k_oo_idx_hi), nullptr, c_oorder, s_arena.get());
      ALWAYS_ERROR(c_oorder.size() == 1);
    }

    oorder_c_id_idx::key k_oo_idx_temp;
    const oorder_c_id_idx::key *k_oo_idx = Decode(*newest_o_c_id, k_oo_idx_temp);
    const uint o_id = k_oo_idx->o_o_id;

    order_line_nop_callback c_order_line;
    const order_line::key k_ol_0(warehouse_id, districtID, o_id, 0);
    const order_line::key k_ol_1(warehouse_id, districtID, o_id, numeric_limits<int32_t>::max());
    tbl_order_line(warehouse_id)->scan(txn, Encode(obj_key0, k_ol_0), &Encode(obj_key1, k_ol_1), c_order_line, s_arena.get());
    ALWAYS_ERROR(c_order_line.n >= 5 && c_order_line.n <= 15);

    //measure_txn_counters(txn, "txn_order_status");
    if (likely(db->commit_txn(txn)))
      return txn_result(true, 0);
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
  }
  return txn_result(false, 0);
}



class order_line_scan_callback : public abstract_ordered_index::scan_callback {
public:
  order_line_scan_callback() : n(0) {}
  virtual bool invoke(
      const char *keyp, size_t keylen,
      const string &value)
  {
    INVARIANT(keylen == sizeof(order_line::key));
    order_line::value v_ol_temp;
    const order_line::value *v_ol = Decode(value, v_ol_temp);

#ifdef CHECK_INVARIANTS
    order_line::key k_ol_temp;
    const order_line::key *k_ol = Decode(keyp, k_ol_temp);
    checker::SanityCheckOrderLine(k_ol, v_ol);
#endif

    s_i_ids[v_ol->ol_i_id] = 1;
    n++;
    return true;
  }
  size_t n;
  small_unordered_map<uint, bool, 512> s_i_ids;
};

STATIC_COUNTER_DECL(scopedperf::tod_ctr, stock_level_probe0_tod, stock_level_probe0_cg)
STATIC_COUNTER_DECL(scopedperf::tod_ctr, stock_level_probe1_tod, stock_level_probe1_cg)
STATIC_COUNTER_DECL(scopedperf::tod_ctr, stock_level_probe2_tod, stock_level_probe2_cg)

static event_avg_counter evt_avg_stock_level_loop_join_lookups("stock_level_loop_join_lookups");

tpcc_worker::txn_result
tpcc_worker::txn_stock_level()
{
  const uint warehouse_id = PickWarehouseId(r, warehouse_id_start, warehouse_id_end);
  const uint threshold = RandomNumber(r, 10, 20);
  const uint districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());

  // verify the data
  //int cnt=0;
  //if (cluster.compare("learner")==0){
  //  Warning("verify warehouse-id: %d", warehouse_id);
  //  for (int i=1;i<=NumItems();i++){
  //    void *txn = db->new_txn(txn_flags, arena, txn_buf(), abstract_db::HINT_DEFAULT);
  //    const stock::key k_s(warehouse_id, i);
  //    const size_t nbytesread = serializer<int16_t, true>::max_nbytes();
  //    auto ret=tbl_stock(warehouse_id)->get(txn, EncodeK(obj_key0, k_s), obj_v, nbytesread);
  //    if (!ret){
  //      cnt++;
  //      //Warning("# can't find i-id:%d",i);
  //    }
  //    db->commit_txn(txn);
  //  }
  //  Warning("verify pass, warehouse_id:%d, cnt:%d", warehouse_id, cnt);
  //  sleep(100);
  //}

  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 19
  //   max_read_set_size : 241
  //   max_write_set_size : 0
  //   n_node_scan_large_instances : 1
  //   n_read_set_large_instances : 2
  //   num_txn_contexts : 3
  const uint64_t read_only_mask =
    g_disable_read_only_scans ? 0 : transaction_base::TXN_FLAG_READ_ONLY;
  const abstract_db::TxnProfileHint hint =
    g_disable_read_only_scans ?
      abstract_db::HINT_TPCC_STOCK_LEVEL :
      abstract_db::HINT_TPCC_STOCK_LEVEL_READ_ONLY;
  void *txn = db->new_txn(txn_flags | read_only_mask, arena, txn_buf(), hint);
  scoped_str_arena s_arena(arena);
  // NB: since txn_stock_level() is a RO txn, we assume that
  // locking is un-necessary (since we can just read from some old snapshot)
  try {
    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ERROR(tbl_district(warehouse_id)->get(txn, Encode(obj_key0, k_d), obj_v));
    if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
    if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}
    district::value v_d_temp;
    const district::value *v_d = Decode(obj_v, v_d_temp);
    checker::SanityCheckDistrict(&k_d, v_d);

    const uint64_t cur_next_o_id = g_new_order_fast_id_gen ?
      NewOrderIdHolder(warehouse_id, districtID).load(memory_order_acquire) :
      v_d->d_next_o_id;

    // manual joins are fun!
    order_line_scan_callback c;
    const int32_t lower = cur_next_o_id >= 20 ? (cur_next_o_id - 20) : 0;
    const order_line::key k_ol_0(warehouse_id, districtID, lower, 0);
    const order_line::key k_ol_1(warehouse_id, districtID, cur_next_o_id, 0);
    {
      ANON_REGION("StockLevelOrderLineScan:", &stock_level_probe0_cg);
      tbl_order_line(warehouse_id)->scan(txn, Encode(obj_key0, k_ol_0), &Encode(obj_key1, k_ol_1), c, s_arena.get());
    }
    {
      small_unordered_map<uint, bool, 512> s_i_ids_distinct;
      for (auto &p : c.s_i_ids) {
        ANON_REGION("StockLevelLoopJoinIter:", &stock_level_probe1_cg);

        const size_t nbytesread = serializer<int16_t, true>::max_nbytes();

        const stock::key k_s(warehouse_id, p.first);
        INVARIANT(p.first >= 1 && p.first <= NumItems());
        {
          ANON_REGION("StockLevelLoopJoinGet:", &stock_level_probe2_cg);
          auto ret=tbl_stock(warehouse_id)->get(txn, EncodeK(obj_key0, k_s), obj_v, nbytesread);
          if(TThread::transget_without_stable){TThread::transget_without_stable=false;}
          if(TThread::transget_without_throw){TThread::transget_without_throw=false;db->abort_txn_local(txn);return txn_result(false,0);}
          if(!ret){ 
            Warning("ERROR warehouse_id:%d, cid:%d, maxbytes:%d", warehouse_id, p.first,nbytesread);
          }
          ALWAYS_ERROR(ret);
        }
       // INVARIANT(obj_v.size() <= nbytesread);
        const uint8_t *ptr = (const uint8_t *) obj_v.data();
        int16_t i16tmp;
        ptr = serializer<int16_t, true>::read(ptr, &i16tmp);
        if (i16tmp < int(threshold))
          s_i_ids_distinct[p.first] = 1;
      }
      evt_avg_stock_level_loop_join_lookups.offer(c.s_i_ids.size());
      // NB(stephentu): s_i_ids_distinct.size() is the computed result of this txn
    }
    //measure_txn_counters(txn, "txn_stock_level");
    if (likely(db->commit_txn(txn)))
      return txn_result(true, 0);
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
  } catch (int n) {
    db->abort_txn(txn);
 }
  return txn_result(false, 0);
}

template <typename T>
static vector<T>
unique_filter(const vector<T> &v)
{
  set<T> seen;
  vector<T> ret;
  for (auto &e : v)
    if (!seen.count(e)) {
      ret.emplace_back(e);
      seen.insert(e);
    }
  return ret;
}

class tpcc_bench_runner : public bench_runner {
private:

  static bool
  IsTableReadOnly(const char *name)
  {
    return strcmp("item", name) == 0;
  }

  static bool
  IsTableAppendOnly(const char *name)
  {
    return strcmp("history", name) == 0 ||
           strcmp("oorder_c_id_idx", name) == 0;
  }

  static bool
  UseHashtable(const char *name)
  {
#if !HASHTABLE
    return false;
#endif
    return strcmp("customer", name) == 0 || 
	   //strcmp("district", name) == 0 ||
	   strcmp("history", name) == 0 ||
	   strcmp("item", name) == 0 ||
           strcmp("oorder", name) == 0 ||
	   strcmp("stock", name) == 0 ||
	   //strcmp("stock_data", name) == 0 ||
	   //strcmp("warehouse", name) == 0 ||
	0;
  }

  static vector<abstract_ordered_index *>
  OpenTablesForTablespace(abstract_db *db, const char *name, size_t expected_size)
  {
    const bool is_read_only = IsTableReadOnly(name);
    const bool is_append_only = IsTableAppendOnly(name);
    const bool use_hashtable = UseHashtable(name); 
    const string s_name(name);
    vector<abstract_ordered_index *> ret(NumWarehouses());
    if (g_enable_separate_tree_per_partition && !is_read_only) {
      if (NumWarehouses() <= nthreads) {
        for (size_t i = 0; i < NumWarehouses(); i++)
          ret[i] = db->open_index(s_name + "_" + to_string(i), expected_size, is_append_only, use_hashtable);
      } else {
        const unsigned nwhse_per_partition = NumWarehouses() / nthreads;
        for (size_t partid = 0; partid < nthreads; partid++) {
          const unsigned wstart = partid * nwhse_per_partition;
          const unsigned wend   = (partid + 1 == nthreads) ?
            NumWarehouses() : (partid + 1) * nwhse_per_partition;
          abstract_ordered_index *idx =
            db->open_index(s_name + "_" + to_string(partid), expected_size, is_append_only, use_hashtable);
          for (size_t i = wstart; i < wend; i++)
            ret[i] = idx;
        }
      }
    } else {
      abstract_ordered_index *idx = db->open_index(s_name, expected_size, is_append_only, use_hashtable);
      for (size_t i = 0; i < NumWarehouses(); i++)
        ret[i] = idx;
    }
    return ret;
  }

  static vector<abstract_ordered_index *>
  OpenTablesForTablespaceDummy(abstract_db *db, const char *name, size_t expected_size)
  {
    const bool is_read_only = IsTableReadOnly(name);
    const bool is_append_only = IsTableAppendOnly(name);
    const bool use_hashtable = UseHashtable(name); 
    const string s_name(name);
    vector<abstract_ordered_index *> ret(NumWarehousesTotal());
    for (size_t i = 0; i < NumWarehousesTotal(); i++) {
      int global_wid=i+1;
      abstract_ordered_index *idx = db->open_index(s_name + "_dummy_" + to_string(global_wid), expected_size, is_append_only, use_hashtable);
      ret[i] = idx;
    }
    return ret;
  }

public:
  tpcc_bench_runner(abstract_db *db, bool failure=false)
    : bench_runner(db)
  {
    if (failure) {
      printf("reinitializing partitions under failure\n");
        string nCount[12] = {"customer", "customer_name_idx", "district", "history", "new_order", "oorder", 
                             "oorder_c_id_idx", "order_line", "stock", "stock_data", "warehouse", "item"};

        int table_id = 0;
        for (int i=0; i<12; i++) {
            if (nCount[i] != "item") {
                vector<abstract_ordered_index *> ret(nthreads);
                for (int j=0; j<nthreads; j++) {
                    table_id += 1;
                    ret[j] = db->open_index(table_id) ;
                }
                partitions[nCount[i]] = ret;
            } else {
                vector<abstract_ordered_index *> ret(nthreads);
                table_id += 1;
                for (int j=0; j<nthreads; j++) {
                    ret[j] = db->open_index(table_id) ;
                }
                partitions[nCount[i]] = ret;
            }
        }
      printf("reinitializing partitions under failure - DONE\n");  
    } else {
#define OPEN_TABLESPACE_X(x) \
    partitions[#x] = OpenTablesForTablespace(db, #x, sizeof(x));

    TPCC_TABLE_LIST(OPEN_TABLESPACE_X);

#undef OPEN_TABLESPACE_X
    }

#define DUMMY_OPEN_TABLESPACE_X(x) \
    dummy_partitions[#x] = OpenTablesForTablespaceDummy(db, #x, sizeof(x));

    TPCC_TABLE_LIST(DUMMY_OPEN_TABLESPACE_X);

#undef DUMMY_OPEN_TABLESPACE_X

    for (auto &t : partitions) {
      auto v = unique_filter(t.second);
      for (size_t i = 0; i < v.size(); i++) {
        open_tables[t.first + "_" + to_string(i)] = v[i];
        //std::cout<<"Table: "<<t.first + "_" + to_string(i)<<", id:"<<v[i]->get_table_id()<<std::endl;
      }
    }

    if (g_enable_partition_locks) {
      static_assert(sizeof(aligned_padded_elem<spinlock>) == CACHELINE_SIZE, "xx");
      void * const px = memalign(CACHELINE_SIZE, sizeof(aligned_padded_elem<spinlock>) * nthreads);
      ALWAYS_ERROR(px);
      ALWAYS_ERROR(reinterpret_cast<uintptr_t>(px) % CACHELINE_SIZE == 0);
      g_partition_locks = reinterpret_cast<aligned_padded_elem<spinlock> *>(px);
      for (size_t i = 0; i < nthreads; i++) {
        new (&g_partition_locks[i]) aligned_padded_elem<spinlock>();
        ALWAYS_ERROR(!g_partition_locks[i].elem.is_locked());
      }
    }

    if (g_new_order_fast_id_gen) {
      void * const px =
        memalign(
            CACHELINE_SIZE,
            sizeof(aligned_padded_elem<atomic<uint64_t>>) *
              NumWarehouses() * NumDistrictsPerWarehouse());
      g_district_ids = reinterpret_cast<aligned_padded_elem<atomic<uint64_t>> *>(px);
      for (size_t i = 0; i < NumWarehouses() * NumDistrictsPerWarehouse(); i++)
        new (&g_district_ids[i]) atomic<uint64_t>(3001);
    }
    helper_threads.resize(NumWarehousesTotal());
    server_transports.resize(num_erpc_server);
  }

  void static helper_server(int g_wid, // server_id=g_wid-1;
                          std::string cluster,
                          int running_shardIndex, // running shardIndex
                          int num_warehouses,
                          transport::Configuration *config,
                          abstract_db *db,
                          srolis::HelperQueue *queue,
                          srolis::HelperQueue *queue_response,
                          const map<string, abstract_ordered_index *> &open_tables,
                          const map<string, vector<abstract_ordered_index *>> &partitions,
                          const map<string, vector<abstract_ordered_index *>> &dummy_partitions) {
    /**
     * weihshen:
     *  relationship between g_wid and (shardIndex, par_id)
     *    shardIndex * Numwarehouses() + par_id + 1 == g_wid
     */
    scoped_db_thread_ctx ctx(db, true, 1);  // invoke thread_init
    TThread::set_mode(1);
#if defined(DISABLE_MULTI_VERSION)
    TThread::disable_multiversion();
#else
    TThread::enable_multiverison();
#endif
    int shardIdx = (g_wid - 1)/num_warehouses;
    int par_id = (g_wid - 1)%num_warehouses;
    TThread::set_shard_index(running_shardIndex);  // the running shardIndex
    TThread::set_pid(par_id);
    TThread::set_nshards(config->nshards);
    srolis::ShardServer *ss=new srolis::ShardServer(config->configFile,
                                                    running_shardIndex, shardIdx, par_id);
    ss->Register(db, queue, queue_response, open_tables, partitions, dummy_partitions);
    ss->Run();  // it is event driven!
  }

  // setup (n-1)*warehouses helper threads
  void setup_helper() {
    for (int i=0; i<NumWarehousesTotal(); i++) {
      if (i / NumWarehouses() == shardIndex) continue;

      helper_threads[i] = std::thread(helper_server,
                                      i+1, cluster, shardIndex, NumWarehouses(),
                                      config, db, 
                                      queue_holders[i],
                                      queue_holders_response[i],
                                      open_tables, partitions, dummy_partitions);
      pthread_setname_np(helper_threads[i].native_handle(), ("helper_"+std::to_string(i)).c_str());
      // int core_id = shardIndex * 64 + config->warehouses+1;
      // cpu_set_t cpuset;
      // CPU_ZERO(&cpuset);
      // CPU_SET(core_id, &cpuset);
      // pthread_setaffinity_np(helper_threads[i].native_handle(),sizeof(cpu_set_t), &cpuset);
    }
  }

  void static erpc_server(std::string cluster,
                          int running_shardIndex, // running shardIndex
                          int num_warehouses,
                          transport::Configuration *config,
                          int alpha=0) {
    std::string local_uri = config->shard(running_shardIndex, srolis::convertCluster(cluster)).host;
    int base=5;
    int id = num_warehouses+base+alpha;
    server_transports[alpha] = new FastTransport(config->configFile,
                                  local_uri,  // local_uri
                                  cluster,
                                  1, 12,
                                  0,          // physPort
                                  0,          // numa node
                                  running_shardIndex, // used to get basic port
                                  id);
    for (int i=0; i<NumWarehousesTotal(); i++) {
      if (i / NumWarehouses() == running_shardIndex) continue;
      if (i%num_erpc_server==alpha){
        auto *it = new srolis::HelperQueue(i,true);
        server_transports[alpha]->c->queue_holders[i] = it;
        auto *it_res = new srolis::HelperQueue(i,false);
        server_transports[alpha]->c->queue_holders_response[i] = it_res;
      }
    }
    set_server_transport.fetch_add(1);
    server_transports[alpha]->Run();
    Notice("the erpc_server is terminated on shardIdx:%d, alpha:%d!", running_shardIndex, alpha);
  }

  // setup erpc server: at this moment, we only have one erpc server (id=warehouses)
  //  warehouses clients(id=0~warehouses-1)
  void setup_erpc_server()
  {
    for (int i=0;i<num_erpc_server;++i){
      auto t=std::thread(erpc_server, cluster, shardIndex, NumWarehouses(), config, i);
      pthread_setname_np(t.native_handle(), "erpc_server");
      t.detach();
    }
    while(set_server_transport<num_erpc_server) { sleep(0); }
    for (int i=0; i<NumWarehousesTotal(); i++) {
      if (i / NumWarehouses() == shardIndex) continue;
      queue_holders[i] = server_transports[i%num_erpc_server]->c->queue_holders[i];
      queue_holders_response[i] = server_transports[i%num_erpc_server]->c->queue_holders_response[i];
    } 
    // int core_id = shardIndex * 64 + config->warehouses;
    // cpu_set_t cpuset;
    // CPU_ZERO(&cpuset);
    // CPU_SET(core_id, &cpuset);
    // pthread_setaffinity_np(t.native_handle(),sizeof(cpu_set_t), &cpuset);
  }

  void cleanup()
  {
    Warning("stop the server rpc");
    for (int i=0; i<num_erpc_server; ++i)
      server_transports[i]->Stop();
  }

protected:
  virtual vector<bench_loader *>
  make_loaders()
  {
    vector<bench_loader *> ret;
#if defined(SIMPLE_WORKLOAD)
    ret.push_back(new tpcc_warehouse_loader(9324, db, open_tables, partitions, dummy_partitions));
#elif defined(MICRO_BENCHMARK)
    ret.push_back(new tpcc_warehouse_loader(9324, db, open_tables, partitions, dummy_partitions));
#else
    ret.push_back(new tpcc_warehouse_loader(9324, db, open_tables, partitions, dummy_partitions));
    ret.push_back(new tpcc_item_loader(235443, db, open_tables, partitions, dummy_partitions));
    if (enable_parallel_loading) {
      fast_random r(89785943);
      for (uint i = 1; i <= NumWarehouses(); i++)
        ret.push_back(new tpcc_stock_loader(r.next(), db, open_tables, partitions, dummy_partitions, i));
    } else {
      ret.push_back(new tpcc_stock_loader(89785943, db, open_tables, partitions, dummy_partitions, -1));
    }
    ret.push_back(new tpcc_district_loader(129856349, db, open_tables, partitions, dummy_partitions));
    if (enable_parallel_loading) {
      fast_random r(923587856425);
      for (uint i = 1; i <= NumWarehouses(); i++)
        ret.push_back(new tpcc_customer_loader(r.next(), db, open_tables, partitions, dummy_partitions, i));
    } else {
      ret.push_back(new tpcc_customer_loader(923587856425, db, open_tables, partitions, dummy_partitions, -1));
    }
    if (enable_parallel_loading) {
      fast_random r(2343352);
      for (uint i = 1; i <= NumWarehouses(); i++)
        ret.push_back(new tpcc_order_loader(r.next(), db, open_tables, partitions, dummy_partitions, i));
    } else {
      ret.push_back(new tpcc_order_loader(2343352, db, open_tables, partitions, dummy_partitions, -1));
    }
#endif
    return ret;
  }

  virtual vector<bench_worker *>
  make_workers()
  {
    const unsigned alignment = coreid::num_cpus_online();
    const int blockstart =
      coreid::allocate_contiguous_aligned_block(nthreads, alignment);
    ALWAYS_ERROR(blockstart >= 0);
    ALWAYS_ERROR((blockstart % alignment) == 0);
    fast_random r(23984543);
    vector<bench_worker *> ret;
    if (NumWarehouses() <= nthreads) {
      for (size_t i = 0; i < nthreads; i++) {
        ret.push_back(
          new tpcc_worker(
            blockstart + i,
            r.next(), db, open_tables, partitions, dummy_partitions, 
            &barrier_a, &barrier_b,
            (i % NumWarehouses()) + 1, (i % NumWarehouses()) + 2));
      }
    } else {
      const unsigned nwhse_per_partition = NumWarehouses() / nthreads;
      for (size_t i = 0; i < nthreads; i++) {
        const unsigned wstart = i * nwhse_per_partition;
        const unsigned wend   = (i + 1 == nthreads) ?
          NumWarehouses() : (i + 1) * nwhse_per_partition;
        ret.push_back(
          new tpcc_worker(
            blockstart + i,
            r.next(), db, open_tables, partitions, dummy_partitions,
            &barrier_a, &barrier_b, wstart+1, wend+1));
      }
    }
    return ret;
  }

private:
  map<string, vector<abstract_ordered_index *>> partitions;
  // dummy_partitions has exact same key and number of items as partitions, but doesn't store any data
  map<string, vector<abstract_ordered_index *>> dummy_partitions;
  std::vector<std::thread> helper_threads;
  // hold pointer: identical to those c in several erpc_servers (we only have one erpc_server now)
  std::unordered_map<uint16_t, srolis::HelperQueue*> queue_holders;
  std::unordered_map<uint16_t, srolis::HelperQueue*> queue_holders_response;
};

bench_runner*
tpcc_do_test(abstract_db *db, int argc, char **argv, int run = 0, bench_runner *rc = NULL)
{
  if (run==1){
    ((tpcc_bench_runner*)rc)->run();
    ((tpcc_bench_runner*)rc)->cleanup();
#if defined(PAXOS_LIB_ENABLED)
    Warning("######--------------###### send endLlogs #####---------------######");
    std::string endLogInd = "";
    for (int i = 0; i < nthreads; i++)
        add_log_to_nc((char *)endLogInd.c_str(), 0, i);

    // vector<std::thread> wait_threads;
    // for (int i = 0; i < nthreads; i++)
    // {
    //     wait_threads.push_back(std::thread([i]() {
    //        std::cout << "starting wait for par_id: " << i << std::endl;
    //        wait_for_submit(i); 
    //     }));
    // }
    // for (auto &th : wait_threads)
    // {
    //     th.join();
    // }
#endif
    return rc; // rc is same object as r below
  }

  auto x0 = std::chrono::high_resolution_clock::now() ;
  // parse options
  optind = 1;
  bool did_spec_remote_pct = false;
  g_enable_separate_tree_per_partition = 1;  // keep it same as rolis, but in fact it doesn't matter
  while (1) {
    static struct option long_options[] =
    {
      {"disable-cross-partition-transactions" , no_argument       , &g_disable_xpartition_txn             , 1}   ,
      {"disable-read-only-snapshots"          , no_argument       , &g_disable_read_only_scans            , 1}   ,
      {"enable-partition-locks"               , no_argument       , &g_enable_partition_locks             , 1}   ,
      {"enable-separate-tree-per-partition"   , no_argument       , &g_enable_separate_tree_per_partition , 1}   ,
      {"f_mode"                               , required_argument , 0                                     , 'm'} ,
      {"new-order-remote-item-pct"            , required_argument , 0                                     , 'r'} ,
      {"new-order-fast-id-gen"                , no_argument       , &g_new_order_fast_id_gen              , 1}   ,
      {"uniform-item-dist"                    , no_argument       , &g_uniform_item_dist                  , 1}   ,
      {"order-status-scan-hack"               , no_argument       , &g_order_status_scan_hack             , 1}   ,
      {"workload-mix"                         , required_argument , 0                                     , 'w'} ,
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "r:", long_options, &option_index);
    if (c == -1)
      break;
    switch (c) {
    case 0:
      if (long_options[option_index].flag != 0)
        break;
      abort();
      break;

    case 'r':
      g_new_order_remote_item_pct = strtoul(optarg, NULL, 10);
      ALWAYS_ERROR(g_new_order_remote_item_pct >= 0 && g_new_order_remote_item_pct <= 100);
      did_spec_remote_pct = true;
      break;

    case 'm':
      if(optarg) {
          f_mode = strtoul(optarg, NULL, 10);
      }
      break;

    case 'w':
      {
        const vector<string> toks = split(optarg, ',');
        ALWAYS_ERROR(toks.size() == ARRAY_NELEMS(g_txn_workload_mix));
        unsigned s = 0;
        for (size_t i = 0; i < toks.size(); i++) {
          unsigned p = strtoul(toks[i].c_str(), nullptr, 10);
          ALWAYS_ERROR(p >= 0 && p <= 100);
          s += p;
          g_txn_workload_mix[i] = p;
        }
        ALWAYS_ERROR(s == 100);
      }
      break;

    case '?':
      /* getopt_long already printed an error message. */
      exit(1);

    default:
      abort();
    }
  }

  if (did_spec_remote_pct && g_disable_xpartition_txn) {
    cerr << "WARNING: --new-order-remote-item-pct given with --disable-cross-partition-transactions" << endl;
    cerr << "  --new-order-remote-item-pct will have no effect" << endl;
  }

  if (verbose) {
    cerr << "tpcc settings:" << endl;
    cerr << "  cross_partition_transactions : " << !g_disable_xpartition_txn << endl;
    cerr << "  read_only_snapshots          : " << !g_disable_read_only_scans << endl;
    cerr << "  partition_locks              : " << g_enable_partition_locks << endl;
    cerr << "  separate_tree_per_partition  : " << g_enable_separate_tree_per_partition << endl;
    cerr << "  new_order_remote_item_pct    : " << g_new_order_remote_item_pct << endl;
    cerr << "  new_order_fast_id_gen        : " << g_new_order_fast_id_gen << endl;
    cerr << "  uniform_item_dist            : " << g_uniform_item_dist << endl;
    cerr << "  order_status_scan_hack       : " << g_order_status_scan_hack << endl;
    cerr << "  workload_mix                 : " <<
      format_list(g_txn_workload_mix,
                  g_txn_workload_mix + ARRAY_NELEMS(g_txn_workload_mix)) << endl;
  }

  tpcc_bench_runner *r = NULL;
  r = new tpcc_bench_runner(db, f_mode==1);
  // the erpc server and redirect requests to helper threads on the server side
  r->setup_erpc_server();
  r->setup_helper(); // the helper threads on the server side
  r->f_mode=f_mode;
  auto x1 = std::chrono::high_resolution_clock::now() ;
  printf("start worker:%d\n",
            std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count());
  

  
  return r;
}
