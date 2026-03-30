#ifndef RW_BENCHMARK_PIE_H_
#define RW_BENCHMARK_PIE_H_

#include "deptran/workload.h"
#include "../../deptran/scheduler.h"

namespace janus {

extern char RW_BENCHMARK_TABLE[];

#define RW_BENCHMARK_W_TXN  (100)
#define RW_BENCHMARK_R_TXN  (200)
#define RW_BENCHMARK_FINISH (300)
#define RW_BENCHMARK_NOOP (400)
#define RW_BENCHMARK_W_TXN_NAME  "WRITE"
#define RW_BENCHMARK_R_TXN_NAME  "READ"

#define RW_BENCHMARK_W_TXN_0 (101)
#define RW_BENCHMARK_R_TXN_0 (201)

class RwWorkload : public Workload {
 public:
  void RegisterPrecedures() override;
  std::mt19937 rand_gen_{};
  map<cooid_t, int32_t> key_ids_ = {};
  RwWorkload(Config *config);
  virtual void GetTxRequest(TxRequest* req, uint32_t cid) override;
  std::recursive_mutex mtx_{};

  // For Frequency check
  Frequency frequency_;
  ~RwWorkload() {
    Log_info("Generated Frequency: %s", frequency_.top_keys_pcts().c_str());
  }
 protected:
  int32_t GetId(uint32_t cid);
  void GenerateWriteRequest(TxRequest *req, uint32_t cid);
  void GenerateReadRequest(TxRequest *req, uint32_t cid);
};

} // namespace janus

#endif
