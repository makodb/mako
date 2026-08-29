// Relative overhead benchmark for the local MassTrans transaction boundary.
//
// This binary runs one surface per process (direct MassTrans, the raw public C
// facade, or the private trusted Rust fast ABI).
// crates/mako-local/tests/overhead.rs runs the safe Rust surface in another
// process, validates this machine-readable protocol, and computes same-host
// relative wrapper tax. Absolute throughput is deliberately not a correctness
// or release gate.

#include "lib/common.h"
#include "sto/MassTrans.hh"
#include "sto/StringWrapper.hh"
#include "sto/thread_registration.hh"
#include "storage/mako_local_abi.h"
#include "storage/mako_local_rust_fast_abi.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef STO_OPACITY
#define STO_OPACITY 0
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::string;

constexpr size_t kWorkers = 4;
constexpr size_t kRepetitions = 7;
constexpr size_t kWarmupKeyTouches = 2048;
constexpr size_t kSampleKeyTouches = 8192;
constexpr size_t kMinimumWarmupTransactions = 64;
constexpr size_t kMinimumSampleTransactions = 256;
constexpr size_t kLowContentionWindows = 64;
constexpr size_t kRetryLimitMultiplier = 1000;
constexpr uint64_t kForcedCollisionAttempts = 512;
constexpr uint64_t kFirstTableId = 81'000;
constexpr size_t kConfigurationCount = 3 * 4 * 2;
// Every surface runs in its own process. The main worker consumes one STO ID;
// each of the 24 configurations creates exactly four worker threads. This
// bounded 97-ID matrix is an overhead experiment, not Item 4's fixed-worker
// reuse/progress gate.
constexpr size_t kLifetimeWorkerIds = 1 + kConfigurationCount * kWorkers;
static_assert(kLifetimeWorkerIds == 97);
static_assert(kLifetimeWorkerIds <= MAX_THREADS);

#if STO_OPACITY
using DirectTable =
    MassTrans<std::string, versioned_str_struct, true /* opacity */>;
#else
using DirectTable =
    MassTrans<std::string, versioned_str_struct, false /* opacity */>;
#endif

enum class Mode { direct, abi, fast };
enum class Workload { read, write, rmw };
enum class Contention { low, high };

struct Configuration {
  Workload workload;
  size_t transaction_size;
  Contention contention;
  size_t ordinal;
};

struct BatchStats {
  uint64_t commits = 0;
  uint64_t conflicts = 0;
  uint64_t logical_operations = 0;
};

struct Sample {
  uint64_t duration_ns = 0;
  BatchStats stats;
};

struct TableHandle {
  DirectTable *direct = nullptr;
  mako_local_table *abi = nullptr;
};

std::string_view mode_name(Mode mode) {
  switch (mode) {
  case Mode::direct:
    return "direct";
  case Mode::abi:
    return "abi";
  case Mode::fast:
    return "fast";
  }
  std::abort();
}

std::string_view workload_name(Workload workload) {
  switch (workload) {
  case Workload::read:
    return "read";
  case Workload::write:
    return "write";
  case Workload::rmw:
    return "rmw";
  }
  std::abort();
}

std::string_view contention_name(Contention contention) {
  return contention == Contention::low ? "low" : "high";
}

std::string status_description(int status) {
  const char *message = mako_local_status_string(status);
  return std::to_string(status) + " (" +
         (message == nullptr ? std::string("null status message")
                             : std::string(message)) +
         ")";
}

void require_ok(int status, std::string_view operation) {
  if (status != MAKO_LOCAL_OK) {
    throw std::runtime_error(std::string(operation) + " failed with " +
                             status_description(status));
  }
}

void require_features() {
  constexpr uint64_t required =
      MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS | MAKO_LOCAL_FEATURE_READ_MY_WRITES;
  const uint64_t actual = mako_local_feature_bits();
  if ((actual & required) != required) {
    throw std::runtime_error(
        "overhead benchmark requires point transactions and read-my-writes");
  }
}

void initialize_direct_worker() {
  if (!mako::silo::claim_thread_runtime(
          mako::silo::thread_runtime::native_mako)) {
    throw std::runtime_error("direct worker runtime was already claimed");
  }
  const int id = mako::silo::try_allocate_thread_id();
  if (id < 0)
    throw std::runtime_error("direct worker exhausted STO thread IDs");

  TThread::set_id(id);
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

  if (!mako::silo::ensure_epoch_runtime())
    throw std::runtime_error("could not initialize the MassTrans epoch");
  DirectTable::thread_init();
}

Bytes key_bytes(uint64_t key) {
  Bytes bytes(sizeof(key), '\0');
  for (size_t index = 0; index != sizeof(key); ++index) {
    bytes[index] = static_cast<char>((key >> ((sizeof(key) - index - 1) * 8)) &
                                     UINT64_C(0xff));
  }
  return bytes;
}

Bytes value_bytes(uint64_t value) {
  Bytes bytes(sizeof(value), '\0');
  for (size_t index = 0; index != sizeof(value); ++index) {
    bytes[index] = static_cast<char>((value >> (index * 8)) & UINT64_C(0xff));
  }
  return bytes;
}

uint64_t decode_value(std::string_view bytes) {
  if (bytes.size() != sizeof(uint64_t))
    throw std::runtime_error("benchmark read a malformed counter value");
  uint64_t value = 0;
  for (size_t index = 0; index != sizeof(value); ++index) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[index]))
             << (index * 8);
  }
  return value;
}

lcdf::Str direct_key(const Bytes &bytes) {
  return lcdf::Str(bytes.data(), static_cast<int>(bytes.size()));
}

Bytes decode_direct_value(const std::string &encoded) {
  if (encoded.size() < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE))
    throw std::runtime_error("direct MassTrans returned a malformed value");
  return encoded.substr(0, encoded.size() - mako::EXTRA_BITS_FOR_VALUE);
}

class DirectWorker {
public:
  explicit DirectWorker(DirectTable *table) : table_(table) {
    if (table_ == nullptr)
      throw std::runtime_error("null direct benchmark table");
  }

  ~DirectWorker() { abort_if_active(); }

  void begin() {
    if (active_ || Sto::in_progress())
      throw std::runtime_error("direct benchmark transaction already active");
    Sto::start_transaction();
    active_ = true;
  }

  bool get(const Bytes &key, uint64_t &value) {
    require_active();
    try {
      std::string encoded;
      const bool found = table_->transGet(direct_key(key), encoded);
      if (TThread::transget_without_throw) {
        TThread::transget_without_throw = false;
        abort_if_active();
        return false;
      }
      if (!found)
        throw std::runtime_error("direct benchmark key unexpectedly missing");
      value = decode_value(decode_direct_value(encoded));
      return true;
    } catch (const Transaction::Abort &) {
      abort_if_active();
      return false;
    } catch (...) {
      abort_if_active();
      throw;
    }
  }

  bool put(const Bytes &key, uint64_t value) {
    require_active();
    try {
      const Bytes bytes = value_bytes(value);
      const size_t encoded_index = encoded_values_used_++;
      std::string &encoded = encoded_values_[encoded_index];
      const size_t encoded_size =
          bytes.size() + static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);
      if (encoded.size() != encoded_size)
        encoded.resize(encoded_size, '\0');
      if (!bytes.empty())
        std::memcpy(encoded.data(), bytes.data(), bytes.size());
      std::array<unsigned char,
                 static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE)> metadata{};
      char *null_data = nullptr;
      std::memcpy(
          metadata.data() + sizeof(uint32_t) + offsetof(mako::Node, data),
          &null_data, sizeof(null_data));
      std::memcpy(encoded.data() + bytes.size(), metadata.data(),
                  metadata.size());
      if (encoded.capacity() > kMaximumRetainedValueCapacity)
        encoded_value_release_required_ = true;
      (void)table_->transPut(direct_key(key), StringWrapper(encoded));
      return true;
    } catch (const Transaction::Abort &) {
      abort_if_active();
      return false;
    } catch (...) {
      abort_if_active();
      throw;
    }
  }

  bool commit() {
    require_active();
    try {
      const bool committed = Sto::try_commit_no_paxos();
      active_ = false;
      finish_encoded_values();
      return committed;
    } catch (const Transaction::Abort &) {
      abort_if_active();
      return false;
    } catch (...) {
      abort_if_active();
      throw;
    }
  }

private:
  void require_active() const {
    if (!active_)
      throw std::runtime_error("direct benchmark transaction is not active");
  }

  void abort_if_active() noexcept {
    if (!active_)
      return;
    try {
      if (Sto::in_progress())
        Sto::silent_abort();
    } catch (...) {
    }
    active_ = false;
    finish_encoded_values();
  }

  void finish_encoded_values() noexcept {
    if (encoded_value_release_required_) {
      for (size_t index = 0; index != encoded_values_used_; ++index) {
        std::string &encoded = encoded_values_[index];
        if (encoded.capacity() > kMaximumRetainedValueCapacity)
          std::string{}.swap(encoded);
      }
      encoded_value_release_required_ = false;
    }
    encoded_values_used_ = 0;
  }

  DirectTable *table_;
  bool active_ = false;
  // Match the raw ABI's bounded stable-value pool so this benchmark measures
  // the facade contract rather than deque allocation/free policy.
  static constexpr size_t kEncodedValueSlots =
      MAKO_LOCAL_TXN_ITEM_BUDGET / 4;
  static constexpr size_t kMaximumRetainedValueCapacity = 512;
  static_assert(kEncodedValueSlots == 128);
  std::array<std::string, kEncodedValueSlots> encoded_values_;
  size_t encoded_values_used_ = 0;
  bool encoded_value_release_required_ = false;
};

const uint8_t *abi_bytes(const Bytes &bytes) {
  return reinterpret_cast<const uint8_t *>(bytes.data());
}

class AbiWorker {
public:
  AbiWorker(mako_local_db *db, mako_local_table *table)
      : db_(db), table_(table) {
    if (db_ == nullptr || table_ == nullptr)
      throw std::runtime_error("null ABI benchmark handle");
  }

  ~AbiWorker() {
    if (txn_ != nullptr)
      (void)mako_local_txn_destroy(txn_);
  }

  void begin() {
    if (txn_ != nullptr)
      throw std::runtime_error("ABI benchmark transaction already active");
    require_ok(mako_local_txn_begin(db_, &txn_), "benchmark transaction begin");
    if (txn_ == nullptr)
      throw std::runtime_error("ABI benchmark begin returned null");
  }

  bool get(const Bytes &key, uint64_t &value) {
    require_active();
    uint8_t *bytes = nullptr;
    size_t length = 0;
    uint8_t found = 0;
    const int status = mako_local_txn_get(txn_, table_, abi_bytes(key),
                                          key.size(), &bytes, &length, &found);
    std::unique_ptr<uint8_t, decltype(&mako_local_bytes_free)> owned(
        bytes, &mako_local_bytes_free);
    if (status == MAKO_LOCAL_CONFLICT) {
      destroy_finished();
      return false;
    }
    if (status != MAKO_LOCAL_OK) {
      fail_status(status, "benchmark get");
    }
    if (found != 1 || bytes == nullptr || length != sizeof(uint64_t))
      fail("ABI benchmark get returned malformed output");
    value = decode_value(
        std::string_view(reinterpret_cast<const char *>(bytes), length));
    return true;
  }

  bool put(const Bytes &key, uint64_t value) {
    require_active();
    const Bytes bytes = value_bytes(value);
    uint8_t created = 0;
    const int status =
        mako_local_txn_put(txn_, table_, abi_bytes(key), key.size(),
                           abi_bytes(bytes), bytes.size(), &created);
    if (status == MAKO_LOCAL_CONFLICT) {
      destroy_finished();
      return false;
    }
    if (status != MAKO_LOCAL_OK)
      fail_status(status, "benchmark put");
    if (created > 1)
      fail("ABI benchmark put returned malformed output");
    return true;
  }

  bool commit() {
    require_active();
    mako_local_txn *txn = txn_;
    const int commit_status = mako_local_txn_commit(txn);
    const int destroy_status = mako_local_txn_destroy(txn);
    txn_ = nullptr;
    require_ok(destroy_status, "benchmark transaction destroy");
    if (commit_status == MAKO_LOCAL_CONFLICT)
      return false;
    require_ok(commit_status, "benchmark transaction commit");
    return true;
  }

private:
  void require_active() const {
    if (txn_ == nullptr)
      throw std::runtime_error("ABI benchmark transaction is not active");
  }

  void destroy_finished() {
    mako_local_txn *txn = txn_;
    txn_ = nullptr;
    require_ok(mako_local_txn_destroy(txn),
               "conflicted benchmark transaction destroy");
  }

  [[noreturn]] void fail(const std::string &message) {
    const int cleanup =
        txn_ == nullptr ? MAKO_LOCAL_OK : mako_local_txn_destroy(txn_);
    txn_ = nullptr;
    if (cleanup != MAKO_LOCAL_OK) {
      throw std::runtime_error(message + "; destroy failed with " +
                               status_description(cleanup));
    }
    throw std::runtime_error(message);
  }

  [[noreturn]] void fail_status(int status, std::string_view operation) {
    fail(std::string(operation) + " failed with " + status_description(status));
  }

  mako_local_db *db_;
  mako_local_table *table_;
  mako_local_txn *txn_ = nullptr;
};

class FastAbiWorker {
public:
  FastAbiWorker(mako_local_db *db, mako_local_table *table)
      : db_(db), table_(table) {
    if (db_ == nullptr || table_ == nullptr)
      throw std::runtime_error("null fast-ABI benchmark handle");
  }

  ~FastAbiWorker() {
    if (txn_ != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn_);
  }

  void begin() {
    if (txn_ != nullptr)
      throw std::runtime_error("fast-ABI benchmark transaction already active");
    require_ok(mako_rust_fast_txn_begin(db_, table_, &txn_),
               "fast-ABI benchmark transaction begin");
    if (txn_ == nullptr)
      throw std::runtime_error("fast-ABI benchmark begin returned null");
  }

  bool get(const Bytes &key, uint64_t &value) {
    require_active();
    uint8_t *bytes = nullptr;
    size_t length = 0;
    uint8_t found = 0;
    // The trusted begin returns the ordinary transaction facade, so reads can
    // intentionally retain the public ABI until a separate fast-read design
    // has its own measured justification.
    const int status = mako_local_txn_get(txn_, table_, abi_bytes(key),
                                          key.size(), &bytes, &length, &found);
    std::unique_ptr<uint8_t, decltype(&mako_local_bytes_free)> owned(
        bytes, &mako_local_bytes_free);
    if (status == MAKO_LOCAL_CONFLICT) {
      abort_and_destroy();
      return false;
    }
    if (status != MAKO_LOCAL_OK)
      fail_status(status, "fast-ABI benchmark get");
    if (found != 1 || bytes == nullptr || length != sizeof(uint64_t))
      fail("fast-ABI benchmark get returned malformed output");
    value = decode_value(
        std::string_view(reinterpret_cast<const char *>(bytes), length));
    return true;
  }

  bool put(const Bytes &key, uint64_t value) {
    require_active();
    const Bytes bytes = value_bytes(value);
    const uint64_t packed = mako_rust_fast_txn_put(
        txn_, abi_bytes(key), static_cast<uint32_t>(key.size()),
        abi_bytes(bytes), static_cast<uint32_t>(bytes.size()));
    const int status = MAKO_RUST_FAST_PUT_STATUS(packed);
    const uint8_t created = MAKO_RUST_FAST_PUT_CREATED(packed);
    if ((packed >> 33) != 0 || (status != MAKO_LOCAL_OK && created != 0))
      fail("fast-ABI benchmark put returned malformed packed output");
    if (status == MAKO_LOCAL_CONFLICT) {
      abort_and_destroy();
      return false;
    }
    if (status != MAKO_LOCAL_OK)
      fail_status(status, "fast-ABI benchmark put");
    return true;
  }

  bool commit() {
    require_active();
    mako_local_txn *txn = txn_;
    txn_ = nullptr;
    const uint64_t packed = mako_rust_fast_txn_commit_and_destroy(txn);
    const int commit_status = MAKO_RUST_FAST_TERMINAL_STATUS(packed);
    const int cleanup_status = MAKO_RUST_FAST_CLEANUP_STATUS(packed);
    require_ok(cleanup_status, "fast-ABI benchmark transaction cleanup");
    if (commit_status == MAKO_LOCAL_CONFLICT)
      return false;
    require_ok(commit_status, "fast-ABI benchmark transaction commit");
    return true;
  }

private:
  void require_active() const {
    if (txn_ == nullptr)
      throw std::runtime_error("fast-ABI benchmark transaction is not active");
  }

  void abort_and_destroy() {
    mako_local_txn *txn = txn_;
    txn_ = nullptr;
    if (txn == nullptr)
      return;
    const uint64_t packed = mako_rust_fast_txn_abort_and_destroy(txn);
    const int cleanup_status = MAKO_RUST_FAST_CLEANUP_STATUS(packed);
    require_ok(cleanup_status, "fast-ABI benchmark transaction cleanup");
  }

  [[noreturn]] void fail(const std::string &message) {
    mako_local_txn *txn = txn_;
    txn_ = nullptr;
    if (txn != nullptr) {
      const uint64_t packed = mako_rust_fast_txn_abort_and_destroy(txn);
      const int cleanup_status = MAKO_RUST_FAST_CLEANUP_STATUS(packed);
      if (cleanup_status != MAKO_LOCAL_OK) {
        throw std::runtime_error(message + "; cleanup failed with " +
                                 status_description(cleanup_status));
      }
    }
    throw std::runtime_error(message);
  }

  [[noreturn]] void fail_status(int status, std::string_view operation) {
    fail(std::string(operation) + " failed with " + status_description(status));
  }

  mako_local_db *db_;
  mako_local_table *table_;
  mako_local_txn *txn_ = nullptr;
};

size_t transactions_for(size_t transaction_size, bool warmup) {
  const size_t key_touches = warmup ? kWarmupKeyTouches : kSampleKeyTouches;
  const size_t minimum =
      warmup ? kMinimumWarmupTransactions : kMinimumSampleTransactions;
  return std::max(minimum, key_touches / transaction_size);
}

size_t key_count(const Configuration &configuration) {
  if (configuration.contention == Contention::high)
    return configuration.transaction_size;
  return kWorkers * configuration.transaction_size * kLowContentionWindows;
}

uint64_t selected_key(const Configuration &configuration, size_t worker,
                      uint64_t successful_transaction, size_t item) {
  if (configuration.contention == Contention::high)
    return item;
  const uint64_t window =
      configuration.transaction_size * kLowContentionWindows;
  return worker * window +
         (successful_transaction * configuration.transaction_size + item) %
             window;
}

template <typename Worker>
BatchStats run_batch(Worker &worker, const Configuration &configuration,
                     size_t worker_index, uint64_t target_commits,
                     std::barrier<> *collision_barrier) {
  BatchStats stats;
  const uint64_t retry_limit = std::max<uint64_t>(
      target_commits + 1, target_commits * kRetryLimitMultiplier);
  while (stats.commits != target_commits) {
    const uint64_t attempts = stats.commits + stats.conflicts;
    if (attempts >= retry_limit) {
      throw std::runtime_error("benchmark exceeded its conflict retry budget");
    }
    // Keep a bounded prefix of hot write/RMW attempts concurrent even on a
    // single-core scheduler. High-contention timings are diagnostic only.
    const bool synchronize_collision =
        collision_barrier != nullptr &&
        attempts < std::min(target_commits, kForcedCollisionAttempts);
    if (synchronize_collision)
      collision_barrier->arrive_and_wait();
    worker.begin();
    bool live = true;
    for (size_t item = 0; item != configuration.transaction_size; ++item) {
      const Bytes key = key_bytes(
          selected_key(configuration, worker_index, stats.commits, item));
      uint64_t value = 0;
      if (configuration.workload != Workload::write) {
        live = worker.get(key, value);
        if (!live)
          break;
        ++stats.logical_operations;
      }
      if (configuration.workload != Workload::read) {
        const uint64_t next =
            configuration.workload == Workload::rmw ? value + 1 : UINT64_C(1);
        live = worker.put(key, next);
        if (!live)
          break;
        ++stats.logical_operations;
      }
    }
    if (synchronize_collision)
      collision_barrier->arrive_and_wait();
    if (live)
      live = worker.commit();
    if (live) {
      ++stats.commits;
    } else {
      ++stats.conflicts;
      std::this_thread::yield();
    }
  }
  return stats;
}

template <typename Worker> void seed_table(Worker &worker, size_t count) {
  constexpr size_t batch_size = 64;
  for (size_t base = 0; base < count; base += batch_size) {
    const size_t end = std::min(count, base + batch_size);
    worker.begin();
    for (size_t index = base; index < end; ++index) {
      if (!worker.put(key_bytes(index), 0))
        throw std::runtime_error("unexpected conflict while seeding benchmark");
    }
    if (!worker.commit())
      throw std::runtime_error("unexpected commit conflict while seeding");
  }
}

uint64_t expected_configured_value(const Configuration &configuration,
                                   size_t key_index) {
  if (configuration.workload == Workload::read)
    return 0;
  if (configuration.workload == Workload::write)
    return 1;

  const size_t warmup_transactions =
      transactions_for(configuration.transaction_size, true);
  const size_t sample_transactions =
      transactions_for(configuration.transaction_size, false);
  if (configuration.contention == Contention::high) {
    return kWorkers *
           (warmup_transactions + kRepetitions * sample_transactions);
  }

  const size_t window = configuration.transaction_size * kLowContentionWindows;
  const size_t local_key = key_index % window;
  const auto phase_hits = [&](size_t transactions) {
    const size_t touches = transactions * configuration.transaction_size;
    return touches / window +
           (local_key < touches % window ? size_t{1} : size_t{0});
  };
  return phase_hits(warmup_transactions) +
         kRepetitions * phase_hits(sample_transactions);
}

template <typename Worker>
uint64_t validate_table(Worker &worker, const Configuration &configuration) {
  constexpr size_t batch_size = 128;
  const size_t count = key_count(configuration);
  uint64_t sum = 0;
  for (size_t base = 0; base < count; base += batch_size) {
    const size_t end = std::min(count, base + batch_size);
    worker.begin();
    for (size_t index = base; index < end; ++index) {
      uint64_t value = 0;
      if (!worker.get(key_bytes(index), value))
        throw std::runtime_error("unexpected conflict while validating table");
      const uint64_t expected = expected_configured_value(configuration, index);
      if (value != expected) {
        throw std::runtime_error(
            "benchmark configured-key value mismatch at key " +
            std::to_string(index) + ": expected " + std::to_string(expected) +
            ", found " + std::to_string(value));
      }
      sum += value;
    }
    if (!worker.commit())
      throw std::runtime_error("unexpected validation commit conflict");
  }

  return sum;
}

template <typename MakeWorker>
std::vector<Sample> run_concurrent(const Configuration &configuration,
                                   MakeWorker make_worker) {
  const size_t phases = 1 + kRepetitions;
  std::barrier phase_barrier(static_cast<std::ptrdiff_t>(kWorkers + 1));
  std::barrier collision_barrier(static_cast<std::ptrdiff_t>(kWorkers));
  std::latch ready(static_cast<std::ptrdiff_t>(kWorkers));
  std::vector<std::vector<BatchStats>> worker_stats(
      kWorkers, std::vector<BatchStats>(phases));
  std::vector<std::string> worker_errors(kWorkers);
  std::vector<std::thread> threads;
  threads.reserve(kWorkers);

  for (size_t worker_index = 0; worker_index != kWorkers; ++worker_index) {
    threads.emplace_back([&, worker_index] {
      decltype(make_worker()) worker;
      try {
        worker = make_worker();
      } catch (const std::exception &error) {
        worker_errors[worker_index] = error.what();
      } catch (...) {
        worker_errors[worker_index] = "unknown worker initialization failure";
      }
      ready.count_down();

      for (size_t phase = 0; phase != phases; ++phase) {
        phase_barrier.arrive_and_wait();
        if (worker_errors[worker_index].empty()) {
          try {
            worker_stats[worker_index][phase] = run_batch(
                *worker, configuration, worker_index,
                transactions_for(configuration.transaction_size, phase == 0),
                configuration.contention == Contention::high &&
                        configuration.workload != Workload::read
                    ? &collision_barrier
                    : nullptr);
          } catch (const std::exception &error) {
            worker_errors[worker_index] = error.what();
          } catch (...) {
            worker_errors[worker_index] = "unknown benchmark worker failure";
          }
        }
        phase_barrier.arrive_and_wait();
      }
    });
  }

  ready.wait();
  std::vector<uint64_t> durations(phases);
  for (size_t phase = 0; phase != phases; ++phase) {
    const auto start = Clock::now();
    phase_barrier.arrive_and_wait();
    phase_barrier.arrive_and_wait();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start);
    durations[phase] = std::max<int64_t>(1, elapsed.count());
  }
  for (std::thread &thread : threads)
    thread.join();

  for (size_t worker = 0; worker != kWorkers; ++worker) {
    if (!worker_errors[worker].empty()) {
      throw std::runtime_error("benchmark worker " + std::to_string(worker) +
                               " failed: " + worker_errors[worker]);
    }
  }

  std::vector<Sample> samples;
  samples.reserve(kRepetitions);
  for (size_t phase = 1; phase != phases; ++phase) {
    Sample sample;
    sample.duration_ns = durations[phase];
    for (size_t worker = 0; worker != kWorkers; ++worker) {
      sample.stats.commits += worker_stats[worker][phase].commits;
      sample.stats.conflicts += worker_stats[worker][phase].conflicts;
      sample.stats.logical_operations +=
          worker_stats[worker][phase].logical_operations;
    }
    samples.push_back(sample);
  }
  return samples;
}

std::vector<Configuration> configurations() {
  std::vector<Configuration> result;
  size_t ordinal = 0;
  for (Workload workload : {Workload::read, Workload::write, Workload::rmw}) {
    for (size_t size : {size_t{1}, size_t{4}, size_t{16}, size_t{64}}) {
      for (Contention contention : {Contention::low, Contention::high}) {
        result.push_back(Configuration{workload, size, contention, ordinal++});
      }
    }
  }
  return result;
}

class BenchmarkDatabase {
public:
  explicit BenchmarkDatabase(Mode mode) : mode_(mode) {
    require_features();
    if (mode_ == Mode::direct) {
      initialize_direct_worker();
    } else {
      require_ok(mako_local_thread_attach(), "benchmark main-thread attach");
      require_ok(mako_local_db_open(&db_), "benchmark database open");
      if (db_ == nullptr)
        throw std::runtime_error("benchmark database open returned null");
    }
  }

  ~BenchmarkDatabase() {
    if (db_ != nullptr)
      (void)mako_local_db_close(db_);
  }

  TableHandle create_table(const Configuration &configuration) {
    const std::string name =
        "mako-local-overhead-" + std::to_string(configuration.ordinal);
    TableHandle handle;
    if (mode_ == Mode::direct) {
      handle.direct = new DirectTable();
      handle.direct->set_table_id(kFirstTableId + configuration.ordinal);
      handle.direct->set_is_remote(false);
      handle.direct->set_table_name(name);
      DirectWorker worker(handle.direct);
      seed_table(worker, key_count(configuration));
    } else {
      require_ok(mako_local_table_open(
                     db_, reinterpret_cast<const uint8_t *>(name.data()),
                     name.size(), kFirstTableId + configuration.ordinal,
                     &handle.abi),
                 "benchmark table open");
      if (handle.abi == nullptr)
        throw std::runtime_error("benchmark table open returned null");
      if (mode_ == Mode::fast) {
        FastAbiWorker worker(db_, handle.abi);
        seed_table(worker, key_count(configuration));
      } else {
        AbiWorker worker(db_, handle.abi);
        seed_table(worker, key_count(configuration));
      }
    }
    return handle;
  }

  std::vector<Sample> run(const Configuration &configuration,
                          TableHandle handle) {
    if (mode_ == Mode::direct) {
      return run_concurrent(configuration, [handle] {
        initialize_direct_worker();
        return std::make_unique<DirectWorker>(handle.direct);
      });
    }
    if (mode_ == Mode::fast) {
      return run_concurrent(configuration, [this, handle] {
        require_ok(mako_local_thread_attach(), "benchmark worker attach");
        return std::make_unique<FastAbiWorker>(db_, handle.abi);
      });
    }
    return run_concurrent(configuration, [this, handle] {
      require_ok(mako_local_thread_attach(), "benchmark worker attach");
      return std::make_unique<AbiWorker>(db_, handle.abi);
    });
  }

  uint64_t validate(const Configuration &configuration, TableHandle handle) {
    if (mode_ == Mode::direct) {
      DirectWorker worker(handle.direct);
      return validate_table(worker, configuration);
    }
    if (mode_ == Mode::fast) {
      FastAbiWorker worker(db_, handle.abi);
      return validate_table(worker, configuration);
    }
    AbiWorker worker(db_, handle.abi);
    return validate_table(worker, configuration);
  }

private:
  Mode mode_;
  mako_local_db *db_ = nullptr;
};

Mode parse_mode(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error(
        "usage: mako_local_overhead_driver direct|abi|fast");
  const std::string_view mode(argv[1]);
  if (mode == "direct")
    return Mode::direct;
  if (mode == "abi")
    return Mode::abi;
  if (mode == "fast")
    return Mode::fast;
  throw std::runtime_error(
      "mode must be exactly 'direct', 'abi', or 'fast'");
}

void emit_sample(Mode mode, const Configuration &configuration, size_t index,
                 const Sample &sample, uint64_t final_sum) {
  std::cout << "sample mode " << mode_name(mode) << " workload "
            << workload_name(configuration.workload) << " size "
            << configuration.transaction_size << " contention "
            << contention_name(configuration.contention) << " index " << index
            << " duration_ns " << sample.duration_ns << " commits "
            << sample.stats.commits << " conflicts " << sample.stats.conflicts
            << " logical_ops " << sample.stats.logical_operations
            << " validated_keys " << key_count(configuration) << " final_sum "
            << final_sum << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
#ifndef NDEBUG
    throw std::runtime_error(
        "relative overhead measurements require a Release/NDEBUG build");
#endif
    const Mode mode = parse_mode(argc, argv);
    BenchmarkDatabase database(mode);
    std::cout << "mako-local-overhead-v1\n";
    std::cout << "meta mode " << mode_name(mode) << " workers " << kWorkers
              << " warmup_key_touches " << kWarmupKeyTouches
              << " sample_key_touches " << kSampleKeyTouches << " repetitions "
              << kRepetitions << " lifetime_worker_ids " << kLifetimeWorkerIds
              << '\n';
    for (const Configuration &configuration : configurations()) {
      const TableHandle table = database.create_table(configuration);
      const std::vector<Sample> samples = database.run(configuration, table);
      const uint64_t final_sum = database.validate(configuration, table);
      for (size_t index = 0; index != samples.size(); ++index)
        emit_sample(mode, configuration, index, samples[index], final_sum);
    }
    std::cout << "end mode " << mode_name(mode) << '\n';
    std::cout.flush();
    if (!std::cout)
      throw std::runtime_error("failed to write benchmark results");
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "mako-local overhead driver error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "mako-local overhead driver error: unknown exception\n";
  }
  return EXIT_FAILURE;
}
