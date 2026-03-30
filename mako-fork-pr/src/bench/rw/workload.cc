#include "deptran/__dep__.h"
#include "bench/tpcc/workload.h"
#include "workload.h"
#include "../tpca/zipf.h"

namespace janus {
char RW_BENCHMARK_TABLE[] = "history";

void RwWorkload::RegisterPrecedures() {
  RegP(RW_BENCHMARK_R_TXN, RW_BENCHMARK_R_TXN_0,
       {}, // TODO i
       {}, // TODO o
       {}, // TODO c
       {TPCC_TB_HISTORY, {TPCC_VAR_H_KEY}}, // s
       DF_NO,
       PROC {
           mdb::MultiBlob buf(1);
           Value result(0);
           verify(cmd.input.size() == 1);
           auto id = cmd.input[0].get_i64();
           buf[0] = cmd.input[0].get_blob();
           auto tbl = tx.GetTable(RW_BENCHMARK_TABLE);
           auto row = tx.Query(tbl, buf);
           tx.ReadColumn(row, 1, &result, TXN_BYPASS);
           output[0] = result;
           *res = SUCCESS;
           return;
       }
  );

  RegP(RW_BENCHMARK_W_TXN, RW_BENCHMARK_W_TXN_0,
       {}, // TODO i
       {}, // TODO o
       {}, // TODO c
       {TPCC_TB_HISTORY, {TPCC_VAR_H_KEY}}, // s
       DF_REAL,
       PROC {
         mdb::MultiBlob buf(1);
         Value result(0);
         verify(cmd.input.size() == 2);
         auto id = cmd.input[0].get_i64();
         buf[0] = cmd.input[0].get_blob();
         auto tbl = tx.GetTable(RW_BENCHMARK_TABLE);
         auto row = tx.Query(tbl, buf);
         //tx.ReadColumn(row, 1, &result, TXN_BYPASS);
         //result.set_i32(result.get_i32() + 1);
         tx.WriteColumn(row, 1, result, TXN_DEFERRED);
         *res = SUCCESS;
         return;
       }
  );
}

RwWorkload::RwWorkload(Config *config) : Workload(config) {
  std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> time_since_epoch = now.time_since_epoch();
  double seconds_since_epoch = time_since_epoch.count();
  // rand_gen_.seed((int)std::time(0) + (uint64_t)pthread_self());
  rand_gen_.seed((uint64_t)(seconds_since_epoch * 10000000000) + (uint64_t)pthread_self());
  // Log_info("seed %d %d %d %.10f", (int)std::time(0), (uint64_t)pthread_self(), (int)std::time(0) + (uint64_t)pthread_self(), seconds_since_epoch);
  Log_info("seed %llu %llu %llu", (uint64_t)(seconds_since_epoch * 10000000000), (uint64_t)pthread_self(), (uint64_t)(seconds_since_epoch * 10000000000) + (uint64_t)pthread_self());
}

void RwWorkload::GetTxRequest(TxRequest* req, uint32_t cid) {
  req->n_try_ = n_try_;
  // Log_info("Read Weights %.3f Write Weights %.3f", txn_weights_["read"], txn_weights_["write"]);
  std::vector<double> weights = {txn_weights_["read"], txn_weights_["write"]};
  int32_t key, value;
  switch (RandomGenerator::weighted_select(weights)) {
    case 0: // read
      GenerateReadRequest(req, cid);
      key = req->input_.values_->at(0).get_i32();
      break;
    case 1: // write
      GenerateWriteRequest(req, cid);
      key = req->input_.values_->at(0).get_i32();
      value = req->input_.values_->at(1).get_i32();
      break;
    default:
      verify(0);
  }
}

void RwWorkload::GenerateWriteRequest(
    TxRequest *req, uint32_t cid) {
  auto id = this->GetId(cid);
  req->tx_type_ = RW_BENCHMARK_W_TXN;
  req->input_ = {
      {0, Value((i32) id)},
      {1, Value((i32) RandomGenerator::rand(0, 10000))}
  };
}

void RwWorkload::GenerateReadRequest(
    TxRequest *req, uint32_t cid) {
  auto id = GetId(cid);
  req->tx_type_ = RW_BENCHMARK_R_TXN;
  req->input_ = {
      {0, Value((i32) id)}
  };
}

int32_t RwWorkload::GetId(uint32_t cid) {
  // verify(cid < 1000000);
  int32_t id;
  auto& dist = Config::GetConfig()->dist_;
  static int32_t key_range = Config::GetConfig()->range_ == -1 ? rw_benchmark_para_.n_table_ : Config::GetConfig()->range_;
  if (dist == "zipf") {
    static auto theta = Config::GetConfig()->coeffcient_;
    static ZipfDist d(theta, key_range);
    id = d(rand_gen_);
  } else if (fix_id_ == -1) {
    // Log_info("[Jetpack] id range is (0, %d)", key_range - 1);
    id = RandomGenerator::rand(0, key_range - 1);
  } else {
    auto it = this->key_ids_.find(cid);
    if (it == key_ids_.end()) {
      id = fix_id_;
      id += (cid & 0xFFFFFFFF);
      id = (id<0) ? -1*id : id;
      id %= rw_benchmark_para_.n_table_;
      key_ids_[cid] = id;
      Log_info("coordinator %d using fixed id of %d", cid, id);
    } else {
      id = it->second;
    }
  }
  frequency_.append(id);
  return id;
}
} // namespace janus

