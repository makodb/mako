#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace janus {

class Config;
class Sharding;
class TxData;
class Workload;

struct BenchmarkTableNames {
  std::string tpcc_warehouse;
  std::string tpcc_district;
  std::string tpcc_history;
  std::string tpcc_order;
  std::string tpcc_item;
  std::string tpcc_stock;
  std::string tpcc_order_c_id_secondary;
  std::string rw_benchmark_table;
  std::string micro_table_a;
  std::string micro_table_b;
  std::string micro_table_c;
  std::string micro_table_d;
};

class BenchmarkRegistry {
 public:
  using ShardingFactory = std::function<Sharding*()>;
  using TxDataFactory = std::function<TxData*()>;
  using WorkloadFactory = std::function<Workload*(Config*)>;
  using TxTypeMap = std::map<int32_t, std::string>;

  static BenchmarkRegistry& Instance();

  void SetTableNames(BenchmarkTableNames names);
  BenchmarkTableNames GetTableNames() const;

  void RegisterShardingFactory(int benchmark, ShardingFactory factory);
  void RegisterTxnFactory(int benchmark, TxDataFactory factory);
  void RegisterTxnTypes(int benchmark, TxTypeMap tx_types);
  void RegisterTxGeneratorFactory(int benchmark, WorkloadFactory factory);
  void RegisterWorkloadFactory(int benchmark, WorkloadFactory factory);

  Sharding* CreateSharding(int benchmark) const;
  TxData* CreateTxn(int benchmark) const;
  TxTypeMap GetTxnTypes(int benchmark) const;
  Workload* CreateTxGenerator(int benchmark, Config* config) const;
  Workload* CreateWorkload(int benchmark, Config* config) const;

 private:
  BenchmarkRegistry() = default;

  mutable std::mutex mu_;
  BenchmarkTableNames table_names_;
  std::map<int, ShardingFactory> sharding_factories_;
  std::map<int, TxDataFactory> txn_factories_;
  std::map<int, TxTypeMap> txn_types_;
  std::map<int, WorkloadFactory> tx_generator_factories_;
  std::map<int, WorkloadFactory> workload_factories_;
};

// Lazily invokes optional bench-side registration when available.
void EnsureBenchmarkRegistryInitialized();

}  // namespace janus
