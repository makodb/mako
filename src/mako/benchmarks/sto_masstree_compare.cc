#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sto/MassTrans.hh"

namespace {

using Table =
    MassTrans<std::uint64_t, versioned_value_struct<std::uint64_t>, false>;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSplitMixGamma = UINT64_C(0x9E3779B97F4A7C15);
constexpr std::size_t kPrepopulateBatch = 64;

std::uint64_t splitmix_scramble(std::uint64_t value) {
  value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
  value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
  return value ^ (value >> 31U);
}

std::uint64_t worker_random_state(std::uint64_t seed,
                                  std::uint64_t thread_id) {
  /*
   * A direct thread_id * kSplitMixGamma offset selects nearby positions in
   * one stream. Workers two IDs apart then reuse almost every key/write pair.
   * Scramble the selector to choose distant deterministic starting points.
   */
  return splitmix_scramble(seed + (thread_id + 1) * kSplitMixGamma);
}

struct Config {
  std::uint64_t threads = 1;
  std::uint64_t keyspace = 100000;
  std::uint64_t ops_per_txn = 10;
  std::uint64_t write_percent = 50;
  std::uint64_t warmup_ms = 1000;
  std::uint64_t duration_ms = 3000;
  std::uint64_t seed = 1;
};

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t state) : state_(state) {}

  std::uint64_t next_u64() {
    state_ += kSplitMixGamma;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

struct Operation {
  std::uint64_t key_number;
  std::string key;
  bool is_write;
};

struct alignas(64) ThreadResult {
  std::uint64_t commits = 0;
  std::uint64_t attempts = 0;
  std::uint64_t aborts = 0;
  std::uint64_t checksum = 0;
};

enum class Phase : std::uint8_t {
  kReady,
  kWarmup,
  kQuiesce,
  kMeasure,
  kStop,
};

void print_usage(std::ostream& out, const char* program) {
  out << "Usage: " << program
      << " [--threads N] [--keyspace N] [--ops-per-txn N]"
         " [--write-percent N] [--warmup-ms N] [--duration-ms N]"
         " [--seed N]\n";
}

std::uint64_t parse_u64(std::string_view text, std::string_view option) {
  std::uint64_t value = 0;
  const char* first = text.data();
  const char* last = first + text.size();
  const auto [end, error] = std::from_chars(first, last, value);
  if (error != std::errc{} || end != last || text.empty()) {
    throw std::invalid_argument("invalid integer for " + std::string(option) +
                                ": " + std::string(text));
  }
  return value;
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      print_usage(std::cout, argv[0]);
      std::exit(0);
    }
    if (!argument.starts_with("--")) {
      throw std::invalid_argument("unexpected argument: " +
                                  std::string(argument));
    }

    std::string_view option = argument;
    std::string_view value;
    const auto equals = argument.find('=');
    if (equals != std::string_view::npos) {
      option = argument.substr(0, equals);
      value = argument.substr(equals + 1);
    } else {
      if (++index >= argc) {
        throw std::invalid_argument("missing value for " +
                                    std::string(option));
      }
      value = argv[index];
    }

    const auto parsed = parse_u64(value, option);
    if (option == "--threads") {
      config.threads = parsed;
    } else if (option == "--keyspace") {
      config.keyspace = parsed;
    } else if (option == "--ops-per-txn") {
      config.ops_per_txn = parsed;
    } else if (option == "--write-percent") {
      config.write_percent = parsed;
    } else if (option == "--warmup-ms") {
      config.warmup_ms = parsed;
    } else if (option == "--duration-ms") {
      config.duration_ms = parsed;
    } else if (option == "--seed") {
      config.seed = parsed;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }

  if (config.threads == 0 || config.threads >= MAX_THREADS) {
    throw std::invalid_argument("--threads must be in [1, " +
                                std::to_string(MAX_THREADS - 1) +
                                "] (one STO thread id is reserved for loading)");
  }
  if (config.keyspace == 0) {
    throw std::invalid_argument("--keyspace must be greater than zero");
  }
  if (config.ops_per_txn == 0 || config.ops_per_txn > 32768) {
    throw std::invalid_argument("--ops-per-txn must be in [1, 32768]");
  }
  if (config.keyspace < config.ops_per_txn) {
    throw std::invalid_argument(
        "--keyspace must be at least --ops-per-txn so transaction keys can "
        "be unique");
  }
  if (config.write_percent > 100) {
    throw std::invalid_argument("--write-percent must be in [0, 100]");
  }
  if (config.duration_ms == 0 ||
      config.duration_ms >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument(
        "--duration-ms must be in [1, INT64_MAX]");
  }
  if (config.warmup_ms >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("--warmup-ms must not exceed INT64_MAX");
  }
  return config;
}

std::string encode_key(std::uint64_t value) {
  std::string encoded(8, '\0');
  for (unsigned byte = 0; byte != 8; ++byte) {
    encoded[7 - byte] = static_cast<char>(value >> (byte * 8U));
  }
  return encoded;
}

Table::Str as_str(const std::string& value) {
  return Table::Str(value.data(), static_cast<int>(value.size()));
}

void initialize_sto_thread(std::uint64_t id, bool loading) {
  TThread::set_id(static_cast<int>(id));
  TThread::set_pid(0);
  TThread::set_mode(0);
  TThread::disable_multiversion();
  TThread::set_is_micro(1);
  TThread::set_nshards(1);
  TThread::set_shard_index(0);
  TThread::set_warehouses(1);
  TThread::readset_shard_bits = 0;
  TThread::writeset_shard_bits = 0;
  TThread::transget_without_throw = false;
  TThread::transget_without_stable = false;
  TThread::trans_nosend_abort = 0;
  TThread::in_loading_phase = loading;
  TThread::increment_id = 0;
  TThread::skipBeforeRemoteNewOrder = 0;
  TThread::skipBeforeRemotePayment = 0;
  TThread::isHomeWarehouse = true;
  TThread::isRemoteShard = false;
  TThread::is_worker_leader = false;
  Table::thread_init();
  (void)Sto::transaction();
  Sto::update_threadid();
}

bool try_prepopulate_batch(Table& table, std::uint64_t begin,
                           std::uint64_t end) {
  Sto::start_transaction();
  try {
    for (std::uint64_t key_number = begin; key_number != end; ++key_number) {
      const std::string key = encode_key(key_number);
      table.transPut(as_str(key), key_number);
    }
    return Sto::try_commit();
  } catch (const Transaction::Abort&) {
    Sto::silent_abort();
    return false;
  } catch (...) {
    Sto::silent_abort();
    throw;
  }
}

void prepopulate(Table& table, std::uint64_t keyspace) {
  for (std::uint64_t begin = 0; begin < keyspace;) {
    const std::uint64_t remaining = keyspace - begin;
    const std::uint64_t batch =
        std::min<std::uint64_t>(remaining, kPrepopulateBatch);
    const std::uint64_t end = begin + batch;
    while (!try_prepopulate_batch(table, begin, end)) {
    }
    begin = end;
  }
}

void materialize_transaction(SplitMix64& random, const Config& config,
                             std::vector<Operation>& operations) {
  operations.clear();
  for (std::uint64_t index = 0; index != config.ops_per_txn; ++index) {
    std::uint64_t key_number = random.next_u64() % config.keyspace;
    for (;;) {
      bool duplicate = false;
      for (const auto& operation : operations) {
        if (operation.key_number == key_number) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        break;
      }
      key_number = key_number + 1 == config.keyspace ? 0 : key_number + 1;
    }
    const bool is_write = random.next_u64() % 100 < config.write_percent;
    operations.push_back(
        Operation{key_number, encode_key(key_number), is_write});
  }
}

struct AttemptResult {
  bool committed;
  std::uint64_t checksum;
};

AttemptResult run_attempt(Table& table,
                          const std::vector<Operation>& operations) {
  std::uint64_t checksum = 0;
  Sto::start_transaction();
  try {
    for (const auto& operation : operations) {
      std::uint64_t value = 0;
      const bool found = table.transGet(as_str(operation.key), value);
      if (!found || TThread::transget_without_throw || !Sto::in_progress()) {
        Sto::silent_abort();
        return {false, 0};
      }
      checksum += value;
      if (operation.is_write) {
        table.transPut(as_str(operation.key), value + 1);
      }
    }
    if (Sto::try_commit()) {
      return {true, checksum};
    }
    return {false, 0};
  } catch (const Transaction::Abort&) {
    Sto::silent_abort();
    return {false, 0};
  } catch (...) {
    Sto::silent_abort();
    throw;
  }
}

void worker_main(std::uint64_t thread_id, Table& table, const Config& config,
                 std::atomic<Phase>& phase, std::atomic<std::uint64_t>& ready,
                 std::atomic<std::uint64_t>& quiesced,
                 ThreadResult& result) {
  initialize_sto_thread(thread_id, false);
  const std::uint64_t initial_state =
      worker_random_state(config.seed, thread_id);
  SplitMix64 random(initial_state);
  std::vector<Operation> operations;
  operations.reserve(static_cast<std::size_t>(config.ops_per_txn));

  ready.fetch_add(1, std::memory_order_release);
  Phase previous_phase = Phase::kReady;
  bool reported_quiescence = false;
  for (;;) {
    const Phase transaction_phase = phase.load(std::memory_order_acquire);
    if (transaction_phase == Phase::kReady) {
      std::this_thread::yield();
      continue;
    }
    if (transaction_phase == Phase::kQuiesce) {
      if (!reported_quiescence) {
        quiesced.fetch_add(1, std::memory_order_release);
        reported_quiescence = true;
      }
      previous_phase = transaction_phase;
      std::this_thread::yield();
      continue;
    }
    if (transaction_phase == Phase::kStop) {
      return;
    }
    reported_quiescence = false;
    if (transaction_phase == Phase::kMeasure &&
        previous_phase != Phase::kMeasure) {
      // Warmup throughput must not change the measured operation stream.
      random = SplitMix64(initial_state);
    }
    previous_phase = transaction_phase;

    materialize_transaction(random, config, operations);
    std::uint64_t transaction_attempts = 0;
    std::uint64_t transaction_aborts = 0;
    AttemptResult attempt{false, 0};
    do {
      if (phase.load(std::memory_order_acquire) == Phase::kStop) {
        return;
      }
      ++transaction_attempts;
      attempt = run_attempt(table, operations);
      if (!attempt.committed) {
        ++transaction_aborts;
      }
    } while (!attempt.committed);

    if (transaction_phase == Phase::kMeasure &&
        phase.load(std::memory_order_acquire) == Phase::kMeasure) {
      ++result.commits;
      result.attempts += transaction_attempts;
      result.aborts += transaction_aborts;
      result.checksum += attempt.checksum;
    }
  }
}

void wait_for_count(const std::atomic<std::uint64_t>& counter,
                    std::uint64_t expected) {
  while (counter.load(std::memory_order_acquire) != expected) {
    std::this_thread::yield();
  }
}

int run(const Config& config) {
  Table::static_init();
  Table table;
  table.set_table_id(1);
  table.set_is_remote(false);
  table.set_table_name("sto_masstree_compare");

  initialize_sto_thread(config.threads, true);
  prepopulate(table, config.keyspace);
  TThread::in_loading_phase = false;

  std::atomic<Phase> phase{Phase::kReady};
  std::atomic<std::uint64_t> ready{0};
  std::atomic<std::uint64_t> quiesced{0};
  std::vector<ThreadResult> results(static_cast<std::size_t>(config.threads));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.threads));
  for (std::uint64_t thread_id = 0; thread_id != config.threads; ++thread_id) {
    workers.emplace_back(worker_main, thread_id, std::ref(table),
                         std::cref(config), std::ref(phase), std::ref(ready),
                         std::ref(quiesced),
                         std::ref(results[static_cast<std::size_t>(thread_id)]));
  }

  wait_for_count(ready, config.threads);
  phase.store(Phase::kWarmup, std::memory_order_release);
  std::this_thread::sleep_for(
      std::chrono::milliseconds(static_cast<std::int64_t>(config.warmup_ms)));

  phase.store(Phase::kQuiesce, std::memory_order_release);
  wait_for_count(quiesced, config.threads);

  const auto measure_start = Clock::now();
  phase.store(Phase::kMeasure, std::memory_order_release);
  std::this_thread::sleep_until(
      measure_start +
      std::chrono::milliseconds(static_cast<std::int64_t>(config.duration_ms)));
  phase.store(Phase::kStop, std::memory_order_release);
  const auto measure_end = Clock::now();

  for (auto& worker : workers) {
    worker.join();
  }

  ThreadResult total;
  for (const auto& result : results) {
    total.commits += result.commits;
    total.attempts += result.attempts;
    total.aborts += result.aborts;
    total.checksum += result.checksum;
  }
  const std::uint64_t logical_ops = total.commits * config.ops_per_txn;
  const auto elapsed_ns_signed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(measure_end -
                                                           measure_start)
          .count();
  const std::uint64_t elapsed_ns =
      static_cast<std::uint64_t>(elapsed_ns_signed);
  const double seconds = static_cast<double>(elapsed_ns) / 1.0e9;
  const double txn_per_sec = static_cast<double>(total.commits) / seconds;
  const double ops_per_sec = static_cast<double>(logical_ops) / seconds;

  std::cout << std::fixed << std::setprecision(6)
            << "BENCH_RESULT={\"engine\":\"cpp-sto-masstree\""
            << ",\"threads\":" << config.threads
            << ",\"keyspace\":" << config.keyspace
            << ",\"ops_per_txn\":" << config.ops_per_txn
            << ",\"write_percent\":" << config.write_percent
            << ",\"warmup_ms\":" << config.warmup_ms
            << ",\"duration_ms\":" << config.duration_ms
            << ",\"seed\":" << config.seed
            << ",\"commits\":" << total.commits
            << ",\"attempts\":" << total.attempts
            << ",\"aborts\":" << total.aborts
            << ",\"logical_ops\":" << logical_ops
            << ",\"elapsed_ns\":" << elapsed_ns
            << ",\"txn_per_sec\":" << txn_per_sec
            << ",\"ops_per_sec\":" << ops_per_sec
            << ",\"checksum\":" << total.checksum << "}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse_args(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "sto_masstree_cpp_bench: " << error.what() << '\n';
    print_usage(std::cerr, argv[0]);
    return 2;
  }
}
