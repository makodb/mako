#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "mako/masstree_btree.h"
#include "mako/varkey.h"

using PerfTree = single_threaded_btree;

struct ScenarioResult {
  std::string name;
  double ops_per_sec = 0.0;
  size_t operations = 0;
};

struct BenchmarkConfig {
  size_t key_count = 1 << 15;
  size_t lookup_rounds = 4;
  size_t scan_window = 256;
};

struct BenchmarkSummary {
  BenchmarkConfig config;
  std::vector<ScenarioResult> results;
};

namespace {

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm = *std::gmtime(&t);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

double OpsPerSecond(size_t ops, std::chrono::duration<double> elapsed) {
  if (elapsed.count() == 0.0) {
    return 0.0;
  }
  return static_cast<double>(ops) / elapsed.count();
}

class StorageBank {
 public:
  explicit StorageBank(size_t n) : slots_(n) {}

  PerfTree::value_type ValueFor(size_t idx) {
    auto& slot = slots_[idx];
    if (!slot) {
      slot = std::make_unique<uint64_t>(idx);
    }
    return reinterpret_cast<PerfTree::value_type>(slot.get());
  }

 private:
  std::vector<std::unique_ptr<uint64_t>> slots_;
};

class BenchmarkHarness {
 public:
  explicit BenchmarkHarness(BenchmarkConfig cfg)
      : cfg_(cfg) {
    keys_.reserve(cfg_.key_count);
    for (size_t i = 0; i < cfg_.key_count; ++i) {
      keys_.emplace_back(static_cast<uint64_t>(i));
    }
    sequential_order_.resize(cfg_.key_count);
    std::iota(sequential_order_.begin(), sequential_order_.end(), 0);
  }

  BenchmarkSummary RunAll() {
    BenchmarkSummary summary;
    summary.config = cfg_;
    summary.results.push_back(RunSequentialInsert());
    summary.results.push_back(RunRandomInsert());
    summary.results.push_back(RunRandomLookup());
    summary.results.push_back(RunMixedReadWrite());
    summary.results.push_back(RunRangeScan());
    summary.results.push_back(RunSequentialRemove());
    return summary;
  }

 private:
  ScenarioResult RunSequentialInsert() {
    PerfTree tree;
    StorageBank bank(cfg_.key_count);
    auto start = std::chrono::steady_clock::now();
    PopulateTree(&tree, &bank, sequential_order_);
    auto end = std::chrono::steady_clock::now();
    return {"insert_sequential",
            OpsPerSecond(cfg_.key_count, end - start),
            cfg_.key_count};
  }

  ScenarioResult RunRandomInsert() {
    PerfTree tree;
    StorageBank bank(cfg_.key_count);
    auto order = sequential_order_;
    std::shuffle(order.begin(), order.end(), rng_);
    auto start = std::chrono::steady_clock::now();
    PopulateTree(&tree, &bank, order);
    auto end = std::chrono::steady_clock::now();
    return {"insert_random",
            OpsPerSecond(cfg_.key_count, end - start),
            cfg_.key_count};
  }

  ScenarioResult RunRandomLookup() {
    PerfTree tree;
    StorageBank bank(cfg_.key_count);
    PopulateTree(&tree, &bank, sequential_order_);

    std::vector<size_t> order = sequential_order_;
    std::shuffle(order.begin(), order.end(), rng_);

    PerfTree::value_type tmp{};
    size_t operations = cfg_.key_count * cfg_.lookup_rounds;
    auto start = std::chrono::steady_clock::now();
    for (size_t round = 0; round < cfg_.lookup_rounds; ++round) {
      for (size_t idx : order) {
        bool ok = tree.search(keys_[idx], tmp);
        if (!ok || tmp == nullptr) {
          throw std::runtime_error("lookup failed during benchmark");
        }
      }
    }
    auto end = std::chrono::steady_clock::now();
    return {"lookup_random", OpsPerSecond(operations, end - start), operations};
  }

  ScenarioResult RunMixedReadWrite() {
    PerfTree tree;
    StorageBank bank(cfg_.key_count);
    PopulateTree(&tree, &bank, sequential_order_);

    const double read_ratio = 0.8;
    size_t total_ops = cfg_.key_count * cfg_.lookup_rounds;
    std::uniform_int_distribution<size_t> key_dist(0, cfg_.key_count - 1);
    std::uniform_real_distribution<double> mix_dist(0.0, 1.0);
    PerfTree::value_type tmp{};

    auto start = std::chrono::steady_clock::now();
    for (size_t op = 0; op < total_ops; ++op) {
      size_t idx = key_dist(rng_);
      if (mix_dist(rng_) < read_ratio) {
        bool ok = tree.search(keys_[idx], tmp);
        if (!ok || tmp == nullptr) {
          throw std::runtime_error("mixed workload read failed");
        }
      } else {
        tree.insert(keys_[idx], bank.ValueFor(idx));
      }
    }
    auto end = std::chrono::steady_clock::now();
    return {"mixed_read_write", OpsPerSecond(total_ops, end - start), total_ops};
  }

  class CountingRangeCallback : public PerfTree::search_range_callback {
   public:
    bool invoke(const PerfTree::string_type&, PerfTree::value_type) override {
      ++count_;
      return true;
    }
    size_t count() const { return count_; }

   private:
    size_t count_ = 0;
  };

  ScenarioResult RunRangeScan() {
    if (cfg_.key_count == 0) {
      return {"range_scan", 0.0, 0};
    }

    PerfTree tree;
    StorageBank bank(cfg_.key_count);
    PopulateTree(&tree, &bank, sequential_order_);

    size_t window = std::min(cfg_.scan_window, cfg_.key_count - 1);
    std::uniform_int_distribution<size_t> start_dist(0, cfg_.key_count - 1);

    size_t total_keys = 0;
    auto start = std::chrono::steady_clock::now();
    for (size_t round = 0; round < cfg_.lookup_rounds; ++round) {
      size_t lower_idx = start_dist(rng_);
      size_t upper_idx = std::min(cfg_.key_count - 1, lower_idx + window);
      auto lower = keys_[lower_idx];
      auto upper = keys_[upper_idx];
      CountingRangeCallback cb;
      tree.search_range_call(lower, &upper, cb);
      total_keys += cb.count();
    }
    auto end = std::chrono::steady_clock::now();
    return {"range_scan", OpsPerSecond(total_keys, end - start), total_keys};
  }

  ScenarioResult RunSequentialRemove() {
    PerfTree tree;
    StorageBank bank(cfg_.key_count);
    PopulateTree(&tree, &bank, sequential_order_);
    auto start = std::chrono::steady_clock::now();
    for (size_t idx : sequential_order_) {
      tree.remove(keys_[idx]);
    }
    auto end = std::chrono::steady_clock::now();
    return {"remove_sequential",
            OpsPerSecond(cfg_.key_count, end - start),
            cfg_.key_count};
  }

  void PopulateTree(PerfTree* tree, StorageBank* bank,
                    const std::vector<size_t>& order) {
    for (size_t idx : order) {
      tree->insert(keys_[idx], bank->ValueFor(idx));
    }
  }

  BenchmarkConfig cfg_;
  std::vector<u64_varkey> keys_;
  std::vector<size_t> sequential_order_;
  std::mt19937_64 rng_{42};
};

void WriteJson(const BenchmarkSummary& summary, const std::string& output_path) {
  if (output_path.empty()) {
    return;
  }
  std::ofstream out(output_path);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + output_path);
  }
  out << "{\n";
  out << "  \"timestamp\": \"" << CurrentTimestamp() << "\",\n";
  out << "  \"config\": {\n";
  out << "    \"key_count\": " << summary.config.key_count << ",\n";
  out << "    \"lookup_rounds\": " << summary.config.lookup_rounds << ",\n";
  out << "    \"scan_window\": " << summary.config.scan_window << "\n";
  out << "  },\n";
  out << "  \"results\": [\n";
  for (size_t i = 0; i < summary.results.size(); ++i) {
    const auto& result = summary.results[i];
    out << "    { \"name\": \"" << result.name << "\", "
        << "\"ops_per_sec\": " << result.ops_per_sec << ", "
        << "\"operations\": " << result.operations << " }";
    if (i + 1 != summary.results.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
}

std::unordered_map<std::string, double> LoadBaseline(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open baseline file: " + path);
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string content = buffer.str();
  std::unordered_map<std::string, double> metrics;
  size_t pos = 0;
  while ((pos = content.find("\"name\"", pos)) != std::string::npos) {
    auto name_start = content.find('"', pos + 6);
    if (name_start == std::string::npos) {
      break;
    }
    auto name_end = content.find('"', name_start + 1);
    if (name_end == std::string::npos) {
      break;
    }
    std::string name = content.substr(name_start + 1, name_end - name_start - 1);
    auto ops_pos = content.find("\"ops_per_sec\"", name_end);
    if (ops_pos == std::string::npos) {
      break;
    }
    ops_pos = content.find(':', ops_pos);
    if (ops_pos == std::string::npos) {
      break;
    }
    auto ops_end = content.find_first_of(",}\n", ops_pos + 1);
    std::string token = content.substr(ops_pos + 1, ops_end - ops_pos - 1);
    try {
      metrics[name] = std::stod(token);
    } catch (...) {
      // Ignore parse failures.
    }
    pos = name_end + 1;
  }
  return metrics;
}

void PrintComparison(const BenchmarkSummary& summary,
                     const std::unordered_map<std::string, double>& baseline) {
  std::cout << "Performance comparison vs. baseline:\n";
  for (const auto& result : summary.results) {
    auto it = baseline.find(result.name);
    if (it == baseline.end()) {
      std::cout << "  " << result.name << ": no baseline\n";
      continue;
    }
    double delta = result.ops_per_sec - it->second;
    double pct = it->second == 0.0 ? 0.0 : (delta / it->second) * 100.0;
    std::cout << "  " << result.name << ": current=" << result.ops_per_sec
              << " baseline=" << it->second << " ("
              << std::fixed << std::setprecision(2) << pct << "%)\n";
  }
}

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--keys N] [--lookup-rounds M] [--scan-window W]"
            << " [--output file] [--baseline file]\n";
}

}  // namespace

int main(int argc, char** argv) {
  BenchmarkConfig config;
  std::string output_path;
  std::string baseline_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--keys" && i + 1 < argc) {
      config.key_count = std::stoull(argv[++i]);
    } else if (arg == "--lookup-rounds" && i + 1 < argc) {
      config.lookup_rounds = std::stoull(argv[++i]);
    } else if (arg == "--scan-window" && i + 1 < argc) {
      config.scan_window = std::stoull(argv[++i]);
    } else if (arg == "--output" && i + 1 < argc) {
      output_path = argv[++i];
    } else if (arg == "--baseline" && i + 1 < argc) {
      baseline_path = argv[++i];
    } else if (arg == "--help") {
      PrintUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown or incomplete argument: " << arg << "\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }

  if (config.key_count == 0 || config.lookup_rounds == 0) {
    std::cerr << "key count and lookup rounds must be positive numbers.\n";
    return 1;
  }

  try {
    BenchmarkHarness harness(config);
    BenchmarkSummary summary = harness.RunAll();

    std::cout << "Masstree micro-benchmark (keys=" << config.key_count
              << ", lookup_rounds=" << config.lookup_rounds
              << ", scan_window=" << config.scan_window << ")\n";
    for (const auto& result : summary.results) {
      std::cout << "  " << result.name << " = "
                << result.ops_per_sec << " ops/sec ("
                << result.operations << " ops)\n";
    }

    WriteJson(summary, output_path);

    if (!baseline_path.empty()) {
      auto baseline = LoadBaseline(baseline_path);
      PrintComparison(summary, baseline);
    }
  } catch (const std::exception& e) {
    std::cerr << "Benchmark failed: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
