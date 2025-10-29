#pragma once

#include "config.h"
#include "compiler.hh"
#include "rocksdb_persistence_fwd.h"
// #include "small_vector.hh"
#include "TRcu.hh"
#include <algorithm>
#include <functional>
#include <memory>
#include <type_traits>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <atomic>
#include <x86intrin.h>
#include <vector>
#include <cstring> // for memcpy
#include "deptran/s_main.h"
#include "benchmarks/sto/Interface.hh"
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/benchmark_config.h"

// Transaction configuration constants
namespace TransactionConfig {
    // Thread and capacity limits
    static constexpr unsigned MAX_THREAD_COUNT = 460;
    static constexpr unsigned TRANSACTION_SET_INITIAL_CAPACITY = 512;
    static constexpr unsigned TRANSACTION_SET_CHUNK_SIZE = 512;
    static constexpr unsigned TRANSACTION_SET_MAX_CAPACITY = 32768;
    static constexpr unsigned HASH_TABLE_SIZE = 1024;
    static constexpr unsigned HASH_STEP_SIZE = 5;
    static constexpr unsigned HASH_BASE_RESET_THRESHOLD = 32768;
    
    // Memory alignment and sizing
    static constexpr unsigned THREAD_INFO_ALIGNMENT = 128;
    static constexpr unsigned ARRAY_DIMENSION_SIZE = 4098;
    static constexpr unsigned ARRAY_DEPTH_FACTOR = 3;
    static constexpr unsigned SMALL_ARRAY_WIDTH = 8;
    static constexpr unsigned SMALL_ARRAY_DIMENSION = 4096;
    
    // Default values and limits
    static constexpr size_t DEFAULT_SHARD_COUNT = 0;
    static constexpr size_t DEFAULT_BATCH_SIZE = 1;
    static constexpr size_t DEFAULT_MAX_BYTES_SIZE = 300;  // TPC-C default
    static constexpr uint32_t DEFAULT_TIMESTAMP = 0;
    static constexpr size_t INITIAL_POSITION = 0;
    static constexpr size_t INITIAL_ENTRY_COUNT = 0;
    static constexpr double BUFFER_USAGE_THRESHOLD = 0.9;
    static constexpr unsigned PERSISTENCE_LOG_INTERVAL = 1000;
    
    // Spin bounds for different configurations
    static constexpr unsigned SPIN_BOUND_WRITE_EXPBACKOFF = 7;
    static constexpr unsigned SPIN_BOUND_WRITE_NORMAL = 3;
    static constexpr unsigned SPIN_BOUND_WAIT_EXPBACKOFF = 18;
    static constexpr unsigned SPIN_BOUND_WAIT_NORMAL = 20;
    
    // Debug and profiling limits
    static constexpr unsigned TRANSACTION_SIZE_LIMIT = 20000;
    static constexpr double DEBUG_HASH_COLLISION_FRACTION = 0.00001;
    static constexpr double DEBUG_ABORT_FRACTION = 0.0001;
}

#ifndef STO_PROFILE_COUNTERS
#define STO_PROFILE_COUNTERS 0
#endif

#ifndef STO_DEBUG_HASH_COLLISIONS
#define STO_DEBUG_HASH_COLLISIONS 0
#endif
#ifndef STO_DEBUG_HASH_COLLISIONS_FRACTION
#define STO_DEBUG_HASH_COLLISIONS_FRACTION 0.00001
#endif

#ifndef STO_DEBUG_ABORTS
#define STO_DEBUG_ABORTS 0
#endif
#ifndef STO_DEBUG_ABORTS_FRACTION
#define STO_DEBUG_ABORTS_FRACTION 0.0001
#endif

#ifndef STO_SORT_WRITESET
#define STO_SORT_WRITESET 0
#endif

#ifndef STO_SPIN_EXPBACKOFF
#define STO_SPIN_EXPBACKOFF 0
#endif

#ifndef STO_SPIN_BOUND_WRITE
#if STO_SPIN_EXPBACKOFF
#define STO_SPIN_BOUND_WRITE 7
#else
#define STO_SPIN_BOUND_WRITE 3
#endif
#endif

#ifndef STO_SPIN_BOUND_WAIT
#if STO_SPIN_EXPBACKOFF
#define STO_SPIN_BOUND_WAIT 18
#else
#define STO_SPIN_BOUND_WAIT 20
#endif
#endif

#define CONSISTENCY_CHECK 0
#define ASSERT_TX_SIZE 0
#define TRANSACTION_HASHTABLE 1

#if ASSERT_TX_SIZE
#if STO_PROFILE_COUNTERS > 1
#    define TX_SIZE_LIMIT 20000
#    include <cassert>
#else
#    error "ASSERT_TX_SIZE requires STO_PROFILE_COUNTERS > 1!"
#endif
#endif

#include "config.h"

#define MAX_THREADS TransactionConfig::MAX_THREAD_COUNT

// Array size calculations for transaction buffers
// Large array: sufficient for most transaction workloads
#define MAX_ARRAY_SIZE_IN_BYTES  size_t(TransactionConfig::ARRAY_DIMENSION_SIZE)*size_t(TransactionConfig::ARRAY_DEPTH_FACTOR)*size_t(TransactionConfig::ARRAY_DIMENSION_SIZE)*sizeof(char)
// Small array: optimized for remote operations with reduced memory footprint
#define MAX_ARRAY_SIZE_IN_BYTES_SMALL  size_t(TransactionConfig::SMALL_ARRAY_DIMENSION)*size_t(TransactionConfig::ARRAY_DEPTH_FACTOR)*size_t(TransactionConfig::SMALL_ARRAY_WIDTH)*sizeof(char)

void register_sync_util(std::function<int()>);

class StringAllocator{
 private:
  unsigned char *LOG;
//   size_t ul_len = sizeof (unsigned long long int);
  size_t nshards = TransactionConfig::DEFAULT_SHARD_COUNT;
  size_t batch_size = TransactionConfig::DEFAULT_BATCH_SIZE;
  size_t max_bytes_size = TransactionConfig::DEFAULT_MAX_BYTES_SIZE;  // default batching for TPC-C
 
  void freeMemory(){
    resetMemory();
    ::free(LOG);
  }

 public:
  size_t entries;
  size_t curr_pos;
  uint32_t latest_commit_timestamp;  // Single timestamp instead of vector, timestamp*10+epoch

  StringAllocator(size_t shard_count, int max_buffer_size, int batch_size_limit){
    LOG = (unsigned char *) malloc (max_buffer_size);
    curr_pos = TransactionConfig::INITIAL_POSITION;
    entries = TransactionConfig::INITIAL_ENTRY_COUNT;
    nshards = shard_count;
    batch_size = batch_size_limit;
    max_bytes_size = max_buffer_size;
    latest_commit_timestamp = TransactionConfig::DEFAULT_TIMESTAMP;
  }

  ~StringAllocator() {
    if (!BenchmarkConfig::getInstance().getIsReplicated()) {return;}
    size_t current_position = 0;
    unsigned char *queue_log_buffer = getLogOnly(current_position);
    assert(current_position <= max_bytes_size) ;
    if (current_position + 1 > max_bytes_size) {
        Warning("buffer[%d/%d] is not enough!, entries: %d, batch_size: %d", current_position, max_bytes_size, entries, batch_size);
        freeMemory();
        return;
    }
    if(current_position != 0) {
        // Single timestamp system: write single timestamp and latency tracker
        memcpy (queue_log_buffer + current_position, &latest_commit_timestamp, sizeof(uint32_t));
        current_position += sizeof(uint32_t);
        uint32_t start_time = mako::getCurrentTimeMillis();
        memcpy (queue_log_buffer + current_position, &start_time, sizeof(uint32_t));
        current_position += sizeof(uint32_t);
        add_log_to_nc((char *)queue_log_buffer, current_position, TThread::getPartitionID (), batch_size);

#ifndef DISABLE_DISK
        // Asynchronously persist to RocksDB
        auto& persistence_manager = mako::RocksDBPersistence::getInstance();
        uint32_t current_shard_id = BenchmarkConfig::getInstance().getShardIndex();
        static std::atomic<uint64_t> persist_success_count{TransactionConfig::INITIAL_ENTRY_COUNT};
        static std::atomic<uint64_t> persist_fail_count{TransactionConfig::INITIAL_ENTRY_COUNT};
        persistence_manager.persistAsync((const char*)queue_log_buffer, current_position, current_shard_id, TThread::getPartitionID(),
            [](bool success) {
                if (success) {
                    persist_success_count.fetch_add(1, std::memory_order_relaxed);
                    if (persist_success_count % TransactionConfig::PERSISTENCE_LOG_INTERVAL == 0) {
                        std::cout << "[RocksDB] Persisted " << persist_success_count.load()
                                  << " transaction logs to disk" << std::endl;
                    }
                } else {
                    persist_fail_count.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RocksDB] Failed to persist log (total failures: "
                              << persist_fail_count.load() << ")" << std::endl;
                }
            });
#endif
    }
    freeMemory();
  };

//   static size_t kSizeLimit;
//   static void setSTOBatchSize(size_t k){
//     kSizeLimit = k;
//   }
  bool checkPushRequired(){
    if (curr_pos + 1 > max_bytes_size)
        Warning("checkPushRequired: buffer[%d/%d] is not enough!!!!, entries: %d, batch_size: %d", curr_pos, max_bytes_size, entries, batch_size);
	bool ans = entries>=batch_size || curr_pos >= max_bytes_size * TransactionConfig::BUFFER_USAGE_THRESHOLD;
    return ans;
  }
  unsigned char * getLog(int buffer_size=0){
    if(buffer_size == 0)
		return LOG;
    else{
	  return LOG + curr_pos - buffer_size;
    }
  }
  unsigned char * getLogOnly(size_t& position_reference){
    position_reference = curr_pos;
	return LOG;
  }

  void resetMemory(){
    curr_pos = TransactionConfig::INITIAL_POSITION;
    entries = TransactionConfig::INITIAL_ENTRY_COUNT;
    latest_commit_timestamp = TransactionConfig::DEFAULT_TIMESTAMP;
  }
  bool checkLimits(size_t new_log_length){
  	return (curr_pos + new_log_length) < max_bytes_size;
  }

  /*
  inline void bypass_add (const unsigned long long int& value) {
	memcpy (LOG + curr_pos, (char *) &value, ul_len);
	curr_pos+=ul_len;
  }

  inline void increment () {
	entries++;
  }*/

  inline void update_ptr(const size_t& write_position){
    entries++;
    curr_pos = write_position;
  }

  size_t get_max_bytes_size() {
    return max_bytes_size;
  }

  inline void update_commit_id(const uint32_t commit_id) {
    // Single timestamp system: just track the maximum timestamp
    latest_commit_timestamp = std::max(latest_commit_timestamp, commit_id);
  }

  bool add (const char *data_array, int data_length) {
	if(!checkLimits(data_length))
	{
	  std::cout << "[]ERROR <<<<< No more memory in the string pool " << std::endl;
	  return false;
	}
	entries++;
	memcpy (LOG+curr_pos, data_array, data_length);
	curr_pos += data_length;
	return true;
  }
};

inline std::unordered_map<uint32_t, uint32_t> sample_transaction_tracker ;  // local timestamp => updated time (millisecond)

// TRANSACTION macros that can be used to wrap transactional code
#define TRANSACTION                               \
    do {                                          \
        TransactionLoopGuard __txn_guard;         \
        while (1) {                               \
            __txn_guard.start();                  \
            try {
#define RETRY(retry)                              \
                if (__txn_guard.try_commit())     \
                    break;                        \
            } catch (Transaction::Abort e) {      \
            }                                     \
            if (!(retry))                         \
                throw Transaction::Abort();       \
        }                                         \
    } while (0)

// transaction performance counters
enum txp {
    // all logging levels
    txp_total_aborts = 0,
    txp_total_starts,
    txp_commit_time_nonopaque,
    txp_commit_time_aborts,
    txp_max_set,
    txp_hco,
    txp_hco_lock,
    txp_hco_invalid,
    txp_hco_abort,
    // STO_PROFILE_COUNTERS > 1 only
    txp_total_n,
    txp_total_r,
    txp_total_w,
    txp_max_transbuffer,
    txp_total_transbuffer,
    txp_push_abort,
    txp_pop_abort,
    txp_total_check_read,
    txp_total_check_predicate,
    txp_hash_find,
    txp_hash_collision,
    txp_hash_collision2,
    txp_total_searched,
#if !STO_PROFILE_COUNTERS
    txp_count = 0
#elif STO_PROFILE_COUNTERS == 1
    txp_count = txp_hco_abort + 1
#else
    txp_count
#endif
};
typedef uint32_t txp_counter_type;

inline constexpr bool txp_is_max(unsigned counter_type) {
    return counter_type == txp_max_set || counter_type == txp_max_transbuffer;
}

template <unsigned CounterIndex, unsigned MaxCounters, bool IsValid = (CounterIndex < MaxCounters)> struct txp_helper;
template <unsigned CounterIndex, unsigned MaxCounters> struct txp_helper<CounterIndex, MaxCounters, true> {
    static bool counter_exists(unsigned counter_type) {
        return counter_type < MaxCounters;
    }
    static void account_array(txp_counter_type* counter_array, txp_counter_type value) {
        if (txp_is_max(CounterIndex))
            counter_array[CounterIndex] = std::max(counter_array[CounterIndex], value);
        else
            counter_array[CounterIndex] += value;
    }
};
template <unsigned CounterIndex, unsigned MaxCounters> struct txp_helper<CounterIndex, MaxCounters, false> {
    static bool counter_exists(unsigned) {
        return false;
    }
    static void account_array(txp_counter_type*, txp_counter_type) {
    }
};

struct txp_counters {
    txp_counter_type p_[txp_count];
    txp_counters() {
        for (unsigned counter_index = 0; counter_index != txp_count; ++counter_index)
            p_[counter_index] = 0;
    }
    unsigned long long p(int counter_index) {
        return txp_helper<0, txp_count>::counter_exists(counter_index) ? p_[counter_index] : 0;
    }
    void reset() {
        for (int counter_index = 0; counter_index != txp_count; ++counter_index)
            p_[counter_index] = 0;
    }
};


#include "Interface.hh"
#include "TransItem.hh"

void reportPerf();
#define STO_SHUTDOWN() reportPerf()

struct __attribute__((aligned(TransactionConfig::THREAD_INFO_ALIGNMENT))) threadinfo_t {
    using epoch_type = TRcuSet::epoch_type;
    epoch_type epoch;
    TRcuSet rcu_set;
    // Future enhancement: Convert to vectors to support multiple callback registrations
    // Currently supports single callback per event type for simplicity
    std::function<void(void)> trans_start_callback;
    std::function<void(void)> trans_end_callback;
    txp_counters p_;
    threadinfo_t()
        : epoch(0) {
    }
};


class Transaction {
public:
    static constexpr unsigned tset_initial_capacity = TransactionConfig::TRANSACTION_SET_INITIAL_CAPACITY;

    static constexpr unsigned hash_size = TransactionConfig::HASH_TABLE_SIZE;
    static constexpr unsigned hash_step = TransactionConfig::HASH_STEP_SIZE;
    using epoch_type = TRcuSet::epoch_type;
    using signed_epoch_type = TRcuSet::signed_epoch_type;

    static threadinfo_t tinfo[MAX_THREADS];
    static struct epoch_state {
        epoch_type global_epoch; // != 0
        epoch_type active_epoch; // no thread is before this epoch
        TransactionTid::type recent_tid;
        bool run;
    } global_epochs;
    typedef TransactionTid::type tid_type;
private:
    static TransactionTid::type _TID;
public:

    static std::function<void(threadinfo_t::epoch_type)> epoch_advance_callback;

    static txp_counters txp_counters_combined() {
        txp_counters combined_counters;
        for (int thread_index = 0; thread_index != MAX_THREADS; ++thread_index)
            for (int counter_index = 0; counter_index != txp_count; ++counter_index) {
                if (txp_is_max(counter_index))
                    combined_counters.p_[counter_index] = std::max(combined_counters.p_[counter_index], tinfo[thread_index].p_.p_[counter_index]);
                else
                    combined_counters.p_[counter_index] += tinfo[thread_index].p_.p_[counter_index];
            }
        return combined_counters;
    }

    void print_stats();
    uint8_t get_current_term() const;

    static void clear_stats() {
        for (int thread_index = 0; thread_index != MAX_THREADS; ++thread_index)
            tinfo[thread_index].p_.reset();
    }

    static void* epoch_advancer(void*);
    template <typename T>
    static void rcu_delete(T* object_ptr) {
        auto& thread_info = tinfo[TThread::id()];
        thread_info.rcu_set.add(thread_info.epoch, ObjectDestroyer<T>::destroy_and_free, object_ptr);
    }
    template <typename T>
    static void rcu_delete_array(T* array_ptr) {
        auto& thread_info = tinfo[TThread::id()];
        thread_info.rcu_set.add(thread_info.epoch, ObjectDestroyer<T>::destroy_and_free_array, array_ptr);
    }
    static void rcu_free(void* ptr) {
        auto& thread_info = tinfo[TThread::id()];
        thread_info.rcu_set.add(thread_info.epoch, ::free, ptr);
    }
    static void rcu_call(void (*function)(void*), void* argument) {
        auto& thread_info = tinfo[TThread::id()];
        thread_info.rcu_set.add(thread_info.epoch, function, argument);
    }
    static void rcu_quiesce() {
        tinfo[TThread::id()].epoch = 0;
    }

#if STO_PROFILE_COUNTERS
    template <unsigned CounterType> static void txp_account(txp_counter_type counter_value) {
        txp_helper<CounterType, txp_count>::account_array(tinfo[TThread::id()].p_.p_, counter_value);
    }
#else
    template <unsigned CounterType> static void txp_account(txp_counter_type) {
    }
#endif

#define TXP_INCREMENT(counter_type) Transaction::txp_account<(counter_type)>(1)
#define TXP_ACCOUNT(counter_type, counter_value) Transaction::txp_account<(counter_type)>((counter_value))


private:
    static constexpr unsigned tset_chunk = TransactionConfig::TRANSACTION_SET_CHUNK_SIZE;
    static constexpr unsigned tset_max_capacity = TransactionConfig::TRANSACTION_SET_MAX_CAPACITY;

    void initialize();

    Transaction()
        : threadid_(TThread::id()), is_test_(false) {
        initialize();
        start();
    }

    struct testing_type {};
    static testing_type testing;

    Transaction(int threadid, const testing_type&)
        : threadid_(threadid), is_test_(true) {
        initialize();
        start();
    }

    Transaction(bool)
        : threadid_(TThread::id()), is_test_(false) {
        initialize();
        state_ = s_aborted;
        // init once
        start_time = mako::getCurrentTimeMillis();
    }

    ~Transaction();

    // reset data so we can be reused for another transaction
    void start() {  // Initialize and start a new transaction
        threadinfo_t& current_thread_info = tinfo[TThread::id()];
        // Reset thread-local transaction state
        TThread::readset_shard_bits = 0;
        TThread::writeset_shard_bits = 0;
        TThread::the_debug_bit = 0;
        TThread::transget_without_throw = false;
        TThread::transget_without_stable = false;
        TThread::trans_nosend_abort = 0;
        TThread::increment_id += 1;
        
        // Update epoch information for RCU
        current_thread_info.epoch = global_epochs.global_epoch;
        current_thread_info.rcu_set.clean_until(global_epochs.active_epoch);
        if (current_thread_info.trans_start_callback)
            current_thread_info.trans_start_callback();
        hash_base_ += tset_size_ + 1;
        tset_size_ = 0;
        tset_next_ = tset0_;
#if TRANSACTION_HASHTABLE
        if (hash_base_ >= TransactionConfig::HASH_BASE_RESET_THRESHOLD) {
            memset(hashtable_, 0, sizeof(hashtable_));
            hash_base_ = 0;
        }
#endif
        any_writes_ = any_nonopaque_ = may_duplicate_items_ = false;
        first_write_ = 0;
        start_tid_ = commit_tid_ = 0;
        tid_unique_ = 0;
        current_term_ = 0;
        // Initialize single timestamp system
        maxTimestampReadSet = 0;
        buf_.clear();
#if STO_DEBUG_ABORTS
        abort_item_ = nullptr;
        abort_reason_ = nullptr;
        abort_version_ = 0;
#endif
        TXP_INCREMENT(txp_total_starts);
        state_ = s_in_progress;
    }

#if TRANSACTION_HASHTABLE
    static int hash(const TObject* object_ptr, void* key_ptr) {
        // Enhanced hash function with better distribution
        // Combine object pointer and key with improved mixing
        constexpr uintptr_t KEY_OFFSET = 0x4000000;
        constexpr uintptr_t KEY_THRESHOLD = 0x8000000;
        constexpr uint32_t GOLDEN_RATIO_HASH_MULTIPLIER = 2654435761U;
        constexpr int OBJECT_POINTER_SHIFT = 4;
        constexpr int HASH_MIX_SHIFT = 16;
        constexpr int HASH_MIX_MULTIPLIER = 9;
        
        auto key_hash = reinterpret_cast<uintptr_t>(key_ptr) + KEY_OFFSET;
        auto object_contribution = (reinterpret_cast<uintptr_t>(object_ptr) >> OBJECT_POINTER_SHIFT);
        
        // Apply conditional mixing based on key range
        key_hash += -uintptr_t(key_hash < KEY_THRESHOLD) & object_contribution;
        
        // Improved hash mixing using prime multiplier and bit shifting
        auto mixed_hash = key_hash + (key_hash >> HASH_MIX_SHIFT) * HASH_MIX_MULTIPLIER;
        
        return (mixed_hash * GOLDEN_RATIO_HASH_MULTIPLIER >> HASH_MIX_SHIFT) % hash_size;
    }
#endif

    void refresh_tset_chunk();

    TransItem* allocate_item(const TObject* object_ptr, void* transaction_key) {  // Allocate a new transaction item
        if (tset_size_ && tset_size_ % tset_chunk == 0)
            refresh_tset_chunk();
        ++tset_size_;
        new(reinterpret_cast<void*>(tset_next_)) TransItem(const_cast<TObject*>(object_ptr), transaction_key);
#if TRANSACTION_HASHTABLE
        unsigned hash_index = hash(object_ptr, transaction_key);
# if TRANSACTION_HASHTABLE > 1
        if (hashtable_[hash_index] > hash_base_)
            hash_index = (hash_index + hash_step) % hash_size;
# endif
        if (hashtable_[hash_index] <= hash_base_)
            hashtable_[hash_index] = hash_base_ + tset_size_;
#endif
        return tset_next_++;
    }

public:
    int threadid() const {
        return threadid_;
    }

    // adds item for a key that is known to be new (must NOT exist in the set)
    template <typename T>
    TransProxy new_item(const TObject* object_ptr, T key) {
        void* packed_key = Packer<T>::pack_unique(buf_, std::move(key));
        return TransProxy(*this, *allocate_item(object_ptr, packed_key));
    }

    // adds item without checking its presence in the array
    template <typename T>
    TransProxy fresh_item(const TObject* object_ptr, T key) {
        may_duplicate_items_ = tset_size_ > 0;
        void* packed_key = Packer<T>::pack_unique(buf_, std::move(key));
        return TransProxy(*this, *allocate_item(object_ptr, packed_key));
    }

    // tries to find an existing item with this key, otherwise adds it
    template <typename T>
    TransProxy item(const TObject* object_ptr, T key) {
        void* packed_key = Packer<T>::pack_unique(buf_, std::move(key));
        TransItem* transaction_item = find_item(const_cast<TObject*>(object_ptr), packed_key);
        if (!transaction_item)
            transaction_item = allocate_item(object_ptr, packed_key);
        return TransProxy(*this, *transaction_item);
    }

    // gets an item that is intended to be read only. this method essentially allows for duplicate items
    // in the set in some cases
    template <typename T>
    TransProxy read_item(const TObject* object_ptr, T key) {
        void* packed_key = Packer<T>::pack_unique(buf_, std::move(key));
        TransItem* transaction_item = nullptr;
        if (any_writes_)
            transaction_item = find_item(const_cast<TObject*>(object_ptr), packed_key);
        else
            may_duplicate_items_ = tset_size_ > 0;
        if (!transaction_item)
            transaction_item = allocate_item(object_ptr, packed_key);
        return TransProxy(*this, *transaction_item);
    }

    template <typename T>
    OptionalTransProxy check_item(const TObject* object_ptr, T key) const {
        void* packed_key = Packer<T>::pack_unique(buf_, std::move(key));
        TransItem* transaction_item = find_item(const_cast<TObject*>(object_ptr), packed_key);
        return OptionalTransProxy(const_cast<Transaction&>(*this), transaction_item);
    }

private:
    // tries to find an existing item with this key, returns NULL if not found
    TransItem* find_item(TObject* object_ptr, void* transaction_key) const {
#if TRANSACTION_HASHTABLE
        TXP_INCREMENT(txp_hash_find);
        unsigned hash_index = hash(object_ptr, transaction_key);
        for (int collision_steps = 0; collision_steps < TRANSACTION_HASHTABLE; ++collision_steps) {
            if (hashtable_[hash_index] <= hash_base_)
                return nullptr;
            unsigned transaction_index = hashtable_[hash_index] - hash_base_ - 1;
            const TransItem* current_item;
            if (likely(transaction_index < tset_initial_capacity))
                current_item = &tset0_[transaction_index];
            else
                current_item = &tset_[transaction_index / tset_chunk][transaction_index % tset_chunk];
            if (current_item->owner() == object_ptr && current_item->key_ == transaction_key)
                return const_cast<TransItem*>(current_item);
            if (!collision_steps) {
                TXP_INCREMENT(txp_hash_collision);
# if STO_DEBUG_HASH_COLLISIONS
                if (local_random() <= uint32_t(0xFFFFFFFF * TransactionConfig::DEBUG_HASH_COLLISION_FRACTION)) {
                    std::ostringstream debug_buffer;
                    TransItem fake_item(object_ptr, transaction_key);
                    debug_buffer << "$ STO hash collision: search " << fake_item << ", find " << *current_item << '\n';
                    std::cerr << debug_buffer.str();
                }
# endif
            } else
                TXP_INCREMENT(txp_hash_collision2);
            hash_index = (hash_index + hash_step) % hash_size;
        }
#endif
        const TransItem* current_item = nullptr;
        for (unsigned transaction_index = 0; transaction_index != tset_size_; ++transaction_index) {
            current_item = (transaction_index % tset_chunk ? current_item + 1 : tset_[transaction_index / tset_chunk]);
            TXP_INCREMENT(txp_total_searched);
            if (current_item->owner() == object_ptr && current_item->key_ == transaction_key)
                return const_cast<TransItem*>(current_item);
        }
        return nullptr;
    }

    bool preceding_duplicate_read(TransItem *it) const;

#if STO_DEBUG_ABORTS
    void mark_abort_because(TransItem* item, const char* reason, TVersion::type version = 0) const {
        abort_item_ = item;
        abort_reason_ = reason;
        if (version)
            abort_version_ = version;
    }
#else
    void mark_abort_because(TransItem*, const char*, TVersion::type = 0) const {
    }
#endif

    void abort_because(TransItem& item, const char* reason, TVersion::type version = 0) {
        mark_abort_because(&item, reason, version);
        abort();
    }

public:
    void silent_abort() {
        if (in_progress())
            stop(false, nullptr, 0);
    }

    void abort() {
        silent_abort();
        throw Abort();
    }

    bool try_commit(bool no_paxos= false);
    bool shard_try_lock_last_writeset();
    int shard_validate();
    void shard_install(uint32_t timestamp);
    void shard_serialize_util(uint32_t timestamp);
    void shard_unlock(bool committed);

    void commit() {
        if (!try_commit())
            throw Abort();
    }

    bool aborted() {
        return state_ == s_aborted;
    }

    bool in_progress() {
        return TThread::mode() == 1 || state_ < s_aborted;
    }

    // opacity checking
    // These function will eventually help us track the commit TID when we
    // have no opacity, or for GV7 opacity.
    bool try_lock(TransItem& item, TVersion& vers) {
        return try_lock(item, const_cast<TransactionTid::type&>(vers.value()));
    }
    bool try_lock(TransItem& item, TNonopaqueVersion& vers) {
        return try_lock(item, const_cast<TransactionTid::type&>(vers.value()));
    }
    bool try_lock(TransItem& item, TransactionTid::type& version) {
#if STO_SORT_WRITESET
        (void) item;
        TransactionTid::lock(version, threadid_);
        return true;
#else
        // This function will eventually help us track the commit TID when we
        // have no opacity, or for GV7 opacity.
        unsigned retry_count = 0;
        while (1) {
            if (TransactionTid::try_lock(version, threadid_, current_term_))
                return true;
            ++retry_count;
# if STO_SPIN_EXPBACKOFF
            if (item.has_read() || retry_count == TransactionConfig::SPIN_BOUND_WRITE_EXPBACKOFF) {
#  if STO_DEBUG_ABORTS
                abort_version_ = version;
#  endif
                return false;
            }
            if (retry_count > 3)
                for (unsigned backoff_cycles = 1 << std::min(15U, retry_count - 2); backoff_cycles; --backoff_cycles)
                    relax_fence();
# else
            if (item.has_read() || retry_count == (1 << TransactionConfig::SPIN_BOUND_WRITE_NORMAL)) {
#  if STO_DEBUG_ABORTS
                abort_version_ = version;
#  endif
                return false;
            }
# endif
            relax_fence();
        }
#endif
    }

    void check_opacity(TransItem& item, TransactionTid::type version_value) {
        assert(state_ <= s_committing_locked);
        if (!start_tid_)
            start_tid_ = _TID;
        if (!TransactionTid::try_check_opacity(start_tid_, version_value)
            && state_ < s_committing)
            hard_check_opacity(&item, version_value);
    }
    void check_opacity(TransItem& item, TVersion version) {
        check_opacity(item, version.value());
    }
    void check_opacity(TransItem&, TNonopaqueVersion) {
    }

    void check_opacity(TransactionTid::type version_value) {
        assert(state_ <= s_committing_locked);
        if (!start_tid_)
            start_tid_ = _TID;
        if (!TransactionTid::try_check_opacity(start_tid_, version_value)
            && state_ < s_committing)
            hard_check_opacity(nullptr, version_value);
    }

    void check_opacity() {
        check_opacity(_TID);
    }

    // committing
    tid_type commit_tid() const {
        assert(state_ == s_committing_locked || state_ == s_committing);
        if (!commit_tid_)
            commit_tid_ = fetch_and_add(&_TID, TransactionTid::increment_value);
        return commit_tid_;
    }

    void updateSingleTimestamp() const {
        assert(state_ == s_committing_locked || state_ == s_committing);
	    if(!tid_unique_)
            tid_unique_ = __sync_fetch_and_add(&sync_util::sync_logger::local_replica_id, 1);

        if (TThread::writeset_shard_bits>0/*||TThread::readset_shard_bits>0*/) {
            // Get single timestamp from remote shards
            uint32_t remote_timestamp = 0;
            TThread::sclient->remoteGetTimestamp(remote_timestamp);
            // Use the max timestamp
            if (remote_timestamp > tid_unique_) {
                tid_unique_ = remote_timestamp;
            }
        }
    }

    void set_version(TVersion& version, TVersion::type version_flags = 0) const {
        version.set_version(commit_tid() | version_flags);
    }
    void set_version_unlock(TVersion& version, TransItem& item, TVersion::type version_flags = 0) const {
        version.set_version_unlock(commit_tid() | version_flags);
        item.clear_needs_unlock();
    }
    void assign_version_unlock(TVersion& version, TransItem& item, TVersion::type version_flags = 0) const {
        version = commit_tid() | version_flags;
        item.clear_needs_unlock();
    }
    void set_version(TNonopaqueVersion& version, TNonopaqueVersion::type version_flags = 0) const {
        assert(state_ == s_committing_locked || state_ == s_committing);
        tid_type version_value = commit_tid_ ? commit_tid_ : TransactionTid::next_unflagged_nonopaque_version(version.value());
        version.set_version(version_value | version_flags);
    }
    void set_version_unlock(TNonopaqueVersion& version, TransItem& item, TNonopaqueVersion::type version_flags = 0) const {
        assert(state_ == s_committing_locked || state_ == s_committing);
        tid_type version_value = commit_tid_ ? commit_tid_ : TransactionTid::next_unflagged_nonopaque_version(version.value());
        version.set_version_unlock(version_value | version_flags);
        item.clear_needs_unlock();
    }
    void assign_version_unlock(TNonopaqueVersion& version, TransItem& item, TNonopaqueVersion::type version_flags = 0) const {
        tid_type version_value = commit_tid_ ? commit_tid_ : TransactionTid::next_unflagged_nonopaque_version(version.value());
        version = version_value | version_flags;
        item.clear_needs_unlock();
    }

    static const char* state_name(int state);
    void print() const;
    inline void serialize_util(unsigned writeset_count, bool is_remote_operation, int max_bytes_size, int batch_size, uint32_t timestamp) const;
    void print(std::ostream& w) const;

    class Abort {};

    uint32_t local_random() const {
        lrng_state_ = lrng_state_ * 1664525 + 1013904223;
        return lrng_state_;
    }
    void local_srandom(uint32_t state) {
        lrng_state_ = state;
    }

    static bool hasDeleteOp(const TransItem* item) {
      return item->flags() & TransItem::mdelete_bit;
    }

    bool hasInsertOp(const TransItem* item) const{
      return item->has_flag (TransItem::minsert_bit);
    }

    // Single timestamp system: tid_unique_ now serves as the single timestamp
    mutable uint32_t tid_unique_; // Single timestamp using fetch_and_add instruction
    mutable uint8_t current_term_;
    // The maximal timestamp received for this transaction in its readSet
    mutable uint32_t maxTimestampReadSet;
    mutable unordered_map<uint64_t, vector<uint64_t>> rollbacks_tracker; // <time in ms, shard clock of shard-0>

private:
    enum {
        s_in_progress = 0, s_opacity_check = 1, s_committing = 2,
        s_committing_locked = 3, s_aborted = 4, s_committed = 5
    };

    uint32_t start_time;
    int threadid_;
    uint16_t hash_base_;
    uint16_t first_write_;
    uint8_t state_;
    bool any_writes_;
    bool any_nonopaque_;
    bool may_duplicate_items_;
    bool is_test_;
    TransItem* tset_next_;
    unsigned tset_size_;
    mutable tid_type start_tid_;
    mutable tid_type commit_tid_;
    mutable TransactionBuffer buf_;
    mutable uint32_t lrng_state_;
#if STO_DEBUG_ABORTS
    mutable TransItem* abort_item_;
    mutable const char* abort_reason_;
    mutable TVersion::type abort_version_;
#endif
    TransItem* tset_[tset_max_capacity / tset_chunk];
#if TRANSACTION_HASHTABLE
    uint16_t hashtable_[hash_size];
#endif
    TransItem tset0_[tset_initial_capacity];

    void hard_check_opacity(TransItem* item, TransactionTid::type transaction_id);
    void stop(bool is_committed, unsigned* write_items, unsigned write_count);

    friend class TransProxy;
    friend class TransItem;
    friend class Sto;
    friend class TestTransaction;
    friend class TNonopaqueVersion;
};


class Sto {
public:
    static Transaction* transaction() {
        if (!TThread::txn)
            TThread::txn = new Transaction(false);
        return TThread::txn;
    }

    static void start_transaction() {
        Transaction* t = transaction();
        if (TThread::mode() == 0)
            always_assert(!t->in_progress());
        t->start();
    }

    static void update_threadid() {
        if (TThread::txn)
            TThread::txn->threadid_ = TThread::id();
    }

    static bool in_progress() {
        return TThread::mode() == 1 ||
                    (TThread::mode() == 0 && TThread::txn && TThread::txn->in_progress());
    }

    static void abort() {
        always_assert(in_progress());
        TThread::txn->abort();
    }

    static void abort_without_throw() {
        Sto::silent_abort();
        if (TThread::writeset_shard_bits>0||TThread::readset_shard_bits>0)
            TThread::sclient->remoteAbort(); 
    }

    static void silent_abort() {
        if (in_progress())
            TThread::txn->silent_abort();
    }

    template <typename T>
    static TransProxy item(const TObject* object_ptr, T key) {
        if (!in_progress()) {
            Panic("IT should never happen:%d", TThread::txn->state_);
        }
        always_assert(in_progress());
        return TThread::txn->item(object_ptr, key);
    }

    static void check_opacity(TransactionTid::type transaction_id) {
        always_assert(in_progress());
        TThread::txn->check_opacity(transaction_id);
    }

    static void check_opacity() {
        always_assert(in_progress());
        TThread::txn->check_opacity();
    }

    template <typename T>
    static OptionalTransProxy check_item(const TObject* object_ptr, T key) {
        if (!in_progress()){
            Panic("check_item is not in_progress, mode:%d, state_:%d", TThread::mode(), TThread::txn->state_);
        }
        return TThread::txn->check_item(object_ptr, key);
    }

    template <typename T>
    static TransProxy new_item(const TObject* object_ptr, T key) {
        always_assert(in_progress());
        return TThread::txn->new_item(object_ptr, key);
    }

    template <typename T>
    static TransProxy read_item(const TObject* object_ptr, T key) {
        always_assert(in_progress());
        return TThread::txn->read_item(object_ptr, key);
    }

    template <typename T>
    static TransProxy fresh_item(const TObject* object_ptr, T key) {
        always_assert(in_progress());
        return TThread::txn->fresh_item(object_ptr, key);
    }

    static void commit() {
        always_assert(in_progress());
        TThread::txn->commit();
    }

    static bool try_commit() {
        always_assert(in_progress());
        return TThread::txn->try_commit();
    }

    static bool try_commit_no_paxos() {
        always_assert(in_progress());
        return TThread::txn->try_commit(true);
    }

    static bool shard_try_lock_last_writeset() {
        return TThread::txn->shard_try_lock_last_writeset();
    }

    static void print_stats() {
        return TThread::txn->print_stats();
    }

    static int shard_validate() {
        return TThread::txn->shard_validate();
    }

    static void shard_install(uint32_t timestamp) {
        TThread::txn->shard_install(timestamp);
    }

    static void shard_serialize_util(uint32_t timestamp)  {
        TThread::txn->shard_serialize_util(timestamp);
    }

    static void shard_unlock(bool committed) {
        TThread::txn->shard_unlock(committed);
    }

    static TransactionTid::type commit_tid() {
        return TThread::txn->commit_tid();
    }

    static TransactionTid::type recent_tid() {
        return Transaction::global_epochs.recent_tid;
    }

    static TransactionTid::type initialized_tid() {
        // Future consideration: Add nonopaque_bit support for enhanced opacity control
        return TransactionTid::increment_value;
    }
};

class TestTransaction {
public:
    TestTransaction(int thread_id)
        : test_transaction_(thread_id, Transaction::testing), base_transaction_(TThread::txn) {
        use();
    }
    ~TestTransaction() {
        if (base_transaction_ && !base_transaction_->is_test_) {
            TThread::txn = base_transaction_;
            TThread::set_id(base_transaction_->threadid_);
        }
    }
    void use() {
        TThread::txn = &test_transaction_;
        TThread::set_id(test_transaction_.threadid_);
    }
    void print(std::ostream& output_stream) const {
        test_transaction_.print(output_stream);
    }
    bool try_commit() {
        use();
        return test_transaction_.try_commit();
    }
private:
    Transaction test_transaction_;
    Transaction* base_transaction_;
};

class TransactionGuard {
  public:
    TransactionGuard() {
        Sto::start_transaction();
    }
    ~TransactionGuard() {
        Sto::commit();
    }
    typedef void (TransactionGuard::* unspecified_bool_type)(std::ostream&) const;
    operator unspecified_bool_type() const {
        return &TransactionGuard::print;
    }
    void print(std::ostream& output_stream) const {
        TThread::txn->print(output_stream);
    }
};

class TransactionLoopGuard {
  public:
    TransactionLoopGuard() {
    }
    ~TransactionLoopGuard() {
        if (TThread::txn->in_progress())
            TThread::txn->silent_abort();
    }
    void start() {
        Sto::start_transaction();
    }
    bool try_commit() {
        return TThread::txn->try_commit();
    }
};


template <typename T>
inline TransProxy& TransProxy::add_read(T read_data) {
    assert(!has_stash());
    if (!has_read()) {
        item().__or_flags(TransItem::read_bit);
        item().rdata_ = Packer<T>::pack(t()->buf_, std::move(read_data));
        t()->any_nonopaque_ = true;
    }
    return *this;
}

inline TransProxy& TransProxy::add_extra(std::string extra_data) {
    item().extra = extra_data ;
    return *this;
}

// like add_read but checks opacity too.
// should be used by data structures that have non-TransactionTid
// versions and still need to respect opacity.
template <typename T>
inline TransProxy& TransProxy::add_read_opaque(T read_data) {
    assert(!has_stash());
    t()->check_opacity();
    if (!has_read()) {
        item().__or_flags(TransItem::read_bit);
        item().rdata_ = Packer<T>::pack(t()->buf_, std::move(read_data));
    }
    return *this;
}

inline TransProxy& TransProxy::observe(TVersion version, bool should_add_read) {
    assert(!has_stash());
    if (version.is_locked_elsewhere(t()->threadid_))
        t()->abort_because(item(), "locked", version.value());
    t()->check_opacity(item(), version.value());
    if (should_add_read && !has_read()) {
        item().__or_flags(TransItem::read_bit);
        item().rdata_ = Packer<TVersion>::pack(t()->buf_, std::move(version));
    }
    return *this;
}

inline TransProxy& TransProxy::observe(TNonopaqueVersion version, bool should_add_read) {
    assert(!has_stash());
    if (version.is_locked_elsewhere(t()->threadid_))
        t()->abort_because(item(), "locked", version.value());
    if (should_add_read && !has_read()) {
        item().__or_flags(TransItem::read_bit);
        item().rdata_ = Packer<TNonopaqueVersion>::pack(t()->buf_, std::move(version));
        t()->any_nonopaque_ = true;
    }
    return *this;
}

inline TransProxy& TransProxy::observe(TCommutativeVersion version, bool should_add_read) {
    assert(!has_stash());
    if (version.is_locked())
        t()->abort_because(item(), "locked", version.value());
    t()->check_opacity(item(), version.value());
    if (should_add_read && !has_read()) {
        item().__or_flags(TransItem::read_bit);
        item().rdata_ = Packer<TCommutativeVersion>::pack(t()->buf_, std::move(version));
    }
    return *this;
}

inline TransProxy& TransProxy::observe(TVersion version) {
    return observe(version, true);
}

inline TransProxy& TransProxy::observe(TNonopaqueVersion version) {
    return observe(version, true);
}

inline TransProxy& TransProxy::observe(TCommutativeVersion version) {
    return observe(version, true);
}

inline TransProxy& TransProxy::observe_opacity(TVersion version) {
    return observe(version, false);
}

inline TransProxy& TransProxy::observe_opacity(TNonopaqueVersion version) {
    return observe(version, false);
}

inline TransProxy& TransProxy::observe_opacity(TCommutativeVersion version) {
    return observe(version, false);
}

template <typename T>
inline TransProxy& TransProxy::update_read(T old_read_data, T new_read_data) {
    if (has_read() && this->read_value<T>() == old_read_data)
        item().rdata_ = Packer<T>::repack(t()->buf_, item().rdata_, new_read_data);
    return *this;
}


inline TransProxy& TransProxy::set_predicate() {
    assert(!has_read());
    item().__or_flags(TransItem::predicate_bit);
    return *this;
}

template <typename T>
inline TransProxy& TransProxy::set_predicate(T predicate_data) {
    assert(!has_read());
    item().__or_flags(TransItem::predicate_bit);
    item().rdata_ = Packer<T>::pack(t()->buf_, std::move(predicate_data));
    return *this;
}

template <typename T>
inline T& TransProxy::predicate_value(T default_predicate_data) {
    assert(!has_read());
    if (!has_predicate())
        set_predicate(default_predicate_data);
    return this->template predicate_value<T>();
}

inline TransProxy& TransProxy::add_write() {
    if (!has_write()) {
        item().__or_flags(TransItem::write_bit);
        t()->any_writes_ = true;
    }
    return *this;
}

template <typename T>
inline TransProxy& TransProxy::add_write(const T& write_data) {
    return add_write<T, const T&>(write_data);
}

template <typename T>
inline TransProxy& TransProxy::add_write(T&& write_data) {
    typedef typename std::decay<T>::type ValueType;
    return add_write<ValueType, ValueType&&>(std::move(write_data));
}

template <typename T, typename... Args>
inline TransProxy& TransProxy::add_write(Args&&... constructor_args) {
    if (!has_write()) {
        item().__or_flags(TransItem::write_bit);
        item().wdata_ = Packer<T>::pack(t()->buf_, std::forward<Args>(constructor_args)...);
        t()->any_writes_ = true;
    } else
        // Current limitation: Assumes consistent writer data types per item
        // This holds true for current usage patterns but may need enhancement
        // for heterogeneous data types with automatic destructor management
        item().wdata_ = Packer<T>::repack(t()->buf_, item().wdata_, std::forward<Args>(constructor_args)...);
    return *this;
}

template <typename T>
inline TransProxy& TransProxy::set_stash(T stash_data) {
    assert(!has_read());
    if (!has_stash()) {
        item().__or_flags(TransItem::stash_bit);
        item().rdata_ = Packer<T>::pack(t()->buf_, std::move(stash_data));
    } else
        item().rdata_ = Packer<T>::repack(t()->buf_, item().rdata_, std::move(stash_data));
    return *this;
}

template <typename Exception>
inline void TNonopaqueVersion::opaque_throw(const Exception&) {
    Sto::abort();
}

inline auto TVersion::snapshot(TransProxy& transaction_item) -> type {
    type version_value = value();
    transaction_item.observe_opacity(TVersion(version_value));
    return version_value;
}

inline auto TVersion::snapshot(const TransItem& transaction_item, const Transaction& transaction) -> type {
    type version_value = value();
    const_cast<Transaction&>(transaction).check_opacity(const_cast<TransItem&>(transaction_item), version_value);
    return version_value;
}

inline auto TNonopaqueVersion::snapshot(TransProxy& transaction_item) -> type {
    transaction_item.transaction().any_nonopaque_ = true;
    return value();
}

inline auto TNonopaqueVersion::snapshot(const TransItem&, const Transaction& transaction) -> type {
    const_cast<Transaction&>(transaction).any_nonopaque_ = true;
    return value();
}

inline bool TVersion::is_locked_here(const Transaction& transaction) const {
    return is_locked_here(transaction.threadid());
}

inline bool TNonopaqueVersion::is_locked_here(const Transaction& transaction) const {
    return is_locked_here(transaction.threadid());
}

inline bool TVersion::is_locked_elsewhere(const Transaction& transaction) const {
    return is_locked_elsewhere(transaction.threadid());
}

inline bool TNonopaqueVersion::is_locked_elsewhere(const Transaction& transaction) const {
    return is_locked_elsewhere(transaction.threadid());
}

std::ostream& operator<<(std::ostream& output_stream, const Transaction& transaction);
std::ostream& operator<<(std::ostream& output_stream, const TestTransaction& test_transaction);
std::ostream& operator<<(std::ostream& output_stream, const TransactionGuard& transaction_guard);
