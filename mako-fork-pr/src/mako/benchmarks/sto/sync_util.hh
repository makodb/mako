#ifndef _SYNC_UTIL_H
#define _SYNC_UTIL_H
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "benchmarks/sto/Interface.hh"

using namespace std;

// Single timestamp system: vectors contain replicated single values for compatibility
namespace sync_util {
    class sync_logger {
    public:
        // latest timestamp for each local Paxos stream
        // Please, we can keep this as vector
        static vector<std::atomic<uint32_t>> local_timestamp_; // timestamp*10+epoch
#ifndef DISABLE_DISK
        // latest timestamp for disk persistence per partition
        static vector<std::atomic<uint32_t>> disk_timestamp_; // timestamp*10+epoch
#endif
        // Single watermark for the entire system
        static std::atomic<uint32_t> single_watermark_; // timestamp*10+epoch
        
        static std::chrono::time_point<std::chrono::high_resolution_clock> last_update;
        static int shardIdx;
        static int nshards;
        static int nthreads; // nthreads == n of Paxos streams
        static bool worker_running;
        static bool is_leader;
        static string cluster;
        static transport::Configuration *config;
        static int local_replica_id; // local server incremental id

        // https://en.cppreference.com/w/cpp/thread/condition_variable
        static bool toLeader;
        static std::mutex m;
        static std::condition_variable cv;
        // term --> shard watermark (not term info)
        static std::unordered_map<int, uint32_t> hist_timestamp; // on the running shard
        static std::atomic<uint32_t> noops_cnt;
        static std::atomic<uint32_t> noops_cnt_hole;
        static int exchange_refresh_cnt;
        static std::atomic<bool> exchange_running;
        static int failed_shard_index;
        static uint32_t failed_shard_ts;
        
        static void Init(int shardIdx_X, int nshards_X, int nthreads_X, bool is_leader_X,
                         string cluster_X,
                         transport::Configuration *config_X) {
            for (int i = 0; i < nthreads_X; i++) {
                local_timestamp_[i].store(0, memory_order_relaxed);
#ifndef DISABLE_DISK
                disk_timestamp_[i].store(0, memory_order_relaxed);
#endif
            }
            // Initialize single watermark
            single_watermark_.store(0, memory_order_relaxed);
            shardIdx = shardIdx_X;
            nshards = nshards_X;
            nthreads = nthreads_X;
            is_leader = is_leader_X;
            cluster = cluster_X;
            config = config_X; 

            exchange_running = true;
            // 2. for the exchange_thread, we attach it to remoteValidate on the leader replica
            if (!is_leader) { // on the follower replica, we start a client and server with busy_loop
                if (nshards <= 1) 
                    return ;

                //start_advancer();
                Warning("the watermark is exchanging within the cluster: %s",cluster.c_str());
                // erpc server - busy loop
                thread server_thread(&sync_logger::server_watermark_exchange);
                server_thread.detach();

                std::this_thread::sleep_for(std::chrono::milliseconds(300));

                // erpc client
                thread client_thread(&sync_logger::client_watermark_exchange);
                client_thread.detach(); 
            }
        }

        static void reset() {
           for (int i = 0; i < nthreads; i++) {
              local_timestamp_[i].store(0, memory_order_relaxed);
#ifndef DISABLE_DISK
              disk_timestamp_[i].store(0, memory_order_relaxed);
#endif
           }
           single_watermark_.store(0, memory_order_relaxed);
        }

        static void shutdown() {
            worker_running = false ;
            toLeader = true; // in order to stop the thread in the follower
            exchange_running = false;
            sync_util::sync_logger::cv.notify_one();
        }

        // Single timestamp safety check
        static bool safety_check(uint32_t timestamp, uint32_t watermark) {
            if (!worker_running) return true;
            return timestamp <= watermark;
        }
        
        // Single timestamp check against global watermark
        static bool safety_check(uint32_t timestamp) {
            if (!worker_running) return true;
            // Check against single watermark
            return timestamp <= single_watermark_.load(memory_order_acquire);
        }
        
        static uint32_t computeLocal() { // compute G immediately and strictly, tt*10+epoch
            uint32_t min_so_far = numeric_limits<uint32_t>::max();

            for (int i=0; i<nthreads; i++) {
                // Take minimum of replication and disk timestamps for each partition
                auto repl_ts = local_timestamp_[i].load(memory_order_acquire);
#ifndef DISABLE_DISK
                auto disk_ts = disk_timestamp_[i].load(memory_order_acquire);
                // Use the minimum of replication and disk timestamp for this partition
                auto partition_min = min(repl_ts, disk_ts);
#else
                // When disk persistence is disabled, only consider replication timestamp
                auto partition_min = repl_ts;
#endif

                if (partition_min >= single_watermark_.load(memory_order_acquire))
                    min_so_far = min(min_so_far, partition_min);
            }
            if (min_so_far!=numeric_limits<uint32_t>::max()) {
#if defined(COCO)
                if ((std::chrono::high_resolution_clock::now() - last_update).count() / 1000.0 / 1000.0 >= COCO_ADVANCING_DURATION) {
#endif
                    single_watermark_.store(min_so_far, memory_order_release);
#if defined(COCO)
                    last_update = std::chrono::high_resolution_clock::now() ;
                }
#endif
            }
            return single_watermark_.load(memory_order_acquire) ;  // invalidate the cache
        }

        // In previous submission, we assume the healthy shards are always INF
        static void update_stable_timestamp(int epoch, uint32_t tt) { 
           hist_timestamp[epoch]=tt; 
        }

        // Single timestamp system: ensure vector contains replicated value
        static void update_stable_timestamp_vec(int epoch, vector<uint32_t> tt_vec) { 
           // In single timestamp system, just use the first element
           if (!tt_vec.empty()) {
               hist_timestamp[epoch] = tt_vec[0];
               std::cout<<"update_stable_timestamp_vec, epoch:"<<epoch<<",tt:"<<hist_timestamp[epoch]<<std::endl;
           }
        }
        
        static uint32_t retrieveW() {
            // Return single watermark
            return single_watermark_.load(memory_order_acquire);
        }

        static void start_advancer() {
            // detached thread for advancing vectorized timestamp
            std::cout<<"start the advancer thread..."<<std::endl;
            for (int i=0; i<nthreads; i++) {
                local_timestamp_[i].store(0, memory_order_release);
#ifndef DISABLE_DISK
                disk_timestamp_[i].store(0, memory_order_release);
#endif
            }
            worker_running = true;
            thread advancer_thread(&sync_logger::advancer);
            advancer_thread.detach();
        }

        static void setShardWBlind(uint32_t w, int sIdx) {
            // In single timestamp system, just update single watermark
            single_watermark_.store(w, memory_order_release);
        }
        
        // Helper for single timestamp system
        static void setSingleWatermark(uint32_t w) {
            single_watermark_.store(w, memory_order_release);
        }

        static uint32_t retrieveShardW() {
           // Return single watermark
           return single_watermark_.load(memory_order_acquire);  // timestamp*10+epoch
        }

        static uint32_t retrieveShardW_relaxed() {
           // Return single watermark
           return single_watermark_.load(memory_order_relaxed);  // timestamp*10+epoch
        }

#ifndef DISABLE_DISK
        // Update disk persistence timestamp for a partition
        static void updateDiskTimestamp(int partition_id, uint32_t timestamp) {
            if (partition_id >= 0 && partition_id < nthreads) {
                disk_timestamp_[partition_id].store(timestamp, memory_order_release);
            }
        }
#endif

        // detached thread to advance local watermark on the current shard
        // there are two ways to advance watermarks: (1) periodically exchange; (2) piggyback in the RPCs in transaction execution
        static void advancer() {
            size_t counter=0;
            while(worker_running) {
                counter++;
                uint32_t min_so_far = numeric_limits<uint32_t>::max();
                for (int i=0;i<nthreads;i++) {
                    // Take minimum of replication and disk timestamps for each partition
                    auto repl_ts = local_timestamp_[i].load(memory_order_acquire);
#ifndef DISABLE_DISK
                    auto disk_ts = disk_timestamp_[i].load(memory_order_acquire);
                    // Use the minimum of replication and disk timestamp for this partition
                    auto partition_min = min(repl_ts, disk_ts);
#else
                    // When disk persistence is disabled, only consider replication timestamp
                    auto partition_min = repl_ts;
#endif

                    uint32_t current_watermark = single_watermark_.load(memory_order_acquire);
                    if (partition_min > 0 && partition_min >= current_watermark)
                        min_so_far = min(min_so_far, partition_min);
                }
                if (min_so_far!=numeric_limits<uint32_t>::max()) {
                    // In single timestamp system, update all shards
                    setSingleWatermark(min_so_far);
                }
                
                std::this_thread::sleep_for(std::chrono::microseconds(1 * 1000));
                /*if (counter % 200 == 0) {
                     Warning("watermark update: %u, shardIdx: %d, watermark: %llu", 
                             min_so_far, shardIdx, 
                             single_watermark_.load(memory_order_acquire));
                    std::string local_w_msg = "local-w: ";
                    for (int i = 0; i < nthreads; i++) {
                        local_w_msg += std::to_string(local_timestamp_[i].load(memory_order_relaxed));
                        if (i < nthreads - 1) local_w_msg += ", ";
                    }
                    Warning("%s", local_w_msg.c_str());
                }*/
            }
            std::cout << "END of advancer" << std::endl;
        }

        // for the server
        static void server_watermark_exchange() {
            std::string local_uri = config->shard(shardIdx, mako::convertCluster(cluster)).host;
            auto id = config->warehouses + 1;
            auto file = config->configFile;
            TThread::set_nshards(nshards);
            TThread::set_shard_index(shardIdx);
            auto transport = new FastTransport(file,
                                               local_uri,
                                               cluster,
                                               12, 13, /* nr_req_types, it would accept thre req from the control:4 */
                                               0,
                                               0,
                                               shardIdx,
                                               id);
            transport->RunNoQueue();
            Warning("server_watermark_exchange is terminated!");
        }

        static int get_exchange_refresh_cnt() { return exchange_refresh_cnt; } 

        static void client_watermark_exchange() {
            uint32_t watermark = 0;
            // erpc ports: 
            //   0-warehouses-1: db worker threads
            //   warehouses: server receiver
            //   warehouses+1: exchange watermark server
            //   warehouses+2: exchange watermark client
            //   warehouses+3: control client
            auto id = config->warehouses + 2;
            TThread::set_nshards(nshards);
            TThread::set_shard_index(shardIdx);
            auto sclient = new mako::ShardClient(config->configFile,
                                                   cluster,
                                                   shardIdx,
                                                   id);
            // send the exchange requests frequently
            int fail_cnt=0;
            while (exchange_running) {
                try{
                    uint32_t dstShardIndex=0;
                    for (int i=0; i<nshards; i++) {
                        if (i==shardIdx) continue;
                        dstShardIndex |= (1 << i);
                    }
                    sclient->remoteExchangeWatermark(watermark, dstShardIndex);
                    // Update single watermark if received value is higher
                    uint32_t currentWatermark = single_watermark_.load(memory_order_acquire);
                    if (watermark > currentWatermark) {
                        setSingleWatermark(watermark);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(1 * 1000));
                    exchange_refresh_cnt++;
                    fail_cnt=0;
                } catch (int n) {
                    // do nothing, continue working on the next exchange
                    Warning("watermark exchange client timeout");
                    fail_cnt++;
                    if (fail_cnt>5) {
                        break;
                    }
                }
            }
            Warning("client_watermark_exchange is terminated!");
        }

        static void client_control2(int control, uint32_t value) {
            auto id = config->warehouses + 4;
            TThread::set_nshards(nshards);
            TThread::set_shard_index(shardIdx);
            // client is created multiple times
            auto static control_sclient = new mako::ShardClient(config->configFile,
                                                   cluster,
                                                   shardIdx,
                                                   id);
            uint32_t dstShardIndex=0;
            for (int i=0; i<nshards; i++) {
                if (i==shardIdx) continue;
                dstShardIndex |= (1 << i);
            }
            uint32_t shard_tt = retrieveShardW()/10;
            value = shard_tt*10+value;
            uint32_t ret_value = 0;
            Warning("client for the control is starting! control:%d, value:%zu", control, value);
            try{
                control_sclient->remoteControl(control, value, ret_value, dstShardIndex);
            } catch(int n) {
                Panic("remoteControl throw an error");
            }
            Warning("client for the control is terminated! control:%d, value:%lld", control, value);
        }

        // for the control:4, send the req to the exchange-watermark server,
        //   for the rest, send the req to the erpc server.
        static void client_control(int control, uint32_t value) {
            auto id = config->warehouses + 3;
            TThread::set_nshards(nshards);
            TThread::set_shard_index(shardIdx);
            auto static control_sclient = new mako::ShardClient(config->configFile,
                                                   cluster,
                                                   shardIdx,
                                                   id);
            uint32_t dstShardIndex=0;
            
            bool is_datacenter_failure = control >= 4;

            for (int i=0; i<nshards; i++) {
                if (i==shardIdx && !is_datacenter_failure) continue;
                dstShardIndex |= (1 << i);
            }

            uint32_t shard_tt = retrieveShardW()/10;
            value = shard_tt*10+value;
            uint32_t ret_value = 0;
            Warning("client for the control is starting! control:%d, value:%lld", control, value);
            try{
                control_sclient->remoteControl(control, value, ret_value, dstShardIndex);
            } catch(int n) {
                Panic("remoteControl throw an error");
            }
            Warning("client for the control is terminated! control:%d, value:%lld", control, value);
        }

    }; // end of class definition
}

#endif