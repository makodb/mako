// Native worker/RPC attachment shared by Rust-backed applications.
#include "runtime/benchmark_thread.hh"
#include "runtime/worker_context.hh"
#include "runtime/epoch.hh"
#include "runtime/thread_registration.hh"
#include "benchmarks/benchmark_config.h"
#include "lib/configuration.h"
#include "lib/shardClient.h"
#include "benchmarks/common.h"
#include <atomic>
#include <stdexcept>
#include <unistd.h>

namespace mako {
namespace { thread_local bool tls_benchmark_thread_attached_ = false; }
// @unsafe - Binds native worker IDs and owns the per-thread shard client.
void benchmark_thread_init(bool loader)
  {
    if (tls_benchmark_thread_attached_) {
      throw std::logic_error("database thread was initialized twice");
    }
    if (TThread::sclient != nullptr) {
      throw std::logic_error(
          "database thread initialization found a live shard client");
    }
    tls_benchmark_thread_attached_ = true;
    struct attachment_reservation {
      bool committed = false;
      ~attachment_reservation() {
        if (!committed)
          tls_benchmark_thread_attached_ = false;
      }
    } reservation;

    // Per-SHARD worker sequence. A single process can run several
    // shards (dbtest -L 0,1): pid = seq % warehouses is only correct
    // if each shard's workers draw a contiguous block, but concurrent
    // shard-runners interleave on a shared counter — two same-shard
    // workers could get the same pid, derive identical client ports,
    // and EADDRINUSE-panic (shard2SingleProcess CI flake).
    static constexpr size_t kMaxLocalShards = 64;
    static constexpr size_t kMaxProtocolShards = 31;
    static std::atomic<size_t> partition_seq[kMaxLocalShards];
    auto& benchmark_config = BenchmarkConfig::getInstance();
    transport::Configuration* const transport_config =
        benchmark_config.getConfig();
    const size_t shard_index = benchmark_config.getShardIndex();
    const size_t shard_count = benchmark_config.getNshards();
    // Shard participation still flows through unsigned-int masks and signed
    // `1 << shard` expressions in ShardClient. Until those are widened, only
    // indices 0..30 are representable without overflow or signed-shift UB.
    ALWAYS_ASSERT(shard_count > 0 && shard_count <= kMaxProtocolShards);
    ALWAYS_ASSERT(shard_index < shard_count &&
                  shard_index < kMaxLocalShards);

    size_t warehouse_count = benchmark_config.getNthreads();
    if (transport_config != nullptr) {
      // Configuration::warehouses is signed. Check it before conversion so a
      // negative YAML value cannot become a huge size_t and corrupt pid/port
      // assignment.
      ALWAYS_ASSERT(transport_config->warehouses > 0);
      ALWAYS_ASSERT(transport_config->nshards > 0 &&
                    transport_config->nshards <=
                        static_cast<int>(kMaxProtocolShards));
      ALWAYS_ASSERT(shard_index <
                    static_cast<size_t>(transport_config->nshards));
      warehouse_count =
          static_cast<size_t>(transport_config->warehouses);
    }
    ALWAYS_ASSERT(warehouse_count > 0);

    // A detached OS thread keeps ownership of its RCU slot. Reusing that slot
    // on a later attachment avoids exhausting MAX_THREADS while preserving
    // the no-cross-thread-recycling safety boundary. The shared registration
    // also rejects a worker that already serves the Rust-facing local ABI.
    if (!mako::silo::claim_thread_runtime(
            mako::silo::thread_runtime::native_mako)) {
      Panic("OS thread cannot switch from plain Masstree to Mako");
    }
    const int thread_id = mako::silo::try_allocate_thread_id();
    if (thread_id < 0) {
      Panic("Native worker thread-ID space exhausted (%d process-lifetime slots)",
            MAX_THREADS);
    }
    TThread::set_id(thread_id);
    TThread::set_mode(0); // checking in-progress
    TThread::in_loading_phase = loader;
    TThread::set_num_rpc_server(benchmark_config.getNumRpcServer());
    TThread::set_is_micro(benchmark_config.getIsMicro());
#if defined(DISABLE_MULTI_VERSION)
    TThread::disable_multiversion();
#else
    if (benchmark_config.getIsReplicated()) {
      TThread::enable_multiverison();
    }else{
      TThread::disable_multiversion();
    }
#endif
    TThread::set_shard_index(benchmark_config.getShardIndex());
    TThread::set_nshards(benchmark_config.getNshards());
    TThread::set_warehouses(warehouse_count);
    Notice("thread_init: thread_id=%d, shard_index=%d, getShardIndex=%zu, loader=%d",
           TThread::id(), TThread::get_shard_index(),
           benchmark_config.getShardIndex(), loader);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    TThread::the_debug_bit = 0;
    TThread::is_worker_leader = benchmark_config.getLeaderConfig();

    TThread::increment_id = 0;
    TThread::skipBeforeRemoteNewOrder = 0;
    TThread::isHomeWarehouse = true;
    TThread::isRemoteShard = false;
    TThread::skipBeforeRemotePayment = 0;
    if(!loader) {
      const size_t shard_slot = shard_index;
      static thread_local size_t assigned_local_pid[kMaxLocalShards] = {};
      static thread_local size_t assigned_warehouse_count[kMaxLocalShards] = {};
      if (assigned_warehouse_count[shard_slot] == 0) {
        size_t old = partition_seq[shard_slot].fetch_add(1);
        if (old >= warehouse_count) {
          throw std::runtime_error(
              "native STO worker topology exceeded configured warehouses");
        }
        assigned_local_pid[shard_slot] = old;
        assigned_warehouse_count[shard_slot] = warehouse_count;
      } else {
        // A process-wide shard topology is immutable once a thread attaches.
        // Changing it would also invalidate ShardClient port ownership.
        ALWAYS_ASSERT(assigned_warehouse_count[shard_slot] == warehouse_count);
      }
      // Preserve the local partition and ShardClient port across detach and
      // reattach on the same OS thread.
      size_t local_pid = assigned_local_pid[shard_slot];
      TThread::set_pid(local_pid);

      if (transport_config == nullptr) {
        // The RocksDB-compatible local facade permits a standalone,
        // non-replicated one-shard database without a transport YAML file.
        // Such a transaction can never acquire remote shard bits and needs no
        // ShardClient, but it still needs native worker context attachment.
        ALWAYS_ASSERT(benchmark_config.getNshards() == 1);
        ALWAYS_ASSERT(!benchmark_config.getIsReplicated());
      } else {
        try {
          TThread::sclient = new mako::ShardClient(
              transport_config->configFile,
              benchmark_config.getCluster(),
              benchmark_config.getShardIndex(),
              local_pid);

          // Verify remote shards are ready before proceeding (Option 4A)
          // This ensures distributed deployment safety: all shards must be listening
          // before any worker starts executing transactions
          int myShardIndex = benchmark_config.getShardIndex();
          int nshards = benchmark_config.getNshards();
          for (int i = 0; i < nshards; i++) {
            if (i == myShardIndex) continue;  // Skip self
            int retries = 0;
            const int maxRetries = 30;  // 30 seconds max wait
            while (TThread::sclient->checkRemoteShardReady(i) != mako::ErrorCode::SUCCESS) {
              retries++;
              if (retries >= maxRetries) {
                Warning("Shard %d not ready after %d retries, proceeding anyway", i, maxRetries);
                break;
              }
              usleep(1000000);  // 1 second retry interval
            }
            if (retries < maxRetries && retries > 0) {
              Notice("Shard %d ready after %d retries", i, retries);
            }
          }
        } catch (...) {
          delete TThread::sclient;
          TThread::sclient = nullptr;
          throw;
        }
      }
      //Notice("ParID[worker-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getGlobalPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    } else {
      TThread::set_pid(TThread::id() % warehouse_count);
      //Notice("ParID[load-id] pid:%d,id:%d,config:%s,loader:%d, ismultiversion:%d,helper_thread?:%d",TThread::getGlobalPartitionID(),TThread::id(),BenchmarkConfig::getInstance().getConfig()->configFile.c_str(),loader,TThread::is_multiversion(),source==1);
    }
    reservation.committed = true;
  }

void benchmark_thread_end()
  {
    if (!tls_benchmark_thread_attached_) {
      throw std::logic_error("database thread was not initialized");
    }
    // A loader, worker, or helper that exits with its last transaction epoch
    // published would pin active_epoch forever. It has no further STO reads,
    // so remove its slot from the process-wide minimum before returning.
    EpochRuntime::rcu_quiesce();
    if (TThread::sclient != nullptr) {
      mako::ShardClient *client = TThread::sclient;
      TThread::sclient = nullptr;
      delete client;
    }
    tls_benchmark_thread_attached_ = false;
  }


} // namespace mako
