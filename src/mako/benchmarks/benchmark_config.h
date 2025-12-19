#ifndef _NDB_BENCHMARK_CONFIG_H_
#define _NDB_BENCHMARK_CONFIG_H_

#include <stdint.h>
#include <string>
#include <vector>
#include <atomic>
#include <utility>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <chrono>
#include "lib/configuration.h"
#include "lib/common.h"
#include "lib/helper_queue.h"
#include "lib/fasttransport.h"
#include "silo_runtime.h"

enum {
  RUNMODE_TIME = 0,
  RUNMODE_OPS  = 1
};

// Forward declarations
class abstract_db;
class abstract_ordered_index;

// Per-shard state for multi-shard mode
struct ShardContext {
    int shard_index;                                              // Shard index (0, 1, 2, ...)
    std::string cluster_role;                                     // Cluster role (localhost, p1, p2, learner)
    rusty::Arc<SiloRuntime> runtime;                              // Per-shard Silo runtime (epoch, threads, core IDs)
    abstract_db* db;                                              // Per-shard database instance
    FastTransport* transport;                                     // Per-shard transport
    std::vector<FastTransport*> server_transports;                // Per-shard server transports
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders;          // Request queues
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_response; // Response queues
    std::map<int, abstract_ordered_index*> open_tables;           // Per-shard tables

    ShardContext() : shard_index(-1), runtime(nullptr), db(nullptr), transport(nullptr) {}
};

// @unsafe: singleton with mutable state
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
          replay_batch_(0),
          cpu_limit_percent_(0.0),      // 0 = no limit (default)
          throttle_cycle_ms_(100) {}    // 100ms default cycle
      
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

      // CPU throttling configuration
      double cpu_limit_percent_;      // 0.0-100.0, 0 = no limit
      uint32_t throttle_cycle_ms_;    // Duty cycle period in ms

      // Watermark tracking for latency measurements
      std::vector<std::pair<uint32_t, uint32_t>> advanceWatermarkTracker_;

      // Runtime TPCC wiring state (moved from tpcc.cc)
      std::vector<FastTransport*> server_transports_;
      std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_;
      std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_response_;
      std::atomic<int> set_server_transport_{0};

      // Multi-shard support: per-shard contexts
      std::map<int, ShardContext> shard_contexts_;

      // NFS-based multi-shard synchronization
      // Works for both single-process and distributed deployments
      std::string nfs_sync_dir_{"/tmp/mako_sync"};  // Shared directory for sync files
      int nfs_sync_timeout_sec_{60};                 // Timeout for waiting for other shards

  public:
      // Delete copy/move constructors
      BenchmarkConfig(const BenchmarkConfig&) = delete;
      BenchmarkConfig& operator=(const BenchmarkConfig&) = delete;

      // Single point of access
      static BenchmarkConfig& getInstance() {
          static BenchmarkConfig instance;
          return instance;
      }

      // Thread-local shard index for multi-shard mode
      // -1 means not set (use global shardIndex_)
      static inline thread_local int tl_shard_index_ = -1;

      // Getters
      size_t getNthreads() const { return nthreads_; }
      size_t getNshards() const { return nshards_; }
      size_t getNumErpcServer() const { return num_erpc_server_; }
      // In multi-shard mode, returns thread-local shard index if set
      size_t getShardIndex() const {
        if (tl_shard_index_ >= 0) {
          return static_cast<size_t>(tl_shard_index_);
        }
        return shardIndex_;
      }
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
      // @safe
      int getUseHashtable() const { return use_hashtable_; }
      // @safe
      int getIsMicro() const { return is_micro_; }
      // @safe
      int getIsReplicated() const { return is_replicated_; }
      // @unsafe: returns std::string by value
      std::string getPaxosProcName() const { return paxos_proc_name_; }
      // @safe
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
      // Set thread-local shard index for multi-shard mode
      // Call with -1 to clear and revert to global shardIndex_
      static void setThreadLocalShardIndex(int idx) { tl_shard_index_ = idx; }
      static void clearThreadLocalShardIndex() { tl_shard_index_ = -1; }
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

      // CPU throttling getters and setters
      double getCpuLimitPercent() const { return cpu_limit_percent_; }
      void setCpuLimitPercent(double pct) { cpu_limit_percent_ = pct; }
      uint32_t getThrottleCycleMs() const { return throttle_cycle_ms_; }
      void setThrottleCycleMs(uint32_t ms) { throttle_cycle_ms_ = ms; }
      bool isCpuThrottlingEnabled() const {
          return cpu_limit_percent_ > 0.0 && cpu_limit_percent_ < 100.0;
      }

      // Getters and setters for watermark tracking
      std::vector<std::pair<uint32_t, uint32_t>>& getAdvanceWatermarkTracker() { return advanceWatermarkTracker_; }
      const std::vector<std::pair<uint32_t, uint32_t>>& getAdvanceWatermarkTracker() const { return advanceWatermarkTracker_; }

      // Multi-shard context management
      std::map<int, ShardContext>& getShardContexts() { return shard_contexts_; }
      const std::map<int, ShardContext>& getShardContexts() const { return shard_contexts_; }

      ShardContext* getShardContext(int shard_index) {
          auto it = shard_contexts_.find(shard_index);
          return (it != shard_contexts_.end()) ? &it->second : nullptr;
      }

      const ShardContext* getShardContext(int shard_index) const {
          auto it = shard_contexts_.find(shard_index);
          return (it != shard_contexts_.end()) ? &it->second : nullptr;
      }

      void addShardContext(int shard_index, const ShardContext& context) {
          shard_contexts_[shard_index] = context;
      }

      bool hasMultipleShards() const {
          return shard_contexts_.size() > 1;
      }

      // NFS sync getters and setters
      const std::string& getNfsSyncDir() const { return nfs_sync_dir_; }
      void setNfsSyncDir(const std::string& dir) { nfs_sync_dir_ = dir; }
      int getNfsSyncTimeoutSec() const { return nfs_sync_timeout_sec_; }
      void setNfsSyncTimeoutSec(int sec) { nfs_sync_timeout_sec_ = sec; }

      // NFS-based multi-shard barrier methods
      // Works for both single-process and distributed deployments

      // Get the ready file path for a shard
      std::string getShardReadyFilePath(int shard_idx) const {
          return nfs_sync_dir_ + "/shard_" + std::to_string(shard_idx) + "_ready";
      }

      // Initialize NFS sync: create directory and clean up old files
      void initMultiShardBarrier(int /* num_shards - unused, we use nshards_ */) {
          // Create sync directory if it doesn't exist
          std::filesystem::create_directories(nfs_sync_dir_);

          // Clean up any old ready files for ALL shards
          for (size_t i = 0; i < nshards_; i++) {
              std::string ready_file = getShardReadyFilePath(i);
              std::filesystem::remove(ready_file);
          }
      }

      // Signal this shard is ready by creating a ready file
      void signalShardReady(int shard_idx) {
          // Ensure directory exists (needed for multi-process mode where
          // initMultiShardBarrier() might not be called)
          std::filesystem::create_directories(nfs_sync_dir_);

          std::string ready_file = getShardReadyFilePath(shard_idx);
          std::ofstream ofs(ready_file);
          ofs << "ready" << std::endl;
          ofs.close();
      }

      // Wait for all shards to be ready (check for ready files)
      void waitMultiShardBarrier() {
          if (nshards_ <= 1) {
              return;  // No barrier needed for single shard
          }

          // Signal that this shard is ready
          // Uses thread-local shard index if set, otherwise global shardIndex_
          signalShardReady(getShardIndex());

          // Then wait for ALL shards to be ready
          auto start = std::chrono::steady_clock::now();
          while (true) {
              bool all_ready = true;
              for (size_t i = 0; i < nshards_; i++) {
                  std::string ready_file = getShardReadyFilePath(i);
                  if (!std::filesystem::exists(ready_file)) {
                      all_ready = false;
                      break;
                  }
              }

              if (all_ready) {
                  break;
              }

              // Check timeout
              auto now = std::chrono::steady_clock::now();
              auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
              if (elapsed >= nfs_sync_timeout_sec_) {
                  // Timeout - proceed anyway with a warning
                  break;
              }

              // Sleep briefly before checking again
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
      }

      // Clean up ready files after benchmark completes
      void resetMultiShardBarrier() {
          for (size_t i = 0; i < nshards_; i++) {
              std::string ready_file = getShardReadyFilePath(i);
              std::filesystem::remove(ready_file);
          }
      }
};

// (no global runtime accessors)

#endif /* _NDB_BENCHMARK_CONFIG_H_ */
