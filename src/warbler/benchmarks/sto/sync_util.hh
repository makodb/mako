#ifndef _SYNC_UTIL_H
#define _SYNC_UTIL_H
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "benchmarks/sto/Interface.hh"

using namespace std;

namespace sync_util {
    class sync_logger {
    public:
        // initialize it in the benchmarks/common3.h
        // latest timestamp for each local Paxos stream
        static vector<std::atomic<uint32_t>> local_timestamp_;
        // latest watermark for each remote shard
        static vector<std::atomic<uint32_t>> vectorized_w_;
        
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
        // term --> vectorized watermark (+ term info)
        static std::unordered_map<int, vector<uint32_t>> hist_timestamp_vec; // on the running shard; In this implementation, we discard INF
        static std::atomic<uint32_t> noops_cnt;
        static std::atomic<uint32_t> noops_cnt_hole;
        static int exchange_refresh_cnt;
        static std::atomic<bool> exchange_running;
        static int failed_shard_index;
        static uint32_t failed_shard_ts;
        
        static void Init(int shardIdx_X, int nshards_X, int nthreads_X, bool is_leader_X,
                         string cluster_X,
                         transport::Configuration *config_X) {
            for (int i = 0; i < nthreads; i++) {
                local_timestamp_[i].store(0, memory_order_relaxed);
            }
            for (int i = 0; i < nshards; i++) {
                vectorized_w_[i].store(0, memory_order_relaxed);
            }
            shardIdx = shardIdx_X;
            nshards = nshards_X;
            nthreads = nthreads_X;
            is_leader = is_leader_X;
            cluster = cluster_X;
            config = config_X; 

            exchange_running = true;
            // 2. for the exchange_thread, we attach it to remoteValidate on the leader replica
            if (!is_leader) { // on the follower replica, we start a client and server with busy_loop
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
           }
           for (int i = 0; i < nshards; i++) {
               vectorized_w_[i].store(0, memory_order_relaxed);
           } 
        }

        static void shutdown() {
            worker_running = false ;
            toLeader = true; // in order to stop the thread in the follower
            exchange_running = false;
            sync_util::sync_logger::cv.notify_one();
        }

        // b -> vectorized watermark
        static bool safety_check(std::vector<uint32_t> a, std::vector<uint32_t> b) {
            //if (worker_running)
            //    printf("a:%d,%d,%d;b:%d,%d,%d;pid:%d\n",a[0],a[1],a[2],b[0],b[1],b[2],TThread::getPartitionID());
            return true;
            if (!worker_running) return true;
            for (int i=0;i<nshards;i++)
                if (a[i]>b[i]) return false;
            return true;
        }

        static bool safety_check(uint32_t *a, std::vector<uint32_t> b) {
            //if (worker_running)
            //    printf("a:%d,%d,%d;b:%d,%d,%d;pid:%d\n",a[0],a[1],a[2],b[0],b[1],b[2],TThread::getPartitionID());
            return true;
            if (!worker_running) return true;
            for (int i=0;i<nshards;i++)
                if (a[i]>b[i]) return false;
            return true;
        }

        static bool safety_check(uint32_t *a) {
            //if (worker_running)
            //    printf("a:%d,%d,%d;b:%d,%d,%d;pid:%d\n",a[0],a[1],a[2],b[0],b[1],b[2],TThread::getPartitionID());
            return true;
            if (!worker_running) return true;
            for (int i=0;i<nshards;i++)
                if (a[i]>vectorized_w_[i]) return false;
            return true;
        }

        static uint32_t computeLocal() { // compute G immediately and strictly, tt*10+epoch
            uint32_t min_so_far = numeric_limits<uint32_t>::max();

            for (int i=0; i<nthreads; i++) {
                auto c = local_timestamp_[i].load(memory_order_acquire) ;
                if (c >= vectorized_w_[shardIdx])
                    min_so_far = min(min_so_far, c);
            }
            if (min_so_far!=numeric_limits<uint32_t>::max()) {
#if defined(COCO)
                if ((std::chrono::high_resolution_clock::now() - last_update).count() / 1000.0 / 1000.0 >= COCO_ADVANCING_DURATION) {
#endif
                    vectorized_w_[shardIdx].store(min_so_far, memory_order_release);
#if defined(COCO)
                    last_update = std::chrono::high_resolution_clock::now() ;
                }
#endif
            }
            return vectorized_w_[shardIdx].load(memory_order_acquire) ;  // invalidate the cache
        }

        // In previous submission, we assume the healthy shards are always INF
        static void update_stable_timestamp(int epoch, uint32_t tt) { 
           hist_timestamp[epoch]=tt; 
        }

        // FVW in the old epoch
        static void update_stable_timestamp_vec(int epoch, vector<uint32_t> tt_vec) { 
           hist_timestamp_vec[epoch].swap(tt_vec);
           std::cout<<"update_stable_timestamp_vec, epoch:"<<epoch<<",tt:"<<hist_timestamp_vec[epoch][0]<<std::endl;
        }
        
        static vector<uint32_t> retrieveW() {
            static std::vector<uint32_t> vectorized_w_local_(nshards);
            for (int i=0;i<nshards;i++)
                vectorized_w_local_[i] = vectorized_w_[i].load(memory_order_acquire);
            return vectorized_w_local_;
        }

        static void start_advancer() {
            // detached thread for advancing vectorized timestamp
            std::cout<<"start the advancer thread..."<<std::endl;
            for (int i=0; i<nthreads; i++) {
                local_timestamp_[i].store(0, memory_order_release);
            } 
            worker_running = true;
            thread advancer_thread(&sync_logger::advancer);
            advancer_thread.detach();
        }

        static void setShardWBlind(uint32_t w, int sIdx) {
            vectorized_w_[sIdx].store(w, memory_order_release);
        }

        static uint32_t retrieveShardW() {
           return vectorized_w_[shardIdx].load(memory_order_acquire);  // timestamp*10+epoch
        }

        // detached thread to advance local watermark on the current shard
        // there are two ways to advance watermarks: (1) periodically exchange; (2) piggyback in the RPCs in transaction execution
        static void advancer() {
            size_t counter=0;
            while(worker_running) {
                counter++;
                uint32_t min_so_far = numeric_limits<uint32_t>::max();
                for (int i=0;i<nthreads;i++) {
                    auto c = local_timestamp_[i].load(memory_order_acquire) ;
                    if (c>0 && c >= vectorized_w_[shardIdx])
                        min_so_far = min(min_so_far, local_timestamp_[i].load(memory_order_acquire));
                }
                if (min_so_far!=numeric_limits<uint32_t>::max())
                    vectorized_w_[shardIdx].store(min_so_far, memory_order_release);
                
                std::this_thread::sleep_for(std::chrono::microseconds(1 * 1000));
                // if (counter % 200 == 0) {
                //      Warning("watermark update: %llu, shardIdx: %d, w0: %llu, w1: %llu, w2: %llu", 
                //              min_so_far, shardIdx, 
                //              vectorized_w_[0].load(memory_order_acquire),
                //              vectorized_w_[1].load(memory_order_acquire),
                //              vectorized_w_[2].load(memory_order_acquire));
                //     Warning("local-w: %llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu",
                //             local_timestamp_[0].load(),local_timestamp_[1].load(),local_timestamp_[2].load(),local_timestamp_[3].load(),
                //             local_timestamp_[4].load(),local_timestamp_[5].load(),local_timestamp_[6].load(),local_timestamp_[7].load());
                // }
            }
        }

        // for the server
        static void server_watermark_exchange() {
            std::string local_uri = config->shard(shardIdx, srolis::convertCluster(cluster)).host;
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
            std::vector<uint32_t> vectorWatermark(nshards);
            // erpc ports: 
            //   0-warehouses-1: db worker threads
            //   warehouses: server receiver
            //   warehouses+1: exchange watermark server
            //   warehouses+2: exchange watermark client
            //   warehouses+3: control client
            auto id = config->warehouses + 2;
            TThread::set_nshards(nshards);
            TThread::set_shard_index(shardIdx);
            auto sclient = new srolis::ShardClient(config->configFile,
                                                   cluster,
                                                   shardIdx,
                                                   id, /* par_id */
                                                   1 /*tpc-c*/);
            // send the exchange requests frequently
            int fail_cnt=0;
            while (exchange_running) {
                try{
                    uint32_t dstShardIndex=0;
                    for (int i=0; i<nshards; i++) {
                        if (i==shardIdx) continue;
                        dstShardIndex |= (1 << i);
                    }
                    sclient->remoteExchangeWatermark(vectorWatermark, dstShardIndex);
                    for (int i=0; i<nshards; i++) {
                        if (i == shardIdx) continue;
                        if (vectorized_w_[i].load()<vectorWatermark[i])
                            vectorized_w_[i].store(vectorWatermark[i], memory_order_release);
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
            auto static control_sclient = new srolis::ShardClient(config->configFile,
                                                   cluster,
                                                   shardIdx,
                                                   id, /* par_id */
                                                   1 /*tpc-c*/);
            uint32_t dstShardIndex=0;
            for (int i=0; i<nshards; i++) {
                if (i==shardIdx) continue;
                dstShardIndex |= (1 << i);
            }
            uint32_t shard_tt = retrieveShardW()/10;
            value = shard_tt*10+value;
            std::vector<uint32_t> ret_values(nshards);
            Warning("client for the control is starting! control:%d, value:%zu", control, value);
            try{
                control_sclient->remoteControl(control, value, ret_values, dstShardIndex);
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
            auto static control_sclient = new srolis::ShardClient(config->configFile,
                                                   cluster,
                                                   shardIdx,
                                                   id, /* par_id */
                                                   1 /*tpc-c*/);
            uint32_t dstShardIndex=0;
            
            bool is_datacenter_failure = control >= 4;

            for (int i=0; i<nshards; i++) {
                if (i==shardIdx && !is_datacenter_failure) continue;
                dstShardIndex |= (1 << i);
            }

            uint32_t shard_tt = retrieveShardW()/10;
            value = shard_tt*10+value;
            std::vector<uint32_t> ret_values(nshards);
            Warning("client for the control is starting! control:%d, value:%lld", control, value);
            try{
                control_sclient->remoteControl(control, value, ret_values, dstShardIndex);
            } catch(int n) {
                Panic("remoteControl throw an error");
            }
            Warning("client for the control is terminated! control:%d, value:%lld", control, value);
        }

    }; // end of class definition
}

#endif