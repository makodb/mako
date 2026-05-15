
#include "deptran/benchmark_registry.h"
#include "deptran/constants.h"

#include "bench/micro/procedure.h"
#include "bench/micro/workload.h"
#include "bench/rw/procedure.h"
#include "bench/rw/sharding.h"
#include "bench/rw/workload.h"
#include "bench/tpca/payment.h"
#include "bench/tpca/sharding.h"
#include "bench/tpca/workload.h"
#include "bench/tpcc/procedure.h"
#include "bench/tpcc/sharding.h"
#include "bench/tpcc/workload.h"
#include "bench/tpcc_dist/procedure.h"
#include "bench/tpcc_real_dist/procedure.h"
#include "bench/tpcc_real_dist/sharding.h"
#include "bench/tpcc_real_dist/workload.h"

import std;

extern "C" void janus_register_bench_factories(void) {
  auto& registry = janus::BenchmarkRegistry::Instance();

  janus::BenchmarkTableNames table_names;
  table_names.tpcc_warehouse = janus::TPCC_TB_WAREHOUSE;
  table_names.tpcc_district = janus::TPCC_TB_DISTRICT;
  table_names.tpcc_history = janus::TPCC_TB_HISTORY;
  table_names.tpcc_order = janus::TPCC_TB_ORDER;
  table_names.tpcc_item = janus::TPCC_TB_ITEM;
  table_names.tpcc_stock = janus::TPCC_TB_STOCK;
  table_names.tpcc_order_c_id_secondary = janus::TPCC_TB_ORDER_C_ID_SECONDARY;
  table_names.rw_benchmark_table = janus::RW_BENCHMARK_TABLE;
  table_names.micro_table_a = janus::MICRO_BENCH_TABLE_A;
  table_names.micro_table_b = janus::MICRO_BENCH_TABLE_B;
  table_names.micro_table_c = janus::MICRO_BENCH_TABLE_C;
  table_names.micro_table_d = janus::MICRO_BENCH_TABLE_D;
  registry.SetTableNames(std::move(table_names));

  registry.RegisterShardingFactory(TPCC_REAL_DIST_PART,
                                   []() -> janus::Sharding* { return new janus::TpccdSharding(); });
  registry.RegisterShardingFactory(TPCC,
                                   []() -> janus::Sharding* { return new janus::TpccSharding(); });
  registry.RegisterShardingFactory(RW_BENCHMARK,
                                   []() -> janus::Sharding* { return new janus::RWBenchmarkSharding(); });
  registry.RegisterShardingFactory(TPCA,
                                   []() -> janus::Sharding* { return new janus::TpcaSharding(); });

  registry.RegisterTxnFactory(TPCA,
                              []() -> janus::TxData* { return new janus::TpcaPaymentChopper(); });
  registry.RegisterTxnTypes(TPCA, {
      {TPCA_PAYMENT, TPCA_PAYMENT_NAME},
  });
  registry.RegisterTxnFactory(TPCC,
                              []() -> janus::TxData* { return new janus::TpccProcedure(); });
  registry.RegisterTxnTypes(TPCC, {
      {TPCC_NEW_ORDER, TPCC_NEW_ORDER_NAME},
      {TPCC_PAYMENT, TPCC_PAYMENT_NAME},
      {TPCC_STOCK_LEVEL, TPCC_STOCK_LEVEL_NAME},
      {TPCC_DELIVERY, TPCC_DELIVERY_NAME},
      {TPCC_ORDER_STATUS, TPCC_ORDER_STATUS_NAME},
  });
  registry.RegisterTxnFactory(TPCC_DIST_PART,
                              []() -> janus::TxData* { return new janus::TpccDistChopper(); });
  registry.RegisterTxnTypes(TPCC_DIST_PART, {
      {TPCC_NEW_ORDER, TPCC_NEW_ORDER_NAME},
      {TPCC_PAYMENT, TPCC_PAYMENT_NAME},
      {TPCC_STOCK_LEVEL, TPCC_STOCK_LEVEL_NAME},
      {TPCC_DELIVERY, TPCC_DELIVERY_NAME},
      {TPCC_ORDER_STATUS, TPCC_ORDER_STATUS_NAME},
  });
  registry.RegisterTxnFactory(TPCC_REAL_DIST_PART,
                              []() -> janus::TxData* { return new janus::TpccRdProcedure(); });
  registry.RegisterTxnTypes(TPCC_REAL_DIST_PART, {
      {TPCC_NEW_ORDER, TPCC_NEW_ORDER_NAME},
      {TPCC_PAYMENT, TPCC_PAYMENT_NAME},
      {TPCC_STOCK_LEVEL, TPCC_STOCK_LEVEL_NAME},
      {TPCC_DELIVERY, TPCC_DELIVERY_NAME},
      {TPCC_ORDER_STATUS, TPCC_ORDER_STATUS_NAME},
  });
  registry.RegisterTxnFactory(RW_BENCHMARK,
                              []() -> janus::TxData* { return new janus::RWChopper(); });
  registry.RegisterTxnTypes(RW_BENCHMARK, {
      {RW_BENCHMARK_W_TXN, RW_BENCHMARK_W_TXN_NAME},
      {RW_BENCHMARK_R_TXN, RW_BENCHMARK_R_TXN_NAME},
  });
  registry.RegisterTxnFactory(MICRO_BENCH,
                              []() -> janus::TxData* { return new janus::MicroProcedure(); });
  registry.RegisterTxnTypes(MICRO_BENCH, {
      {MICRO_BENCH_R, MICRO_BENCH_R_NAME},
      {MICRO_BENCH_W, MICRO_BENCH_W_NAME},
  });

  registry.RegisterTxGeneratorFactory(TPCC,
                                      [](janus::Config* config) -> janus::Workload* {
                                        return new janus::TpccWorkload(config);
                                      });
  registry.RegisterTxGeneratorFactory(TPCC_DIST_PART,
                                      [](janus::Config* config) -> janus::Workload* {
                                        return new janus::TpccRdWorkload(config);
                                      });
  registry.RegisterTxGeneratorFactory(TPCC_REAL_DIST_PART,
                                      [](janus::Config* config) -> janus::Workload* {
                                        return new janus::TpccRdWorkload(config);
                                      });
  registry.RegisterTxGeneratorFactory(TPCA,
                                      [](janus::Config* config) -> janus::Workload* {
                                        return new janus::TpcaWorkload(config);
                                      });
  registry.RegisterTxGeneratorFactory(RW_BENCHMARK,
                                      [](janus::Config* config) -> janus::Workload* {
                                        return new janus::RwWorkload(config);
                                      });

  registry.RegisterWorkloadFactory(TPCA,
                                   [](janus::Config* config) -> janus::Workload* {
                                     return new janus::TpcaWorkload(config);
                                   });
  registry.RegisterWorkloadFactory(TPCC,
                                   [](janus::Config* config) -> janus::Workload* {
                                     return new janus::TpccWorkload(config);
                                   });
  registry.RegisterWorkloadFactory(TPCC_DIST_PART,
                                   [](janus::Config* config) -> janus::Workload* {
                                     return new janus::TpccWorkload(config);
                                   });
  registry.RegisterWorkloadFactory(TPCC_REAL_DIST_PART,
                                   [](janus::Config* config) -> janus::Workload* {
                                     return new janus::TpccRdWorkload(config);
                                   });
  registry.RegisterWorkloadFactory(RW_BENCHMARK,
                                   [](janus::Config* config) -> janus::Workload* {
                                     return new janus::RwWorkload(config);
                                   });
  registry.RegisterWorkloadFactory(MICRO_BENCH,
                                   [](janus::Config* config) -> janus::Workload* {
                                     return new janus::MicroWorkload(config);
                                   });
}
