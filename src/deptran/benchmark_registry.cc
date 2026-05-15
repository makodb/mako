#include "benchmark_registry.h"


import std;

namespace janus {

BenchmarkRegistry& BenchmarkRegistry::Instance() {
  static BenchmarkRegistry registry;
  return registry;
}

void BenchmarkRegistry::SetTableNames(BenchmarkTableNames names) {
  std::lock_guard<std::mutex> lock(mu_);
  table_names_ = std::move(names);
}

BenchmarkTableNames BenchmarkRegistry::GetTableNames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return table_names_;
}

void BenchmarkRegistry::RegisterShardingFactory(int benchmark, ShardingFactory factory) {
  std::lock_guard<std::mutex> lock(mu_);
  sharding_factories_[benchmark] = std::move(factory);
}

void BenchmarkRegistry::RegisterTxnFactory(int benchmark, TxDataFactory factory) {
  std::lock_guard<std::mutex> lock(mu_);
  txn_factories_[benchmark] = std::move(factory);
}

void BenchmarkRegistry::RegisterTxnTypes(int benchmark, TxTypeMap tx_types) {
  std::lock_guard<std::mutex> lock(mu_);
  txn_types_[benchmark] = std::move(tx_types);
}

void BenchmarkRegistry::RegisterTxGeneratorFactory(int benchmark, WorkloadFactory factory) {
  std::lock_guard<std::mutex> lock(mu_);
  tx_generator_factories_[benchmark] = std::move(factory);
}

void BenchmarkRegistry::RegisterWorkloadFactory(int benchmark, WorkloadFactory factory) {
  std::lock_guard<std::mutex> lock(mu_);
  workload_factories_[benchmark] = std::move(factory);
}

Sharding* BenchmarkRegistry::CreateSharding(int benchmark) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sharding_factories_.find(benchmark);
  return it == sharding_factories_.end() ? nullptr : it->second();
}

TxData* BenchmarkRegistry::CreateTxn(int benchmark) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = txn_factories_.find(benchmark);
  return it == txn_factories_.end() ? nullptr : it->second();
}

BenchmarkRegistry::TxTypeMap BenchmarkRegistry::GetTxnTypes(int benchmark) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = txn_types_.find(benchmark);
  return it == txn_types_.end() ? TxTypeMap{} : it->second;
}

Workload* BenchmarkRegistry::CreateTxGenerator(int benchmark, Config* config) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = tx_generator_factories_.find(benchmark);
  return it == tx_generator_factories_.end() ? nullptr : it->second(config);
}

Workload* BenchmarkRegistry::CreateWorkload(int benchmark, Config* config) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = workload_factories_.find(benchmark);
  return it == workload_factories_.end() ? nullptr : it->second(config);
}

}  // namespace janus

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
void janus_register_bench_factories(void) __attribute__((weak));
#else
void janus_register_bench_factories(void);
#endif
}

namespace janus {

void EnsureBenchmarkRegistryInitialized() {
  static std::once_flag once;
  std::call_once(once, []() {
    if (janus_register_bench_factories != nullptr) {
      janus_register_bench_factories();
    }
  });
}

}  // namespace janus
