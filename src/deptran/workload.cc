#include "workload.h"
#include "config.h"
#include "constants.h"
#include "sharding.h"
#include "benchmark_registry.h"

#include <memory>

namespace janus {

Workload* Workload::CreateWorkload(Config *config) {
  EnsureBenchmarkRegistryInitialized();
  auto& registry = BenchmarkRegistry::Instance();
  Workload* workload = registry.CreateWorkload(config->benchmark(), config);
  verify(workload != nullptr);
  return workload;
}

Workload::Workload(Config* config)
    : txn_weight_(config->get_txn_weight()),
      txn_weights_(config->get_txn_weights()) {
  benchmark_ = Config::GetConfig()->benchmark();
  n_try_ = Config::GetConfig()->get_max_retry();
  single_server_ = Config::GetConfig()->get_single_server();

  EnsureBenchmarkRegistryInitialized();
  auto sharding = std::unique_ptr<Sharding>(
      BenchmarkRegistry::Instance().CreateSharding(benchmark_));
  verify(sharding != nullptr);
  std::map<std::string, uint64_t> table_num_rows;
  sharding->get_number_rows(table_num_rows);

  if (Config::GetConfig()->dist_ == "fixed") {
    single_server_ = Config::SS_PROCESS_SINGLE;
  }
  auto table_names = BenchmarkRegistry::Instance().GetTableNames();

  switch (benchmark_) {
    case MICRO_BENCH:
      verify(!table_names.micro_table_a.empty());
      verify(!table_names.micro_table_b.empty());
      verify(!table_names.micro_table_c.empty());
      verify(!table_names.micro_table_d.empty());
      micro_bench_para_.n_table_a_ = table_num_rows[table_names.micro_table_a];
      micro_bench_para_.n_table_b_ = table_num_rows[table_names.micro_table_b];
      micro_bench_para_.n_table_c_ = table_num_rows[table_names.micro_table_c];
      micro_bench_para_.n_table_d_ = table_num_rows[table_names.micro_table_d];
      break;
    case TPCA:
    case TPCC:
    case TPCC_DIST_PART:
    case TPCC_REAL_DIST_PART: {
      break;
    }
    case RW_BENCHMARK:
      verify(!table_names.rw_benchmark_table.empty());
      rw_benchmark_para_.n_table_ = table_num_rows[table_names.rw_benchmark_table];
      fix_id_ = (Config::GetConfig()->dist_ == "fixed") ?
                RandomGenerator::rand(0, rw_benchmark_para_.n_table_) :
                -1;
      break;
    default:
      Log_fatal("benchmark not implemented");
      verify(0);
  }
}

void Workload::GetProcedureTypes(map<int32_t, string> &txn_types) {
  EnsureBenchmarkRegistryInitialized();
  txn_types = BenchmarkRegistry::Instance().GetTxnTypes(benchmark_);
  verify(!txn_types.empty());
}

Workload::~Workload() {
}

} // namespace janus
