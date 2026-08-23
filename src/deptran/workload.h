#pragma once

#include "__dep__.h"
#include "constants.h"
#include "config.h"
#include "txn_reg.h"
#include "tx.h"

namespace janus {

class TxRequest;
class Sharding;

class Workload {
 public:
  typedef struct {
    int n_branch_;
    int n_teller_;
    int n_customer_;
  } tpca_para_t;


  typedef struct {
    int n_table_;
  } rw_benchmark_para_t;

  typedef struct {
    int n_table_a_;
    int n_table_b_;
    int n_table_c_;
    int n_table_d_;
  } micro_bench_para_t;

  union {
    tpca_para_t tpca_para_;
    rw_benchmark_para_t rw_benchmark_para_;
    micro_bench_para_t micro_bench_para_;
  };

  int benchmark_;
  int n_try_;
  int single_server_;
  int fix_id_ = -1;
  std::vector<double>& txn_weight_;
  std::map<string, double>& txn_weights_;
  Sharding* sharding_;
  Sharding* sss_ = nullptr;
  shared_ptr<TxnRegistry> txn_reg_{nullptr};

 public:
  static Workload *CreateWorkload(Config *config);

 public:
  Workload() = delete;
  Workload(Config* config);
  virtual ~Workload();

  virtual void GetTxRequest(TxRequest* req, uint32_t cid) = 0;
  virtual void GetProcedureTypes(std::map<int32_t, std::string> &txn_types);
  virtual void RegisterPrecedures() = 0;

  /*
   * inn_id is piece_type for now. better change in the future.
   */
  void RegP(txntype_t txn_type,
            innid_t inn_id,
            const set<int32_t>& ivars,
            const set<int32_t>& ovars,
            const vector<conf_id_t>& conflicts,
            const sharder_t& sharder,
            const rank_t& rank,
            const ProcHandler& handler
  ) {
    auto& piece = txn_reg_->regs_[txn_type][inn_id];
    piece.input_vars_ = ivars;
    piece.output_vars_ = ovars;
    piece.conflicts_ = conflicts;
    piece.sharder_ = sharder;
    piece.rank_ = rank;
    piece.proc_handler_ = handler;
  }

};

#define PROC \
  [this] (Tx& tx, SimpleCommand& cmd, \
          int32_t *res, map<int32_t, Value> &output)

#define LPROC \
  [this, i] (Tx& tx, SimpleCommand& cmd, \
          int32_t *res, map<int32_t, Value> &output)

#define BEGIN_CB(txn_type, inn_id) \
txn_reg_->regs_[txn_type][inn_id].callback_ = \
[] (TxData *ch, std::map<int32_t, Value> output) -> bool {

#define END_CB  };

#define INPUT_PIE(txn, pie, ...) \
txn_reg_->regs_[txn][pie].input_vars_ \
= {__VA_ARGS__};

#define OUTPUT_PIE(txn, pie, ...) \
txn_reg_->regs_[txn][pie].output_vars_ \
= {__VA_ARGS__};

#define CONFLICT_PIE(txn, pie, ...) \
txn_reg_->regs_[txn][pie].conflicts_ \
= {__VA_ARGS__};

#define SHARD_PIE(txn, pie, tb, ...) \
txn_reg_->regs_[txn][pie].sharder_ \
= std::make_pair(tb, vector<int32_t>({__VA_ARGS__}));


#define CREATE_ROW(schema, row_data) \
    r = mdb::VersionedRow::create(schema, row_data)
} // namespace janus
