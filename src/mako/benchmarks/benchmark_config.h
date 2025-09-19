#ifndef _NDB_BENCHMARK_CONFIG_H_
#define _NDB_BENCHMARK_CONFIG_H_

#include <stdint.h>
#include <string>
#include <vector>
#include <atomic>
#include <utility>
#include <unordered_map>
#include "lib/configuration.h"
#include "lib/common.h"
#include "lib/helper_queue.h"
#include "lib/fasttransport.h"

enum {
  RUNMODE_TIME = 0,
  RUNMODE_OPS  = 1
};

class BenchmarkConfig {
  private:
      // Private constructor with default values
      BenchmarkConfig() : 
          nthreads_(1),
          num_erpc_server_(2), // number of erpc pull threads
          scale_factor_(1.0),
          nshards_(1), // default 1 shard
          shardIndex_(0), // default on the shard-0
          cluster_("localhost"),
          clusterRole_(0), 
          config_(nullptr),
          running_(true),
          control_mode_(0),
          verbose_(1),
          txn_flags_(1),
          runtime_(30),
          runtime_plus_(0),
          ops_per_worker_(0),
          run_mode_(RUNMODE_TIME),
          enable_parallel_loading_(0),
          pin_cpus_(1),
          slow_exit_(0),
          retry_aborted_transaction_(1),
          no_reset_counters_(0),
          backoff_aborted_transaction_(0),
          use_hashtable_(0),
          is_micro_(0), // if run micro-based workload
          end_received_(0),
          end_received_leader_(0),
          replay_batch_(0) {}
      
      // Member variables from dbtest.cc
      size_t nthreads_;
      size_t nshards_;
      size_t num_erpc_server_;
      size_t shardIndex_;
      std::string cluster_;
      int clusterRole_;
      transport::Configuration* config_;
      volatile bool running_;
      volatile int control_mode_;
      int verbose_;
      uint64_t txn_flags_;
      double scale_factor_;
      uint64_t runtime_;
      volatile int runtime_plus_;
      uint64_t ops_per_worker_;
      int run_mode_;
      int enable_parallel_loading_;
      int pin_cpus_;
      int slow_exit_;
      int retry_aborted_transaction_;
      int no_reset_counters_;
      int backoff_aborted_transaction_;
      int use_hashtable_;
      int is_micro_;
      int is_replicated_;
      string paxos_proc_name_;
      std::vector<std::string> paxos_config_file_;
      
      // Atomic variables for Paxos termination tracking
      std::atomic<int> end_received_;
      std::atomic<int> end_received_leader_;
      std::atomic<int> replay_batch_;
      
      // Watermark tracking for latency measurements
      std::vector<std::pair<uint32_t, uint32_t>> advanceWatermarkTracker_;

      // Runtime TPCC wiring state (moved from tpcc.cc)
      std::vector<FastTransport*> server_transports_;
      std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_;
      std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_response_;
      std::atomic<int> set_server_transport_{0};


  public:
      // Delete copy/move constructors
      BenchmarkConfig(const BenchmarkConfig&) = delete;
      BenchmarkConfig& operator=(const BenchmarkConfig&) = delete;

      // Single point of access
      static BenchmarkConfig& getInstance() {
          static BenchmarkConfig instance;
          return instance;
      }

      // Getters
      size_t getNthreads() const { return nthreads_; }
      size_t getNshards() const { return nshards_; }
      size_t getNumErpcServer() const { return num_erpc_server_; }
      size_t getShardIndex() const { return shardIndex_; }
      const std::string& getCluster() const { return cluster_; }
      int getClusterRole() const { return clusterRole_; }
      transport::Configuration* getConfig() const { return config_; }
      bool isRunning() const { return running_; }
      int getControlMode() const { return control_mode_; }
      int getVerbose() const { return verbose_; }
      uint64_t getTxnFlags() const { return txn_flags_; }
      double getScaleFactor() const { return scale_factor_; }
      uint64_t getRuntime() const { return runtime_; }
      int getRuntimePlus() const { return runtime_plus_; }
      uint64_t getOpsPerWorker() const { return ops_per_worker_; }
      int getRunMode() const { return run_mode_; }
      int getEnableParallelLoading() const { return enable_parallel_loading_; }
      int getPinCpus() const { return pin_cpus_; }
      int getSlowExit() const { return slow_exit_; }
      int getRetryAbortedTransaction() const { return retry_aborted_transaction_; }
      int getNoResetCounters() const { return no_reset_counters_; }
      int getBackoffAbortedTransaction() const { return backoff_aborted_transaction_; }
      int getUseHashtable() const { return use_hashtable_; }
      int getIsMicro() const { return is_micro_; }
      int getIsReplicated() const { return is_replicated_; }
      std::string getPaxosProcName() const { return paxos_proc_name_; }
      int getLeaderConfig() const { return paxos_proc_name_==mako::LOCALHOST_CENTER; }
      const std::vector<std::string>& getPaxosConfigFile() const { return paxos_config_file_; }
      
      // Runtime TPCC wiring getters
      std::vector<FastTransport*>& getServerTransports() { return server_transports_; }
      const std::vector<FastTransport*>& getServerTransports() const { return server_transports_; }
      std::unordered_map<uint16_t, mako::HelperQueue*>& getQueueHolders() { return queue_holders_; }
      const std::unordered_map<uint16_t, mako::HelperQueue*>& getQueueHolders() const { return queue_holders_; }
      std::unordered_map<uint16_t, mako::HelperQueue*>& getQueueHoldersResponse() { return queue_holders_response_; }
      const std::unordered_map<uint16_t, mako::HelperQueue*>& getQueueHoldersResponse() const { return queue_holders_response_; }
      std::atomic<int>& getServerTransportReadyCounter() { return set_server_transport_; }
      const std::atomic<int>& getServerTransportReadyCounter() const { return set_server_transport_; }

      // Setters
      void setNthreads(size_t n) { nthreads_ = n; setScaleFactor(n); }
      void setNshards(size_t n) { nshards_ = n; }
      void setNumErpcServer(size_t n) { num_erpc_server_ = n; }
      void setShardIndex(size_t idx) { shardIndex_ = idx; }
      void setCluster(const std::string& c) { cluster_ = c; }
      void setClusterRole(int role) { clusterRole_ = role; }
      void setConfig(transport::Configuration* cfg) { config_ = cfg; }
      void setRunning(bool r) { running_ = r; }
      void setControlMode(int mode) { control_mode_ = mode; }
      void setVerbose(int v) { verbose_ = v; }
      void setTxnFlags(uint64_t flags) { txn_flags_ = flags; }
      void setScaleFactor(double sf) { scale_factor_ = sf; }
      void setRuntime(uint64_t rt) { runtime_ = rt; }
      void setRuntimePlus(int rtp) { runtime_plus_ = rtp; }
      void setOpsPerWorker(uint64_t ops) { ops_per_worker_ = ops; }
      void setRunMode(int mode) { run_mode_ = mode; }
      void setEnableParallelLoading(int enable) { enable_parallel_loading_ = enable; }
      void setPinCpus(int pin) { pin_cpus_ = pin; }
      void setSlowExit(int slow) { slow_exit_ = slow; }
      void setRetryAbortedTransaction(int retry) { retry_aborted_transaction_ = retry; }
      void setNoResetCounters(int no_reset) { no_reset_counters_ = no_reset; }
      void setBackoffAbortedTransaction(int backoff) { backoff_aborted_transaction_ = backoff; }
      void setUseHashtable(int use) { use_hashtable_ = use; }
      void setIsMicro(int micro) { is_micro_ = micro; }
      void setIsReplicated(int replicated) { is_replicated_ = replicated; }
      void setPaxosProcName(std::string paxos_proc_name) { paxos_proc_name_ = paxos_proc_name; setCluster(paxos_proc_name); setClusterRole(mako::convertCluster(paxos_proc_name));}
      void setPaxosConfigFile(const std::vector<std::string>& paxos_config_file) { paxos_config_file_ = paxos_config_file; }
      
      // Getters and setters for Paxos termination tracking
      int getEndReceived() const { return end_received_.load(); }
      int getEndReceivedLeader() const { return end_received_leader_.load(); }
      void setEndReceived(int value) { end_received_.store(value); }
      void setEndReceivedLeader(int value) { end_received_leader_.store(value); }
      void incrementEndReceived() { end_received_.fetch_add(1); }
      void incrementEndReceivedLeader() { end_received_leader_.fetch_add(1); }
      
      // Getters and setters for replay batch tracking
      int getReplayBatch() const { return replay_batch_.load(); }
      void setReplayBatch(int value) { replay_batch_.store(value); }
      void incrementReplayBatch() { replay_batch_.fetch_add(1); }
      
      // Getters and setters for watermark tracking
      std::vector<std::pair<uint32_t, uint32_t>>& getAdvanceWatermarkTracker() { return advanceWatermarkTracker_; }
      const std::vector<std::pair<uint32_t, uint32_t>>& getAdvanceWatermarkTracker() const { return advanceWatermarkTracker_; }
};

// (no global runtime accessors)

#endif /* _NDB_BENCHMARK_CONFIG_H_ */
