#include <gtest/gtest.h>
#include <benchmark/benchmark.h>
#include "mako/varint.h"
#include <random>
#include <vector>

// Performance test fixture
class VarintPerf : public ::testing::Test {
protected:
    std::vector<uint32_t> random_values;
    std::vector<uint8_t> encoded_buffer;
    
    void SetUp() override {
        // Generate random test data
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
        
        random_values.resize(10000);
        for (auto& val : random_values) {
            val = dis(gen);
        }
        
        // Pre-encode for read tests
        encoded_buffer.resize(50000);  // Enough space
        uint8_t* ptr = encoded_buffer.data();
        for (uint32_t val : random_values) {
            ptr = write_uvint32(ptr, val);
        }
    }
};

// Google Benchmark tests
static void BM_WriteUvint32_Small(benchmark::State& state) {
    uint8_t buffer[10];
    uint32_t value = 42;  // Single byte value
    
    for (auto _ : state) {
        write_uvint32(buffer, value);
        benchmark::DoNotOptimize(buffer);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteUvint32_Small);

static void BM_WriteUvint32_Medium(benchmark::State& state) {
    uint8_t buffer[10];
    uint32_t value = 10000;  // Multi-byte value
    
    for (auto _ : state) {
        write_uvint32(buffer, value);
        benchmark::DoNotOptimize(buffer);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteUvint32_Medium);

static void BM_WriteUvint32_Large(benchmark::State& state) {
    uint8_t buffer[10];
    uint32_t value = 0xFFFFFFFF;  // Max value
    
    for (auto _ : state) {
        write_uvint32(buffer, value);
        benchmark::DoNotOptimize(buffer);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteUvint32_Large);

static void BM_ReadUvint32_Small(benchmark::State& state) {
    uint8_t buffer[10];
    write_uvint32(buffer, 42);
    uint32_t value;
    
    for (auto _ : state) {
        read_uvint32(buffer, &value);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReadUvint32_Small);

static void BM_ReadUvint32_Large(benchmark::State& state) {
    uint8_t buffer[10];
    write_uvint32(buffer, 0xFFFFFFFF);
    uint32_t value;
    
    for (auto _ : state) {
        read_uvint32(buffer, &value);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReadUvint32_Large);

static void BM_RoundTrip(benchmark::State& state) {
    uint8_t buffer[10];
    uint32_t original = 123456;
    uint32_t decoded;
    
    for (auto _ : state) {
        uint8_t* end = write_uvint32(buffer, original);
        read_uvint32(buffer, &decoded);
        benchmark::DoNotOptimize(end);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RoundTrip);

static void BM_SizeUvint32(benchmark::State& state) {
    uint32_t value = 123456;
    size_t size;
    
    for (auto _ : state) {
        size = size_uvint32(value);
        benchmark::DoNotOptimize(size);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SizeUvint32);

static void BM_SkipUvint32(benchmark::State& state) {
    uint8_t buffer[10];
    write_uvint32(buffer, 123456);
    size_t skipped;
    
    for (auto _ : state) {
        skipped = skip_uvint32(buffer, nullptr);
        benchmark::DoNotOptimize(skipped);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SkipUvint32);

// Throughput test - encode many values
static void BM_EncodeThroughput(benchmark::State& state) {
    std::vector<uint32_t> values(1000);
    std::vector<uint8_t> buffer(5000);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    
    for (auto& val : values) {
        val = dis(gen);
    }
    
    for (auto _ : state) {
        uint8_t* ptr = buffer.data();
        for (uint32_t val : values) {
            ptr = write_uvint32(ptr, val);
        }
        benchmark::DoNotOptimize(ptr);
    }
    
    state.SetItemsProcessed(state.iterations() * values.size());
    state.SetBytesProcessed(state.iterations() * values.size() * sizeof(uint32_t));
}
BENCHMARK(BM_EncodeThroughput);

// Throughput test - decode many values
static void BM_DecodeThroughput(benchmark::State& state) {
    std::vector<uint32_t> values(1000);
    std::vector<uint8_t> buffer(5000);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    
    // Pre-encode
    uint8_t* ptr = buffer.data();
    for (size_t i = 0; i < values.size(); i++) {
        ptr = write_uvint32(ptr, dis(gen));
    }
    
    for (auto _ : state) {
        const uint8_t* read_ptr = buffer.data();
        for (size_t i = 0; i < values.size(); i++) {
            uint32_t val;
            read_ptr = read_uvint32(read_ptr, &val);
        }
        benchmark::DoNotOptimize(read_ptr);
    }
    
    state.SetItemsProcessed(state.iterations() * values.size());
    state.SetBytesProcessed(state.iterations() * values.size() * sizeof(uint32_t));
}
BENCHMARK(BM_DecodeThroughput);

// Google Test performance tests
TEST_F(VarintPerf, Write10000Values) {
    uint8_t buffer[50000];
    
    auto start = std::chrono::high_resolution_clock::now();
    
    uint8_t* ptr = buffer;
    for (uint32_t value : random_values) {
        ptr = write_uvint32(ptr, value);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Wrote 10000 values in " << duration.count() << " μs" << std::endl;
    std::cout << "Throughput: " << (10000.0 / duration.count()) << " M values/sec" << std::endl;
    
    // Should be fast enough
    EXPECT_LT(duration.count(), 10000);  // Less than 10ms for 10k values
}

TEST_F(VarintPerf, Read10000Values) {
    auto start = std::chrono::high_resolution_clock::now();
    
    const uint8_t* ptr = encoded_buffer.data();
    for (size_t i = 0; i < random_values.size(); i++) {
        uint32_t value;
        ptr = read_uvint32(ptr, &value);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Read 10000 values in " << duration.count() << " μs" << std::endl;
    std::cout << "Throughput: " << (10000.0 / duration.count()) << " M values/sec" << std::endl;
    
    EXPECT_LT(duration.count(), 10000);
}

TEST_F(VarintPerf, CompressionRatio) {
    size_t encoded_size = 0;
    uint8_t temp_buffer[10];
    
    for (uint32_t value : random_values) {
        uint8_t* end = write_uvint32(temp_buffer, value);
        encoded_size += (end - temp_buffer);
    }
    
    size_t uncompressed_size = random_values.size() * sizeof(uint32_t);
    double ratio = static_cast<double>(encoded_size) / uncompressed_size;
    
    std::cout << "Compression ratio: " << ratio << std::endl;
    std::cout << "Space savings: " << ((1.0 - ratio) * 100) << "%" << std::endl;
    
    // Varint should generally save space for random data
    EXPECT_LT(ratio, 1.5);  // Should not expand too much
}

// Run benchmarks
BENCHMARK_MAIN();
