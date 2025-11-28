#include <benchmark/benchmark.h>
#include "mako/benchmarks/sto/TRcu.hh"
#include <vector>
#include <random>

// TRcu cleanup benchmark
static void BM_TRcuCleanUntil_Single(benchmark::State& state) {
    TRcuSet rcu_set;
    int epoch = 0;
    
    for (auto _ : state) {
        rcu_set.clean_until(epoch++);
        benchmark::DoNotOptimize(epoch);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TRcuCleanUntil_Single);

// Batch cleanup benchmark
static void BM_TRcuBatchCleanup(benchmark::State& state) {
    const int batch_size = state.range(0);
    
    for (auto _ : state) {
        TRcuSet rcu_set;
        for (int i = 0; i < batch_size; i++) {
            rcu_set.clean_until(i);
        }
    }
    
    state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_TRcuBatchCleanup)->Range(10, 1000);

// Concurrent cleanup throughput
static void BM_TRcuConcurrentCleanup(benchmark::State& state) {
    const int num_threads = state.range(0);
    
    for (auto _ : state) {
        TRcuSet rcu_set;
        std::vector<std::thread> threads;
        
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back([&rcu_set, i]() {
                for (int j = 0; j < 100; j++) {
                    rcu_set.clean_until(i * 100 + j);
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
    }
    
    state.SetItemsProcessed(state.iterations() * num_threads * 100);
}
BENCHMARK(BM_TRcuConcurrentCleanup)->Range(1, 8);

BENCHMARK_MAIN();
