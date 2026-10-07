//
// makoCon.cc - Redis-compatible server using Mako database (mako::DB interface)
//
// This file implements a simple Redis-compatible key-value server using:
// - mako::DB interface for database operations
// - Rust library for Redis protocol handling (shared listener, thread-per-core)
// - Nonblocking connection I/O with synchronous Mako transactions
// - Transaction support via MULTI/EXEC
//
// Plain GET and unconditional SET use a specialized transaction path. Other
// commands and MULTI/EXEC use execute_transaction().
//

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <shared_mutex>
#include <mako.hh>
#include "rocks_interface/db.hh"
#include <examples/common.h>
#include "transaction_ffi.h"
#include "silo_runtime.h"
#include "makocon_ffi_impl.hh"

import std;

// Global database instance (used by FFI callbacks)
static mako::DB* g_mako_db = nullptr;
static mbta_sharded_ordered_index* g_table = nullptr;
static const auto g_mako_start_time = std::chrono::steady_clock::now();
// @unsafe { std::atomic is used for C/Rust FFI INFO counters across worker threads. }
static std::atomic<uint64_t> g_mako_txn_commits{0};
static std::atomic<uint64_t> g_mako_txn_aborts{0};
// Retry attempts are recorded by the Rust Redis retry loop.
static std::atomic<uint64_t> g_mako_txn_retries{0};
static constexpr size_t kRedisMaxWorkers = 32;
struct alignas(64) RedisFastMetricShard {
    std::atomic<uint64_t> commits{0};
    std::atomic<uint64_t> aborts{0};
};
// The specialized executor updates only its worker's cache line. Generic and
// non-worker calls retain the global fallback counters above.
static std::array<RedisFastMetricShard, kRedisMaxWorkers> g_redis_fast_metrics{};

static std::atomic<uint64_t> g_mako_random_counter{1};
// Redis-facing correctness is claimed through makoCon. Conflicting write
// transactions are serialized by Redis key stripe instead of using one global
// executor lock, so unrelated keys can still make progress in parallel.
static std::shared_mutex g_redis_keyspace_mutex;
// Keep collisions rare at the supported 32-worker maximum. With 256 stripes,
// unrelated uniformly distributed keys frequently contend; 16K stripes cost
// well under 1 MiB on the target libc++ while preserving same-key ordering.
static constexpr size_t kRedisTxnLockStripes = 16 * 1024;
static std::array<std::mutex, kRedisTxnLockStripes> g_redis_txn_key_mutexes;
static bool g_redis_single_worker_mode = false;
static bool g_redis_replication_enabled = false;
static bool g_redis_bench_skip_ttl = false;
static bool g_redis_bench_no_db = false;
static bool g_redis_bench_skip_locks = false;
static bool g_redis_bench_skip_mutex = false;
static size_t g_redis_bench_value_size = 8;
static size_t g_redis_cache_capacity_bytes = 0;

// Optional read-through cache for non-expiring Redis strings. Cache state is
// protected by the same key stripe that orders Redis writes, so hits do not pay
// for a second mutex. Fast plain SET refreshes in place after commit; generic
// writes invalidate after commit and the next GET repopulates.
// The map is indexed by the already-computed 64-bit stripe hash and retains the
// full key to turn a hash collision into a safe miss rather than a wrong value.
static constexpr size_t kRedisCacheShards = kRedisTxnLockStripes;
struct RedisCacheEntry {
    std::string key;
    std::string value;
    size_t charge = 0;
    uint64_t generation = 0;
};
struct RedisCacheShard {
    std::unordered_map<uint64_t, RedisCacheEntry> entries;
    std::deque<std::pair<uint64_t, uint64_t>> fifo;
    size_t bytes = 0;
    uint64_t next_generation = 1;
};
static std::array<RedisCacheShard, kRedisCacheShards> g_redis_cache;
static std::atomic<uint64_t> g_redis_cache_hits{0};
static std::atomic<uint64_t> g_redis_cache_misses{0};
static std::atomic<uint64_t> g_redis_cache_inserts{0};
static std::atomic<uint64_t> g_redis_cache_evictions{0};
static std::atomic<uint64_t> g_redis_cache_invalidations{0};

// Thread-local state for transaction handling
thread_local str_arena* tl_arena = nullptr;
thread_local std::string tl_txn_buf;
thread_local std::string tl_key_buf;
thread_local std::string tl_ttl_key_buf;
thread_local std::string tl_ttl_val_buf;
thread_local std::string tl_val_buf;
thread_local std::string tl_exists_buf;
thread_local std::string tl_delete_buf;
thread_local std::string tl_encoded_val_buf;
thread_local std::string tl_bench_val_buf;
thread_local bool tl_initialized = false;
thread_local size_t tl_redis_worker_id = kRedisMaxWorkers;

static mako::Status redis_table_get(void* txn, const std::string& key,
                                    std::string& value) {
    return tx_get(g_table, txn, key, value)
        ? mako::Status::OK()
        : mako::Status::NotFound();
}

static mako::Status redis_table_put(void* txn, const std::string& key,
                                    const std::string& encoded_value) {
    tx_put(g_table, txn, key, encoded_value);
    return mako::Status::OK();
}

static mako::Status redis_table_delete(void* txn, const std::string& key) {
    tl_delete_buf.clear();
    if (!tx_get(g_table, txn, key, tl_delete_buf)) {
        return mako::Status::NotFound();
    }
    tx_remove(g_table, txn, key);
    return mako::Status::OK();
}

static mako::Status redis_table_exists(void* txn, const std::string& key,
                                       bool* exists) {
    tl_exists_buf.clear();
    *exists = tx_get(g_table, txn, key, tl_exists_buf);
    return mako::Status::OK();
}

// @safe - Parse a positive integer from an environment variable.
static bool parse_env_int(const char* name, int default_value, int min_value, int max_value, int& out) {
    const char* raw = getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        out = default_value;
        return true;
    }
    try {
        size_t pos = 0;
        int parsed = std::stoi(raw, &pos);
        if (pos != std::strlen(raw) || parsed < min_value || parsed > max_value) {
            std::cerr << name << " must be between " << min_value << " and " << max_value << std::endl;
            return false;
        }
        out = parsed;
        return true;
    } catch (...) {
        std::cerr << "Invalid " << name << " value: " << raw << std::endl;
        return false;
    }
}

// @safe - Parse comma-separated shard indices for local multi-shard smoke fixtures.
static bool parse_local_shards(const char* raw, int nshards, std::vector<int>& local_shards) {
    local_shards.clear();
    if (raw == nullptr || raw[0] == '\0') {
        return true;
    }
    std::string input(raw);
    size_t start = 0;
    while (start <= input.size()) {
        size_t comma = input.find(',', start);
        std::string token = input.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (token.empty()) {
            std::cerr << "MAKO_LOCAL_SHARDS contains an empty shard id" << std::endl;
            return false;
        }
        try {
            size_t pos = 0;
            int shard = std::stoi(token, &pos);
            if (pos != token.size() || shard < 0 || shard >= nshards) {
                std::cerr << "Invalid MAKO_LOCAL_SHARDS shard id: " << token << std::endl;
                return false;
            }
            local_shards.push_back(shard);
        } catch (...) {
            std::cerr << "Invalid MAKO_LOCAL_SHARDS shard id: " << token << std::endl;
            return false;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    std::sort(local_shards.begin(), local_shards.end());
    local_shards.erase(std::unique(local_shards.begin(), local_shards.end()), local_shards.end());
    return true;
}

// @safe - Parse a boolean environment variable using common true values.
static bool env_enabled(const char* name) {
    const char* raw = getenv(name);
    if (raw == nullptr) {
        return false;
    }
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static bool redis_can_write_here() {
    return !g_redis_replication_enabled
        || is_replication_leader(static_cast<uint32_t>(TThread::getLocalPartitionID()));
}

static void wait_for_redis_replication() {
    if (g_redis_replication_enabled) {
        wait_for_submit(static_cast<uint32_t>(TThread::getLocalPartitionID()));
    }
}

static void record_fast_commit() {
    if (tl_redis_worker_id < g_redis_fast_metrics.size()) {
        g_redis_fast_metrics[tl_redis_worker_id].commits.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_mako_txn_commits.fetch_add(1, std::memory_order_relaxed);
    }
}

static void record_fast_abort() {
    if (tl_redis_worker_id < g_redis_fast_metrics.size()) {
        g_redis_fast_metrics[tl_redis_worker_id].aborts.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
    }
}

static void append_fast_metrics(MakoMetrics* metrics) {
    for (const auto& shard : g_redis_fast_metrics) {
        metrics->txn_commits += shard.commits.load(std::memory_order_relaxed);
        metrics->txn_aborts += shard.aborts.load(std::memory_order_relaxed);
    }
}
static uint64_t redis_lock_hash(const uint8_t* data, size_t len) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool redis_cache_enabled() {
    return g_redis_cache_capacity_bytes != 0;
}

static bool redis_cache_key_matches(
    const RedisCacheEntry& entry, const uint8_t* key_ptr, size_t key_len) {
    return entry.key.size() == key_len
        && (key_len == 0
            || std::memcmp(entry.key.data(), key_ptr, key_len) == 0);
}

// Caller holds the Redis transaction stripe for hash, or is the sole worker.
static bool redis_cache_get(
    uint64_t hash, const uint8_t* key_ptr, size_t key_len, std::string& value) {
    if (!redis_cache_enabled()) {
        return false;
    }
    RedisCacheShard& shard = g_redis_cache[hash % kRedisCacheShards];
    auto it = shard.entries.find(hash);
    if (it == shard.entries.end()
        || !redis_cache_key_matches(it->second, key_ptr, key_len)) {
        g_redis_cache_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    value = it->second.value;
    g_redis_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static void redis_cache_invalidate(const uint8_t* key_ptr, size_t key_len) {
    if (!redis_cache_enabled()) {
        return;
    }
    const uint64_t hash = redis_lock_hash(key_ptr, key_len);
    RedisCacheShard& shard = g_redis_cache[hash % kRedisCacheShards];
    auto it = shard.entries.find(hash);
    if (it == shard.entries.end()
        || !redis_cache_key_matches(it->second, key_ptr, key_len)) {
        return;
    }
    shard.bytes -= it->second.charge;
    shard.entries.erase(it);
    g_redis_cache_invalidations.fetch_add(1, std::memory_order_relaxed);
}

static void redis_cache_put(
    uint64_t hash, const uint8_t* key_ptr, size_t key_len,
    const uint8_t* value_ptr, size_t value_len) {
    if (!redis_cache_enabled()) {
        return;
    }
    RedisCacheShard& shard = g_redis_cache[hash % kRedisCacheShards];
    const size_t charge = key_len + value_len + sizeof(RedisCacheEntry) + 32;
    const size_t shard_capacity = std::max<size_t>(
        1, g_redis_cache_capacity_bytes / kRedisCacheShards);
    auto existing = shard.entries.find(hash);
    if (existing != shard.entries.end()
        && redis_cache_key_matches(existing->second, key_ptr, key_len)) {
        shard.bytes -= existing->second.charge;
        if (charge > shard_capacity) {
            shard.entries.erase(existing);
            g_redis_cache_invalidations.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (value_len == 0) {
            existing->second.value.clear();
        } else {
            existing->second.value.assign(
                reinterpret_cast<const char*>(value_ptr), value_len);
        }
        existing->second.charge = charge;
        shard.bytes += charge;
    } else {
        if (existing != shard.entries.end()) {
            shard.bytes -= existing->second.charge;
            shard.entries.erase(existing);
            g_redis_cache_evictions.fetch_add(1, std::memory_order_relaxed);
        }
        if (charge > shard_capacity) {
            return;
        }
        while (shard.bytes + charge > shard_capacity && !shard.fifo.empty()) {
            auto victim = std::move(shard.fifo.front());
            shard.fifo.pop_front();
            auto victim_it = shard.entries.find(victim.first);
            if (victim_it == shard.entries.end()
                || victim_it->second.generation != victim.second) {
                continue;
            }
            shard.bytes -= victim_it->second.charge;
            shard.entries.erase(victim_it);
            g_redis_cache_evictions.fetch_add(1, std::memory_order_relaxed);
        }
        RedisCacheEntry entry;
        if (key_len != 0) {
            entry.key.assign(reinterpret_cast<const char*>(key_ptr), key_len);
        }
        if (value_len != 0) {
            entry.value.assign(reinterpret_cast<const char*>(value_ptr), value_len);
        }
        entry.charge = charge;
        entry.generation = shard.next_generation++;
        if (shard.next_generation == 0) {
            shard.next_generation = 1;
        }
        const uint64_t generation = entry.generation;
        shard.entries.emplace(hash, std::move(entry));
        shard.fifo.emplace_back(hash, generation);
        shard.bytes += charge;
    }
    g_redis_cache_inserts.fetch_add(1, std::memory_order_relaxed);
}

static void redis_cache_clear() {
    if (!redis_cache_enabled()) {
        return;
    }
    for (auto& shard : g_redis_cache) {
        shard.entries.clear();
        shard.fifo.clear();
        shard.bytes = 0;
        shard.next_generation = 1;
    }
}

static void redis_cache_usage(uint64_t& entries, uint64_t& bytes) {
    entries = 0;
    bytes = 0;
    for (size_t stripe = 0; stripe < g_redis_cache.size(); ++stripe) {
        std::lock_guard<std::mutex> guard(g_redis_txn_key_mutexes[stripe]);
        auto& shard = g_redis_cache[stripe];
        entries += shard.entries.size();
        bytes += shard.bytes;
    }
}

static bool redis_lock_read_u64_le(const uint8_t* data, size_t len, size_t& pos, uint64_t& out) {
    if (data == nullptr || pos + sizeof(uint64_t) > len) {
        return false;
    }
    out = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        out |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
    }
    pos += sizeof(uint64_t);
    return true;
}

static bool redis_lock_unpack_bytes_list(const uint8_t* data, size_t len, std::vector<std::string>& out) {
    out.clear();
    size_t pos = 0;
    uint64_t count = 0;
    if (!redis_lock_read_u64_le(data, len, pos, count)) {
        return false;
    }
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t item_len = 0;
        if (!redis_lock_read_u64_le(data, len, pos, item_len)
            || item_len > len
            || pos + static_cast<size_t>(item_len) > len) {
            return false;
        }
        out.emplace_back(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(item_len));
        pos += static_cast<size_t>(item_len);
    }
    return pos == len;
}

// ---------------------------------------------------------------------------
// HyperLogLog support (PFADD / PFCOUNT / PFMERGE)
//
// A sketch is a plain string value stored at the usual string storage key, so
// TYPE reports "string" and DEL/EXPIRE/DUMP/RESTORE/GET keep working on it.
// The payload uses a private dense layout: a 16-byte header ("MHLL", format
// version 1, then zero padding) followed by one byte per register.
// ---------------------------------------------------------------------------
static constexpr size_t kHllHeaderSize = 16;
static constexpr size_t kHllIndexBits = 14;
static constexpr size_t kHllRegisters = static_cast<size_t>(1) << kHllIndexBits;  // 16384
static constexpr size_t kHllDenseSize = kHllHeaderSize + kHllRegisters;           // 16400
static constexpr int kHllQ = 64 - static_cast<int>(kHllIndexBits);                // 50
static constexpr uint64_t kHllHashSeed = 0xadc83b19ULL;
// HLL_ALPHA_INF from Redis hyperloglog.c: 0.5 / ln(2).
static constexpr double kHllAlphaInf = 0.721347520444481703680;
static constexpr char kHllMagic[4] = {'M', 'H', 'L', 'L'};
static constexpr uint8_t kHllFormatVersion = 1;

// MurmurHash64A by Austin Appleby, ported verbatim from Redis hyperloglog.c.
// Mako only runs on little-endian hosts, so the word load needs no swap.
static uint64_t hll_murmur64a(const void* key, size_t len, uint64_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;
    uint64_t h = seed ^ (static_cast<uint64_t>(len) * m);
    const uint8_t* data = static_cast<const uint8_t*>(key);
    const uint8_t* end = data + (len - (len & 7));

    while (data != end) {
        uint64_t k = 0;
        std::memcpy(&k, data, sizeof(k));
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
        data += 8;
    }

    switch (len & 7) {
        case 7: h ^= static_cast<uint64_t>(data[6]) << 48; [[fallthrough]];
        case 6: h ^= static_cast<uint64_t>(data[5]) << 40; [[fallthrough]];
        case 5: h ^= static_cast<uint64_t>(data[4]) << 32; [[fallthrough]];
        case 4: h ^= static_cast<uint64_t>(data[3]) << 24; [[fallthrough]];
        case 3: h ^= static_cast<uint64_t>(data[2]) << 16; [[fallthrough]];
        case 2: h ^= static_cast<uint64_t>(data[1]) << 8;  [[fallthrough]];
        case 1: h ^= static_cast<uint64_t>(data[0]);
                h *= m;
                break;
        default: break;
    }

    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

static bool hll_value_is_valid(const std::string& value) {
    return value.size() == kHllDenseSize
        && std::memcmp(value.data(), kHllMagic, sizeof(kHllMagic)) == 0;
}

static std::string hll_make_empty() {
    std::string out(kHllDenseSize, '\0');
    std::memcpy(out.data(), kHllMagic, sizeof(kHllMagic));
    out[sizeof(kHllMagic)] = static_cast<char>(kHllFormatVersion);
    return out;
}

// Register index and run length for one element, exactly as Redis hllPatLen:
// the low kHllIndexBits of the hash select the register, the remaining bits
// carry the run, and a sentinel bit at position kHllQ bounds the scan, so the
// value is in [1, kHllQ + 1]. Keeping Redis's register placement leaves an
// RDB-compatible export possible later.
static void hll_element_slot(const void* data, size_t len, size_t& index, uint8_t& run) {
    const uint64_t hash = hll_murmur64a(data, len, kHllHashSeed);
    index = static_cast<size_t>(hash & (kHllRegisters - 1));
    uint64_t bits = hash >> kHllIndexBits;
    bits |= static_cast<uint64_t>(1) << kHllQ;  // guarantees the loop terminates
    uint8_t count = 1;
    uint64_t bit = 1;
    while ((bits & bit) == 0) {
        ++count;
        bit <<= 1;
    }
    run = count;
}

// tau and sigma from Redis hyperloglog.c (Ertl, arXiv:1702.01284).
static double hll_tau(double x) {
    if (x == 0.0 || x == 1.0) {
        return 0.0;
    }
    double z_prime = 0.0;
    double y = 1.0;
    double z = 1 - x;
    do {
        x = std::sqrt(x);
        z_prime = z;
        y *= 0.5;
        z -= std::pow(1 - x, 2) * y;
    } while (z_prime != z);
    return z / 3;
}

static double hll_sigma(double x) {
    if (x == 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    double z_prime = 0.0;
    double y = 1;
    double z = x;
    do {
        x *= x;
        z_prime = z;
        z += x * y;
        y += y;
    } while (z_prime != z);
    return z;
}

// Redis hllCount for the dense encoding: the improved Ertl estimator, which
// needs no separate linear-counting branch for small cardinalities.
static int64_t hll_estimate(const uint8_t* registers) {
    int reghisto[64] = {0};
    for (size_t i = 0; i < kHllRegisters; ++i) {
        ++reghisto[registers[i] & 63];
    }
    const double m = static_cast<double>(kHllRegisters);
    double z = m * hll_tau((m - static_cast<double>(reghisto[kHllQ + 1])) / m);
    for (int j = kHllQ; j >= 1; --j) {
        z += static_cast<double>(reghisto[j]);
        z *= 0.5;
    }
    z += m * hll_sigma(static_cast<double>(reghisto[0]) / m);
    const long double estimate =
        static_cast<long double>(kHllAlphaInf) * static_cast<long double>(m)
        * static_cast<long double>(m) / static_cast<long double>(z);
    return static_cast<int64_t>(std::llround(estimate));
}

// ---------------------------------------------------------------------------
// BITFIELD support (TXN_OP_BITFIELD)
//
// Bits are numbered big-endian inside the string: bit 0 is the most
// significant bit of byte 0. Reads past the end of the string read zeros; the
// caller grows the string before any write, exactly like Redis. The overflow
// helpers are ports of checkSignedBitfieldOverflow/checkUnsignedBitfieldOverflow
// from Redis bitops.c, with the increment limits computed in unsigned
// arithmetic so the wraparound Redis relies on stays defined.
// ---------------------------------------------------------------------------
enum BitFieldOverflowType {
    kBitFieldWrap = 0,
    kBitFieldSat = 1,
    kBitFieldFail = 2,
};

static uint64_t bitfield_get_unsigned(const std::string& value, uint64_t offset, uint32_t bits) {
    uint64_t result = 0;
    for (uint32_t i = 0; i < bits; ++i) {
        const uint64_t bit_index = offset + i;
        const size_t byte_index = static_cast<size_t>(bit_index >> 3);
        uint64_t bit = 0;
        if (byte_index < value.size()) {
            bit = (static_cast<uint8_t>(value[byte_index]) >> (7 - (bit_index & 7))) & 1;
        }
        result = (result << 1) | bit;
    }
    return result;
}

static int64_t bitfield_get_signed(const std::string& value, uint64_t offset, uint32_t bits) {
    uint64_t raw = bitfield_get_unsigned(value, offset, bits);
    if (bits < 64 && (raw & (static_cast<uint64_t>(1) << (bits - 1))) != 0) {
        raw |= ~static_cast<uint64_t>(0) << bits;  // sign-extend
    }
    return static_cast<int64_t>(raw);
}

// The string must already be long enough; bits beyond its end are dropped.
static void bitfield_set_bits(std::string& value, uint64_t offset, uint32_t bits, uint64_t raw) {
    for (uint32_t i = 0; i < bits; ++i) {
        const uint64_t bit = (raw >> (bits - 1 - i)) & 1;
        const uint64_t bit_index = offset + i;
        const size_t byte_index = static_cast<size_t>(bit_index >> 3);
        if (byte_index >= value.size()) {
            break;
        }
        const uint8_t mask = static_cast<uint8_t>(1u << (7 - (bit_index & 7)));
        uint8_t byte = static_cast<uint8_t>(value[byte_index]);
        if (bit != 0) {
            byte |= mask;
        } else {
            byte &= static_cast<uint8_t>(~mask);
        }
        value[byte_index] = static_cast<char>(byte);
    }
}

// Returns 0 when value+incr fits in the signed field, 1 on positive overflow
// and -1 on negative overflow. On overflow `limit` receives the WRAP or SAT
// replacement; it is untouched for FAIL.
static int bitfield_signed_overflow(
    int64_t value, int64_t incr, uint32_t bits, int owtype, int64_t& limit) {
    const int64_t max = (bits == 64)
                            ? INT64_MAX
                            : ((static_cast<int64_t>(1) << (bits - 1)) - 1);
    const int64_t min = (-max) - 1;
    const int64_t maxincr =
        static_cast<int64_t>(static_cast<uint64_t>(max) - static_cast<uint64_t>(value));
    const int64_t minincr =
        static_cast<int64_t>(static_cast<uint64_t>(min) - static_cast<uint64_t>(value));

    int direction = 0;
    if (value > max || (bits != 64 && incr > maxincr)
        || (value >= 0 && incr > 0 && incr > maxincr)) {
        direction = 1;
    } else if (value < min || (bits != 64 && incr < minincr)
               || (value < 0 && incr < 0 && incr < minincr)) {
        direction = -1;
    } else {
        return 0;
    }

    if (owtype == kBitFieldWrap) {
        const uint64_t msb = static_cast<uint64_t>(1) << (bits - 1);
        uint64_t wrapped = static_cast<uint64_t>(value) + static_cast<uint64_t>(incr);
        if (bits < 64) {
            const uint64_t mask = ~static_cast<uint64_t>(0) << bits;
            if ((wrapped & msb) != 0) {
                wrapped |= mask;
            } else {
                wrapped &= ~mask;
            }
        }
        limit = static_cast<int64_t>(wrapped);
    } else if (owtype == kBitFieldSat) {
        limit = (direction > 0) ? max : min;
    }
    return direction;
}

static int bitfield_unsigned_overflow(
    uint64_t value, int64_t incr, uint32_t bits, int owtype, uint64_t& limit) {
    // u64 is not a legal BITFIELD encoding, so bits is always below 64 here.
    const uint64_t max = (bits == 64)
                             ? ~static_cast<uint64_t>(0)
                             : ((static_cast<uint64_t>(1) << bits) - 1);
    const int64_t maxincr = static_cast<int64_t>(max - value);
    const int64_t minincr = static_cast<int64_t>(-static_cast<int64_t>(value));

    int direction = 0;
    if (value > max || (incr > 0 && incr > maxincr)) {
        direction = 1;
    } else if (incr < 0 && incr < minincr) {
        direction = -1;
    } else {
        return 0;
    }

    if (owtype == kBitFieldWrap) {
        const uint64_t mask = (bits == 64) ? 0 : (~static_cast<uint64_t>(0) << bits);
        limit = (value + static_cast<uint64_t>(incr)) & ~mask;
    } else if (owtype == kBitFieldSat) {
        limit = (direction > 0) ? max : 0;
    }
    return direction;
}

// ===== Redis Streams =====
//
// A stream lives in five hidden key families (documented in
// include/transaction_ffi.h): "\x01X#:" one meta record per stream, "\x01X:"
// one record per entry, "\x01XG:" one per consumer group, "\x01XC:" one per
// consumer and "\x01XP:" one per pending-entry-list entry. Entry and PEL keys
// end in the 128-bit stream ID written big-endian, so a prefix range scan of
// either family walks the records in ID order and an XRANGE start/end pair is
// a plain key range.
//
// Every ID crosses the FFI as decimal "ms-seq" text; both halves are unsigned
// 64-bit, as in Redis, so they are parsed and formatted here rather than
// squeezed into the signed int64 of a TxnOpResult.
struct RedisStreamId {
    uint64_t ms = 0;
    uint64_t seq = 0;
};

static int stream_id_compare(const RedisStreamId& lhs, const RedisStreamId& rhs) {
    if (lhs.ms != rhs.ms) {
        return lhs.ms < rhs.ms ? -1 : 1;
    }
    if (lhs.seq != rhs.seq) {
        return lhs.seq < rhs.seq ? -1 : 1;
    }
    return 0;
}

static bool stream_id_is_zero(const RedisStreamId& id) {
    return id.ms == 0 && id.seq == 0;
}

static bool stream_parse_u64(const char* data, size_t len, uint64_t& out) {
    if (len == 0 || len > 20) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

/// Parse the "<ms>-<seq>" spelling every stream ID crosses the FFI as. Rust has
/// already expanded "-", "+", a bare "<ms>" and the "(" exclusive forms, so the
/// executor only ever sees both halves written out.
static bool stream_id_parse(const std::string& text, RedisStreamId& out) {
    const size_t dash = text.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 == text.size()) {
        return false;
    }
    RedisStreamId parsed;
    if (!stream_parse_u64(text.data(), dash, parsed.ms)) {
        return false;
    }
    if (!stream_parse_u64(text.data() + dash + 1, text.size() - dash - 1, parsed.seq)) {
        return false;
    }
    out = parsed;
    return true;
}

static std::string stream_id_format(const RedisStreamId& id) {
    std::string out = std::to_string(id.ms);
    out.push_back('-');
    out.append(std::to_string(id.seq));
    return out;
}

/// The ID immediately after `id`, false when there is none (Redis's
/// streamIncrID).
static bool stream_id_incr(RedisStreamId& id) {
    if (id.seq == std::numeric_limits<uint64_t>::max()) {
        if (id.ms == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        id.ms += 1;
        id.seq = 0;
    } else {
        id.seq += 1;
    }
    return true;
}

static void stream_put_u64_be(std::string& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

static void stream_put_u64_le(std::string& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

static uint64_t stream_get_u64_le(const std::string& raw, size_t pos) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<unsigned char>(raw[pos + i]))
            << (8 * i);
    }
    return value;
}

/// The stream's own record: everything XINFO STREAM reports that is not an
/// entry, kept beside the entries so XLEN and the ID checks are one read.
struct RedisStreamMeta {
    uint64_t length = 0;
    RedisStreamId last_id;
    // recorded-first-entry-id: 0-0 while the stream holds no entry.
    RedisStreamId first_id;
    uint64_t entries_added = 0;
    RedisStreamId max_deleted_id;
    uint64_t groups = 0;
};

static constexpr size_t kStreamMetaSize = 9 * 8;

static std::string stream_pack_meta(const RedisStreamMeta& meta) {
    std::string raw;
    raw.reserve(kStreamMetaSize);
    stream_put_u64_le(raw, meta.length);
    stream_put_u64_le(raw, meta.last_id.ms);
    stream_put_u64_le(raw, meta.last_id.seq);
    stream_put_u64_le(raw, meta.first_id.ms);
    stream_put_u64_le(raw, meta.first_id.seq);
    stream_put_u64_le(raw, meta.entries_added);
    stream_put_u64_le(raw, meta.max_deleted_id.ms);
    stream_put_u64_le(raw, meta.max_deleted_id.seq);
    stream_put_u64_le(raw, meta.groups);
    return raw;
}

static bool stream_unpack_meta(const std::string& raw, RedisStreamMeta& meta) {
    if (raw.size() != kStreamMetaSize) {
        return false;
    }
    meta.length = stream_get_u64_le(raw, 0);
    meta.last_id.ms = stream_get_u64_le(raw, 8);
    meta.last_id.seq = stream_get_u64_le(raw, 16);
    meta.first_id.ms = stream_get_u64_le(raw, 24);
    meta.first_id.seq = stream_get_u64_le(raw, 32);
    meta.entries_added = stream_get_u64_le(raw, 40);
    meta.max_deleted_id.ms = stream_get_u64_le(raw, 48);
    meta.max_deleted_id.seq = stream_get_u64_le(raw, 56);
    meta.groups = stream_get_u64_le(raw, 64);
    return true;
}

/// A consumer group. `entries_read` is Redis's logical read counter, and -1 is
/// its SCG_INVALID_ENTRIES_READ: the counter cannot be known for a group whose
/// position sits behind a tombstone.
struct RedisStreamGroup {
    RedisStreamId last_id;
    int64_t entries_read = -1;
    uint64_t consumers = 0;
    uint64_t pel = 0;
};

static constexpr size_t kStreamGroupSize = 5 * 8;

static std::string stream_pack_group(const RedisStreamGroup& group) {
    std::string raw;
    raw.reserve(kStreamGroupSize);
    stream_put_u64_le(raw, group.last_id.ms);
    stream_put_u64_le(raw, group.last_id.seq);
    stream_put_u64_le(raw, static_cast<uint64_t>(group.entries_read));
    stream_put_u64_le(raw, group.consumers);
    stream_put_u64_le(raw, group.pel);
    return raw;
}

static bool stream_unpack_group(const std::string& raw, RedisStreamGroup& group) {
    if (raw.size() != kStreamGroupSize) {
        return false;
    }
    group.last_id.ms = stream_get_u64_le(raw, 0);
    group.last_id.seq = stream_get_u64_le(raw, 8);
    group.entries_read = static_cast<int64_t>(stream_get_u64_le(raw, 16));
    group.consumers = stream_get_u64_le(raw, 24);
    group.pel = stream_get_u64_le(raw, 32);
    return true;
}

/// Redis 7.2 keeps two clocks per consumer: seen-time moves on every command
/// the consumer issues, active-time only when it was actually handed an entry,
/// and a consumer that has never been handed one reports inactive -1.
struct RedisStreamConsumer {
    int64_t seen_time_ms = 0;
    int64_t active_time_ms = -1;
    uint64_t pending = 0;
};

static constexpr size_t kStreamConsumerSize = 3 * 8;

static std::string stream_pack_consumer(const RedisStreamConsumer& consumer) {
    std::string raw;
    raw.reserve(kStreamConsumerSize);
    stream_put_u64_le(raw, static_cast<uint64_t>(consumer.seen_time_ms));
    stream_put_u64_le(raw, static_cast<uint64_t>(consumer.active_time_ms));
    stream_put_u64_le(raw, consumer.pending);
    return raw;
}

static bool stream_unpack_consumer(const std::string& raw, RedisStreamConsumer& consumer) {
    if (raw.size() != kStreamConsumerSize) {
        return false;
    }
    consumer.seen_time_ms = static_cast<int64_t>(stream_get_u64_le(raw, 0));
    consumer.active_time_ms = static_cast<int64_t>(stream_get_u64_le(raw, 8));
    consumer.pending = stream_get_u64_le(raw, 16);
    return true;
}

/// One pending-entry-list record: who holds the entry and for how long.
struct RedisStreamNack {
    std::string consumer;
    int64_t delivery_time_ms = 0;
    uint64_t delivery_count = 1;
};

static std::string stream_pack_nack(const RedisStreamNack& nack) {
    std::string raw;
    raw.reserve(16 + nack.consumer.size());
    stream_put_u64_le(raw, static_cast<uint64_t>(nack.delivery_time_ms));
    stream_put_u64_le(raw, nack.delivery_count);
    raw.append(nack.consumer);
    return raw;
}

static bool stream_unpack_nack(const std::string& raw, RedisStreamNack& nack) {
    if (raw.size() < 16) {
        return false;
    }
    nack.delivery_time_ms = static_cast<int64_t>(stream_get_u64_le(raw, 0));
    nack.delivery_count = stream_get_u64_le(raw, 8);
    nack.consumer.assign(raw, 16, raw.size() - 16);
    return true;
}

/// Redis's streamRangeHasTombstones: could an XDEL have removed an entry at or
/// after `start`? The answer drives whether a group's read counter can still be
/// advanced by simple addition.
static bool stream_range_has_tombstones(const RedisStreamMeta& meta, const RedisStreamId& start) {
    if (meta.length == 0 || stream_id_is_zero(meta.max_deleted_id)) {
        return false;
    }
    if (stream_id_compare(meta.first_id, meta.max_deleted_id) > 0) {
        return false;
    }
    return stream_id_compare(start, meta.max_deleted_id) <= 0;
}

/// Redis's streamEstimateDistanceFromFirstEverEntry: how many entries were ever
/// added to the stream at or before `id`. False when the answer cannot be known
/// because a tombstone sits between the first entry and `id`.
static bool stream_estimate_entries_read(
    const RedisStreamMeta& meta,
    const RedisStreamId& id,
    int64_t& entries_read) {
    if (meta.entries_added == 0) {
        entries_read = 0;
        return true;
    }
    if (meta.length == 0 && stream_id_compare(id, meta.last_id) <= 0) {
        entries_read = static_cast<int64_t>(meta.entries_added);
        return true;
    }
    const int cmp_last = stream_id_compare(id, meta.last_id);
    if (cmp_last == 0) {
        entries_read = static_cast<int64_t>(meta.entries_added);
        return true;
    }
    if (cmp_last > 0) {
        return false;
    }
    const int cmp_first = stream_id_compare(id, meta.first_id);
    const bool no_fragmentation = stream_id_is_zero(meta.max_deleted_id)
        || stream_id_compare(meta.max_deleted_id, meta.first_id) < 0;
    if (no_fragmentation) {
        if (cmp_first < 0) {
            entries_read = static_cast<int64_t>(meta.entries_added - meta.length);
            return true;
        }
        if (cmp_first == 0) {
            entries_read = static_cast<int64_t>(meta.entries_added - meta.length + 1);
            return true;
        }
    }
    return false;
}

/// Redis's streamCGLag. False when the lag cannot be determined, which XINFO
/// reports as a null.
static bool stream_group_lag(
    const RedisStreamMeta& meta,
    const RedisStreamGroup& group,
    int64_t& lag) {
    if (meta.entries_added == 0) {
        lag = 0;
        return true;
    }
    if (group.entries_read >= 0 && !stream_range_has_tombstones(meta, group.last_id)) {
        lag = static_cast<int64_t>(meta.entries_added) - group.entries_read;
        return true;
    }
    int64_t entries_read = 0;
    if (stream_estimate_entries_read(meta, group.last_id, entries_read)) {
        lag = static_cast<int64_t>(meta.entries_added) - entries_read;
        return true;
    }
    return false;
}

static bool redis_op_is_read_only(const TxnOperation& op) {
    switch (op.op) {
        case TXN_OP_GET:
        case TXN_OP_EXISTS:
        case TXN_OP_STRLEN:
        case TXN_OP_TTL:
        case TXN_OP_SCAN:
        case TXN_OP_SISMEMBER:
        case TXN_OP_SCARD:
        case TXN_OP_SMEMBERS:
        case TXN_OP_SRANDMEMBER:
        case TXN_OP_TYPE:
        case TXN_OP_LLEN:
        case TXN_OP_LINDEX:
        case TXN_OP_LRANGE:
        case TXN_OP_LPOS:
        case TXN_OP_ZSCORE:
        case TXN_OP_ZCARD:
        case TXN_OP_ZRANGE:
        case TXN_OP_ZRANK:
        case TXN_OP_ZCOUNT:
        case TXN_OP_ZSCAN:
        case TXN_OP_HGET:
        case TXN_OP_HMGET:
        case TXN_OP_HGETALL:
        case TXN_OP_HEXISTS:
        case TXN_OP_HLEN:
        case TXN_OP_HKEYS:
        case TXN_OP_HVALS:
        case TXN_OP_HSTRLEN:
        case TXN_OP_HSCAN:
        // HTTL/HPTTL/HEXPIRETIME/HPEXPIRETIME only read the per-field TTL side
        // keys. Like the other hash reads it can still drop a field whose time
        // has already passed, which is the same lazy-expiry write the key-level
        // TTL check has always done from a read path.
        case TXN_OP_HFIELD_TTL:
        case TXN_OP_GETBIT:
        case TXN_OP_GETRANGE:
        case TXN_OP_DUMP:
        case TXN_OP_ZRANGEBYLEX:
        case TXN_OP_ZLEXCOUNT:
        case TXN_OP_ZRANDMEMBER:
        case TXN_OP_HLL_COUNT:
        // Stream reads. TXN_OP_XRANGE serves XRANGE, XREVRANGE and each stream
        // of an XREAD; XLEN and XINFO only report.
        case TXN_OP_XRANGE:
        case TXN_OP_XLEN:
        case TXN_OP_XINFO:
        case TXN_OP_XPENDING:
            return true;
        case TXN_OP_SET_ALGEBRA:
            return (op.flags & TXN_FLAG_SET_ALGEBRA_STORE) == 0;
        default:
            return false;
    }
}

static bool redis_request_has_flushdb(const TxnRequest* request) {
    if (request == nullptr || request->ops == nullptr) {
        return false;
    }
    for (size_t i = 0; i < request->num_ops; ++i) {
        if (request->ops[i].op == TXN_OP_FLUSHDB) {
            return true;
        }
    }
    return false;
}

static bool redis_request_has_write(const TxnRequest* request) {
    if (request == nullptr || request->ops == nullptr) {
        return false;
    }
    for (size_t i = 0; i < request->num_ops; ++i) {
        if (!redis_op_is_read_only(request->ops[i])) {
            return true;
        }
    }
    return false;
}

static bool redis_op_uses_only_primary_lock_key(const TxnOperation& op) {
    switch (op.op) {
        case TXN_OP_RENAME:
        case TXN_OP_COPY:
        case TXN_OP_MOVE:
        case TXN_OP_SMOVE:
        case TXN_OP_LMOVE:
        case TXN_OP_SORT:
        case TXN_OP_ZRANGESTORE:
        case TXN_OP_SET_ALGEBRA:
        case TXN_OP_ZSET_ALGEBRA:
        case TXN_OP_BPOP:
        case TXN_OP_ZMPOP:
        case TXN_OP_BITOP:
        case TXN_OP_HLL_COUNT:
        case TXN_OP_HLL_MERGE:
            return false;
        default:
            return true;
    }
}

static std::vector<size_t> redis_request_lock_stripes(const TxnRequest* request) {
    std::vector<size_t> stripes;
    if (request == nullptr || request->ops == nullptr) {
        return stripes;
    }
    stripes.reserve(request->num_ops);
    auto add_lock_key = [&](const uint8_t* key, size_t len) {
        const size_t stripe = redis_lock_hash(key, len) % kRedisTxnLockStripes;
        stripes.push_back(stripe);
    };
    auto add_packed_lock_keys = [&](const TxnOperation& op, size_t max_items) {
        std::vector<std::string> keys;
        if (!redis_lock_unpack_bytes_list(op.val_ptr, op.val_len, keys)) {
            return;
        }
        const size_t limit = std::min(max_items, keys.size());
        for (size_t i = 0; i < limit; ++i) {
            if (!keys[i].empty()) {
                add_lock_key(reinterpret_cast<const uint8_t*>(keys[i].data()), keys[i].size());
            }
        }
    };
    for (size_t i = 0; i < request->num_ops; ++i) {
        const TxnOperation& op = request->ops[i];
        add_lock_key(op.key_ptr, op.key_len);
        switch (op.op) {
            case TXN_OP_RENAME:
            case TXN_OP_COPY:
                if (op.val_ptr != nullptr) {
                    add_lock_key(op.val_ptr, op.val_len);
                }
                break;
            case TXN_OP_MOVE:
            case TXN_OP_SMOVE:
            case TXN_OP_LMOVE:
            case TXN_OP_SORT:
            case TXN_OP_ZRANGESTORE:
                add_packed_lock_keys(op, 1);
                break;
            case TXN_OP_SET_ALGEBRA:
            case TXN_OP_ZSET_ALGEBRA:
            case TXN_OP_BPOP:
            case TXN_OP_ZMPOP:
            case TXN_OP_BITOP:
            case TXN_OP_HLL_COUNT:
            case TXN_OP_HLL_MERGE:
                add_packed_lock_keys(op, SIZE_MAX);
                break;
            default:
                break;
        }
    }
    std::sort(stripes.begin(), stripes.end());
    stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());
    return stripes;
}

static void redis_cache_invalidate_committed_writes(const TxnRequest* request) {
    if (!redis_cache_enabled() || request == nullptr || request->ops == nullptr) {
        return;
    }
    auto invalidate_packed_keys = [](const TxnOperation& op, size_t max_items) {
        std::vector<std::string> keys;
        if (!redis_lock_unpack_bytes_list(op.val_ptr, op.val_len, keys)) {
            return;
        }
        const size_t limit = std::min(max_items, keys.size());
        for (size_t i = 0; i < limit; ++i) {
            redis_cache_invalidate(
                reinterpret_cast<const uint8_t*>(keys[i].data()), keys[i].size());
        }
    };
    for (size_t i = 0; i < request->num_ops; ++i) {
        const TxnOperation& op = request->ops[i];
        if (redis_op_is_read_only(op)) {
            continue;
        }
        if (op.op == TXN_OP_FLUSHDB) {
            redis_cache_clear();
            continue;
        }
        redis_cache_invalidate(op.key_ptr, op.key_len);
        switch (op.op) {
            case TXN_OP_RENAME:
            case TXN_OP_COPY:
                if (op.val_ptr != nullptr || op.val_len == 0) {
                    redis_cache_invalidate(op.val_ptr, op.val_len);
                }
                break;
            case TXN_OP_MOVE:
            case TXN_OP_SMOVE:
            case TXN_OP_LMOVE:
            case TXN_OP_SORT:
            case TXN_OP_ZRANGESTORE:
                invalidate_packed_keys(op, 1);
                break;
            case TXN_OP_SET_ALGEBRA:
            case TXN_OP_ZSET_ALGEBRA:
            case TXN_OP_BPOP:
            case TXN_OP_ZMPOP:
                invalidate_packed_keys(op, SIZE_MAX);
                break;
            default:
                break;
        }
    }
}

// Initialize thread-local state for database operations
void ensure_thread_info() {
    if (!tl_initialized && g_mako_db != nullptr) {
        abstract_db* db = g_mako_db->GetDB();
        if (db == nullptr) {
            return;
        }

        // Initialize both the runtime binding and the benchmark DB thread state.
        // mako::DB::InitThread() can no-op before leader config is installed,
        // but the Redis path still needs Masstree/STO thread-local state.
        SiloRuntime::Current()->BindToCurrentThread();
        db->thread_init(false, 0);

        // Allocate thread-local buffers
        tl_arena = new str_arena();
        tl_txn_buf.resize(db->sizeof_txn_object(0));
        tl_initialized = true;

        std::cout << "[cpp] Thread " << std::this_thread::get_id()
                  << " initialized for mako::DB" << std::endl;
    }
}

// Cleanup thread-local state
void cleanup_thread_info() {
    if (tl_arena) {
        delete tl_arena;
        tl_arena = nullptr;
    }
    tl_txn_buf.clear();
    tl_key_buf.clear();
    tl_ttl_key_buf.clear();
    tl_ttl_val_buf.clear();
    tl_val_buf.clear();
    tl_exists_buf.clear();
    tl_delete_buf.clear();
    tl_encoded_val_buf.clear();
    tl_bench_val_buf.clear();
    tl_initialized = false;
}

// ===== Logical databases =====
//
// See the "Logical databases" comment in include/transaction_ffi.h. Database 0
// keys are stored verbatim; a key in database 1..15 carries the three-byte
// prefix 0x02 <db> ':' in front of the Redis-visible name, before any storage
// prefix is added. Everything below only has to answer one question: which
// database does this stored key belong to?

static constexpr unsigned char kRedisDbMarker = 0x02;

// Database of a Redis-visible key.
static int redis_user_key_db_index(const char* data, size_t len) {
    if (len >= 3 && static_cast<unsigned char>(data[0]) == kRedisDbMarker && data[2] == ':') {
        return static_cast<unsigned char>(data[1]);
    }
    return 0;
}

// Database of a key as it sits in storage. Anything this cannot place counts
// as database 0, so a FLUSHDB on database 0 still clears everything a
// single-database server would have cleared.
//
// Two layouts reach storage, from the make_* lambdas in execute_transaction:
//   "table_key_" <key>                a string value (the only prefixed one)
//   "\x01TTL:" <key>                  key expiry metadata
//   "\x01<tag>:" <len:8> <key> …      every collection namespace: set, hash,
//                                     list and zset members, scores and metas
// Only "\x01TTL:" lacks the eight-byte little-endian logical-key length, so a
// new collection namespace needs no change here as long as it keeps that
// shape. In every layout the logical key is what carries the database prefix,
// which is the whole point of prefixing the Redis-visible name rather than the
// storage name.
static int redis_storage_key_db_index(const char* data, size_t len) {
    constexpr std::string_view kStoragePrefix = "table_key_";
    std::string_view key(data, len);
    if (key.size() >= kStoragePrefix.size()
        && key.substr(0, kStoragePrefix.size()) == kStoragePrefix) {
        key.remove_prefix(kStoragePrefix.size());
    }
    if (key.empty()) {
        return 0;
    }
    if (static_cast<unsigned char>(key[0]) != 0x01) {
        return redis_user_key_db_index(key.data(), key.size());
    }
    const size_t colon = key.find(':');
    if (colon == std::string_view::npos) {
        return 0;
    }
    size_t offset = colon + 1;
    if (key.substr(0, colon + 1) != std::string_view("\x01TTL:", 5)) {
        offset += 8;
    }
    if (offset >= key.size()) {
        return 0;
    }
    return redis_user_key_db_index(key.data() + offset, key.size() - offset);
}

// The logical key of one entry of a per-transaction staging buffer: the set
// buffers hold the key itself, the map buffers hold it in `first`.
static inline const std::string& redis_staged_entry_key(const std::string& entry) {
    return entry;
}

template <typename T>
static inline const std::string& redis_staged_entry_key(
    const std::pair<const std::string, T>& entry) {
    return entry.first;
}

// The FLUSHDB/FLUSHALL payload described in transaction_ffi.h, decoded to a
// database filter: -1 clears every database, 0..15 clears just that one.
static int redis_flush_db_filter(const void* val_ptr, size_t val_len) {
    if (val_ptr == nullptr || val_len == 0) {
        return -1;
    }
    const char* data = static_cast<const char*>(val_ptr);
    if (static_cast<unsigned char>(data[0]) != kRedisDbMarker) {
        return -1;
    }
    if (val_len == 1) {
        return 0;
    }
    return static_cast<unsigned char>(data[1]);
}

static bool execute_flushdb_chunked(int db_filter, size_t chunk_size = 1024) {
    ensure_thread_info();
    if (g_mako_db == nullptr || g_table == nullptr) {
        return false;
    }

    class FlushChunkScanCallback : public oi_scan_callback {
    public:
        FlushChunkScanCallback(std::vector<std::string>& keys, size_t limit, int db_filter)
            : keys_(keys), limit_(limit), db_filter_(db_filter) {}

        bool invoke(const char* keyp, size_t keylen, const std::string&) override {
            if (db_filter_ >= 0 && redis_storage_key_db_index(keyp, keylen) != db_filter_) {
                return true;
            }
            keys_.emplace_back(keyp, keylen);
            return keys_.size() < limit_;
        }

    private:
        std::vector<std::string>& keys_;
        size_t limit_;
        int db_filter_;
    };

    for (;;) {
        if (tl_arena) {
            tl_arena->reset();
        }

        std::vector<std::string> keys_to_delete;
        keys_to_delete.reserve(chunk_size);
        void* txn = g_mako_db->BeginTransaction();

        try {
            FlushChunkScanCallback callback(keys_to_delete, chunk_size, db_filter);
            tx_scan(g_table, txn, std::string(), nullptr, callback, tl_arena);

            if (keys_to_delete.empty()) {
                g_mako_db->Rollback(txn);
                return true;
            }

            for (const auto& storage_key : keys_to_delete) {
                mako::Status s = redis_table_delete(txn, storage_key);
                if (!s.ok() && !s.IsNotFound()) {
                    g_mako_db->Rollback(txn);
                    return false;
                }
            }

            g_mako_db->Commit(txn);
            wait_for_redis_replication();
        } catch (abstract_db::abstract_abort_exception&) {
            g_mako_db->Rollback(txn);
            return false;
        } catch (...) {
            g_mako_db->Rollback(txn);
            return false;
        }
    }
}

// Specialized allocation-light executor for the raw plain GET/SET path.
static bool execute_fast_mako_string(
    uint32_t op,
    const uint8_t* key_ptr,
    size_t key_len,
    const uint8_t* val_ptr,
    size_t val_len,
    FastMakoStringResult* result) {
    if (result == nullptr) {
        return false;
    }
    result->status = FAST_MAKO_ABORTED;
    result->data_ptr = nullptr;
    result->data_len = 0;

    const bool is_get = op == TXN_OP_GET;
    const bool is_set = op == TXN_OP_SET;
    if ((!is_get && !is_set)
        || (key_ptr == nullptr && key_len != 0)
        || (is_set && val_ptr == nullptr && val_len != 0)) {
        return false;
    }

    // Benchmark-only ceiling: retain RESP parsing, Rust/C++ FFI, and response
    // formatting while removing every database operation.
    if (g_redis_bench_no_db) {
        if (is_get) {
            if (tl_bench_val_buf.size() != g_redis_bench_value_size) {
                tl_bench_val_buf.assign(g_redis_bench_value_size, 'Y');
            }
            result->status = FAST_MAKO_GET_HIT;
            result->data_ptr = reinterpret_cast<const uint8_t*>(tl_bench_val_buf.data());
            result->data_len = tl_bench_val_buf.size();
        } else {
            result->status = FAST_MAKO_SET_OK;
        }
        record_fast_commit();
        return true;
    }

    ensure_thread_info();
    if (g_mako_db == nullptr || g_table == nullptr) {
        return false;
    }
    if (is_set && !redis_can_write_here()) {
        record_fast_abort();
        return true;
    }

    // FLUSHDB takes every stripe before scanning. Using the command's stripe
    // here preserves keyspace exclusion without making all request workers
    // contend on shared_mutex's reader-count cache line.
    const uint64_t key_hash = redis_lock_hash(key_ptr, key_len);
    std::unique_lock<std::mutex> redis_single_key_lock;
    if (!g_redis_single_worker_mode && !g_redis_bench_skip_locks) {
        const size_t stripe = key_hash % kRedisTxnLockStripes;
        if (!g_redis_bench_skip_mutex) {
            redis_single_key_lock = std::unique_lock<std::mutex>(g_redis_txn_key_mutexes[stripe]);
        }
    }

    if (is_get) {
        tl_val_buf.clear();
        if (redis_cache_get(key_hash, key_ptr, key_len, tl_val_buf)) {
            result->status = FAST_MAKO_GET_HIT;
            result->data_ptr = reinterpret_cast<const uint8_t*>(tl_val_buf.data());
            result->data_len = tl_val_buf.size();
            return true;
        }
    }

    tl_key_buf.clear();
    tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
    tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
    if (key_len != 0) {
        tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);
    }

    tl_ttl_key_buf.clear();
    tl_ttl_key_buf.reserve(sizeof("\x01TTL:") - 1 + key_len);
    tl_ttl_key_buf.append("\x01TTL:", sizeof("\x01TTL:") - 1);
    if (key_len != 0) {
        tl_ttl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);
    }

    void* txn = g_mako_db->BeginTransaction();
    auto abort_transaction = [&]() {
        g_mako_db->Rollback(txn);
        result->status = FAST_MAKO_ABORTED;
        record_fast_abort();
        return true;
    };

    try {
        if (is_get) {
            tl_ttl_val_buf.clear();
            mako::Status ttl_status = g_redis_bench_skip_ttl
                ? mako::Status::NotFound()
                : redis_table_get(txn, tl_ttl_key_buf, tl_ttl_val_buf);
            if (!ttl_status.ok() && !ttl_status.IsNotFound()) {
                return abort_transaction();
            }

            bool expired = false;
            if (ttl_status.ok()
                && !tl_ttl_val_buf.empty()
                && !std::isspace(static_cast<unsigned char>(tl_ttl_val_buf.front()))) {
                errno = 0;
                char* end = nullptr;
                long long parsed = std::strtoll(tl_ttl_val_buf.c_str(), &end, 10);
                if (errno != ERANGE
                    && end != tl_ttl_val_buf.c_str()
                    && end == tl_ttl_val_buf.c_str() + tl_ttl_val_buf.size()) {
                    const int64_t now_ms = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    expired = static_cast<int64_t>(parsed) <= now_ms;
                }
            }

            if (expired) {
                mako::Status s = redis_table_delete(txn, tl_key_buf);
                if (s.ok() || s.IsNotFound()) {
                    s = redis_table_delete(txn, tl_ttl_key_buf);
                }
                if (!s.ok() && !s.IsNotFound()) {
                    return abort_transaction();
                }
                g_mako_db->Commit(txn);
                result->status = FAST_MAKO_GET_MISS;
                record_fast_commit();
                return true;
            }

            tl_val_buf.clear();
            mako::Status s = redis_table_get(txn, tl_key_buf, tl_val_buf);
            if (!s.ok() && !s.IsNotFound()) {
                return abort_transaction();
            }
            g_mako_db->Commit(txn);
            record_fast_commit();
            if (s.IsNotFound()) {
                result->status = FAST_MAKO_GET_MISS;
            } else {
                if (ttl_status.IsNotFound()) {
                    redis_cache_put(
                        key_hash, key_ptr, key_len,
                        reinterpret_cast<const uint8_t*>(tl_val_buf.data()), tl_val_buf.size());
                }
                result->status = FAST_MAKO_GET_HIT;
                result->data_ptr = reinterpret_cast<const uint8_t*>(tl_val_buf.data());
                result->data_len = tl_val_buf.size();
            }
            return true;
        }

        bool string_exists = false;
        mako::Status s = redis_table_exists(txn, tl_key_buf, &string_exists);
        if (!s.ok()) {
            return abort_transaction();
        }
        if (!string_exists) {
            g_mako_db->Rollback(txn);
            result->status = FAST_MAKO_FALLBACK;
            return true;
        }

        tl_encoded_val_buf.resize(val_len + mako::EXTRA_BITS_FOR_VALUE);
        if (val_len != 0) {
            std::memcpy(tl_encoded_val_buf.data(), val_ptr, val_len);
        }
        std::memset(
            tl_encoded_val_buf.data() + val_len,
            0,
            mako::EXTRA_BITS_FOR_VALUE);
        auto* time_term = reinterpret_cast<uint32_t*>(
            tl_encoded_val_buf.data()
            + tl_encoded_val_buf.size() - mako::EXTRA_BITS_FOR_VALUE);
        *time_term = 0;
        auto* node = reinterpret_cast<mako::Node*>(
            tl_encoded_val_buf.data()
            + tl_encoded_val_buf.size() - mako::BITS_OF_NODE);
        node->timestamp = 0;
        node->data_size = 0;
        node->data = nullptr;
        s = redis_table_put(txn, tl_key_buf, tl_encoded_val_buf);
        if (s.ok() && !g_redis_bench_skip_ttl) {
            s = redis_table_delete(txn, tl_ttl_key_buf);
        }
        if (!s.ok() && !s.IsNotFound()) {
            return abort_transaction();
        }

        g_mako_db->Commit(txn);
        wait_for_redis_replication();
        redis_cache_put(key_hash, key_ptr, key_len, val_ptr, val_len);
        result->status = FAST_MAKO_SET_OK;
        record_fast_commit();
        return true;
    } catch (abstract_db::abstract_abort_exception&) {
        return abort_transaction();
    } catch (...) {
        return abort_transaction();
    }
}

// Every piece of state one Mako transaction accumulates while the Redis
// executor runs operations inside it. The batch entry point
// (cpp_execute_transaction) fills one of these, runs its whole op list and
// finishes it in a single call; an interactive session (cpp_txn_begin) keeps
// one alive across many cpp_txn_execute calls, so a Lua script's redis.call
// sees everything the calls before it wrote. Sessions are single-threaded and
// short-lived: the storage transaction, the lock guards and the caches all
// belong to the thread that opened them.
struct RedisTxnSession {
    // Set while the object is a live session, cleared before it is freed, so a
    // stale or bogus handle from Rust is caught instead of dereferenced.
    static constexpr uint64_t kSessionMagic = 0x4d414b4f54584e31ull;  // "MAKOTXN1"
    uint64_t magic = kSessionMagic;

    // The storage transaction. NULL is a legal value: mbta_wrapper::new_txn()
    // always returns NULL and keeps the real transaction in thread-local STO
    // state, so `active` rather than `txn` says whether one is open.
    void* txn = nullptr;
    // A storage transaction is open and has neither committed nor rolled back.
    bool active = false;
    // Opened by cpp_txn_begin rather than by the batch path.
    bool interactive = false;
    // At least one non-read-only op has run (or was declared by the batch
    // path), so the commit waits for replication.
    bool has_write = false;
    // Cleared by any storage failure; the transaction can no longer commit.
    bool all_success = true;
    // An abort exception reached the executor, so the caller marks every
    // result failed.
    bool threw = false;

    std::unique_lock<std::shared_mutex> keyspace_exclusive_lock;
    std::shared_lock<std::shared_mutex> keyspace_shared_lock;
    std::unique_lock<std::mutex> single_key_lock;
    std::vector<std::unique_lock<std::mutex>> key_locks;

    // StringWrapper stores a pointer to the string, so encoded values must
    // outlive Commit(). deque keeps references stable as we append values.
    std::deque<std::string> owned_encoded_vals;

    std::unordered_map<std::string, bool> batch_exists;
    std::unordered_map<std::string, std::string> batch_values;
    std::unordered_map<std::string, int64_t> batch_ttls;
    // Storage keys whose physical removal is deferred to the end of the
    // transaction. STO cannot take a remove and a later write of the same key
    // inside one transaction: MassTrans::transDelete sets delete_bit on the
    // TransItem and stores the key in the item's write slot, and a later
    // transPut lands in handlePutFound, whose delete-then-write branch is
    // compiled out (the tree is built with -DREAD_MY_WRITES=OFF), so it only
    // replaces the write slot with the value. At install() the item still says
    // "delete": it sets invalid_bit on the record and calls remove() with the
    // value bytes as the key, so the record stays in the tree permanently
    // invalid and every later access aborts -- the key is stranded for the
    // lifetime of the process. Deletes are therefore recorded here; a later
    // write of the same key cancels the record and overwrites in place, and
    // whatever is left is removed once, just before commit.
    std::unordered_set<std::string> pending_deletes;
    // Every raw write this transaction makes, buffered by storage key, last
    // write wins. The same -DREAD_MY_WRITES=OFF build that forces the deferred
    // deletes above also makes a record unchangeable once this transaction has
    // created it: MassTrans::trans_write allocates the versioned_value with the
    // creation value already in it and only records the KEY in the TransItem
    // write slot (insert_bit), so install() re-publishes that same value and
    // ignores any later transPut; transDelete on it takes the
    // "if (!valid) Sto::abort()" branch because the record still carries
    // invalid_bit; and transGet on it fails validityCheck, because
    // t_read_only_item() is Sto::fresh_item() in this build and a fresh item
    // has no insert_bit. Buffering here means the record is created exactly
    // once, by flush_pending_writes(), with the value the transaction ended up
    // with -- so a second SET, a DEL, an APPEND or a read of a key the same
    // transaction created all behave. A key is never in both pending_writes
    // and pending_deletes: put_raw cancels the delete and delete_raw_if_exists
    // drops the buffered write.
    std::unordered_map<std::string, std::string> pending_writes;
    std::unordered_map<std::string, std::unordered_set<std::string>> staged_sets;
    std::unordered_set<std::string> staged_sets_loaded;
    std::unordered_set<std::string> dirty_sets;
    std::unordered_map<std::string, std::vector<std::string>> staged_lists;
    std::unordered_set<std::string> staged_lists_loaded;
    std::unordered_set<std::string> dirty_lists;
    std::unordered_map<std::string, std::map<std::string, double>> staged_zsets;
    std::unordered_set<std::string> staged_zsets_loaded;
    std::unordered_set<std::string> dirty_zsets;
    std::unordered_map<uint32_t, bool> group_can_write;
    // Interactive sessions only: the Redis-visible names whose cached string
    // value this transaction changed, collected just before the flush and
    // dropped from the cache once the commit succeeds. The batch path still
    // has its TxnRequest at that point and invalidates straight from it.
    std::vector<std::string> cache_invalidations;

    void release_locks() {
        key_locks.clear();
        single_key_lock = std::unique_lock<std::mutex>();
        keyspace_shared_lock = std::shared_lock<std::shared_mutex>();
        keyspace_exclusive_lock = std::unique_lock<std::shared_mutex>();
    }
};

// Opens the storage transaction of a session whose locks are already held.
static void begin_session_txn(RedisTxnSession& session) {
    // Reset arena for this transaction
    if (tl_arena) {
        tl_arena->reset();
    }

    // Begin a single database transaction for all operations
    // NOTE: mbta_wrapper::new_txn() always returns NULL - it uses thread-local TThread::txn state
    // The actual transaction is started via Sto::start_transaction() internally
    // DO NOT check for NULL - that's expected behavior!
    session.txn = g_mako_db->BeginTransaction();
    session.active = true;
}

// The executor body. `finish` runs the end-of-transaction tail (staged
// collections, the two flushes, commit or rollback); `commit` says which of the
// two the tail should do. execute_ops() and finish_session() below are the two
// ways in: they are one function because the tail needs the same hundred helper
// lambdas the op loop does, and those close over the session's caches.
static bool execute_ops_impl(
    RedisTxnSession& session,
    const TxnRequest* request,
    TxnResponse* response,
    bool finish,
    bool commit);

// Runs one op list inside an open session, leaving the transaction open.
static bool execute_ops(
    RedisTxnSession& session, const TxnRequest* request, TxnResponse* response) {
    return execute_ops_impl(session, request, response, false, false);
}

// Ends an open session: writes out the staged collections and the buffered
// writes and deletes, then commits or rolls back. Returns whether the storage
// commit succeeded (an OCC abort, or `commit == false`, returns false).
static bool finish_session(RedisTxnSession& session, bool commit) {
    if (!session.active) {
        session.release_locks();
        return false;
    }
    const TxnRequest empty_request{0, nullptr};
    TxnResponse sink{false, 0, nullptr};
    execute_ops_impl(session, &empty_request, &sink, true, commit);
    session.release_locks();
    return sink.transaction_success;
}

// Generic executor for single commands and MULTI/EXEC batches.
bool execute_transaction(const TxnRequest* request, TxnResponse* response) {
    ensure_thread_info();

    if (!makocon_ffi::allocate_response(request, response)) {
        return false;
    }

    const bool has_write = redis_request_has_write(request);
    if (has_write && !redis_can_write_here()) {
        response->transaction_success = false;
        for (size_t i = 0; i < response->num_results; ++i) {
            response->results[i].success = false;
        }
        g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    RedisTxnSession session;
    session.has_write = has_write;
    if (g_redis_single_worker_mode) {
        // One Rust protocol worker already serializes all FFI executor calls.
    } else if (redis_request_has_flushdb(request)) {
        session.keyspace_exclusive_lock = std::unique_lock<std::shared_mutex>(g_redis_keyspace_mutex);
        // Generic operations take the keyspace lock before any stripe. Once
        // exclusive ownership is established, acquiring every stripe cannot
        // deadlock and excludes specialized GET/SET until the scan completes.
        session.key_locks.reserve(kRedisTxnLockStripes);
        for (size_t stripe = 0; stripe < kRedisTxnLockStripes; ++stripe) {
            session.key_locks.emplace_back(g_redis_txn_key_mutexes[stripe]);
        }
    } else {
        session.keyspace_shared_lock = std::shared_lock<std::shared_mutex>(g_redis_keyspace_mutex);
        if (request != nullptr
            && request->ops != nullptr
            && request->num_ops == 1
            && !redis_op_is_read_only(request->ops[0])
            && redis_op_uses_only_primary_lock_key(request->ops[0])) {
            const TxnOperation& op = request->ops[0];
            const size_t stripe = redis_lock_hash(op.key_ptr, op.key_len) % kRedisTxnLockStripes;
            session.single_key_lock = std::unique_lock<std::mutex>(g_redis_txn_key_mutexes[stripe]);
        } else if (has_write) {
            const std::vector<size_t> stripes = redis_request_lock_stripes(request);
            session.key_locks.reserve(stripes.size());
            for (size_t stripe : stripes) {
                session.key_locks.emplace_back(g_redis_txn_key_mutexes[stripe]);
            }
        }
    }

    if (request->num_ops == 1 && request->ops != nullptr && request->ops[0].op == TXN_OP_FLUSHDB) {
        const bool ok = execute_flushdb_chunked(
            redis_flush_db_filter(request->ops[0].val_ptr, request->ops[0].val_len));
        response->transaction_success = ok;
        response->results[0].success = ok;
        response->results[0].value_present = ok;
        if (ok) {
            redis_cache_clear();
            g_mako_txn_commits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    begin_session_txn(session);
    execute_ops(session, request, response);
    if (!session.active) {
        // An op settled the transaction on its own (the chunked DBSIZE path)
        // and already filled in the response.
        return true;
    }

    const bool committed = finish_session(session, true);
    response->transaction_success = committed;
    if (committed) {
        if (has_write) {
            redis_cache_invalidate_committed_writes(request);
        }
    } else if (session.threw) {
        // Mark all results as failed on abort
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    }
    return true;
}

static bool execute_ops_impl(
    RedisTxnSession& session,
    const TxnRequest* request,
    TxnResponse* response,
    bool finish,
    bool commit) {
    // Bound to the session so the op bodies below read exactly as they did when
    // all of this was one function's locals.
    void* txn = session.txn;
    bool& all_success = session.all_success;
    auto& owned_encoded_vals = session.owned_encoded_vals;
    auto& batch_exists = session.batch_exists;
    auto& batch_values = session.batch_values;
    auto& batch_ttls = session.batch_ttls;
    // Deferred removals and buffered writes; see RedisTxnSession for why both
    // buffers exist and what breaks without them.
    auto& pending_deletes = session.pending_deletes;
    auto& pending_writes = session.pending_writes;
    auto& staged_sets = session.staged_sets;
    auto& staged_sets_loaded = session.staged_sets_loaded;
    auto& dirty_sets = session.dirty_sets;
    auto& staged_lists = session.staged_lists;
    auto& staged_lists_loaded = session.staged_lists_loaded;
    auto& dirty_lists = session.dirty_lists;
    auto& staged_zsets = session.staged_zsets;
    auto& staged_zsets_loaded = session.staged_zsets_loaded;
    auto& dirty_zsets = session.dirty_zsets;
    auto& group_can_write = session.group_can_write;

    auto make_prefixed_key = [](const TxnOperation& op) {
        std::string key;
        key.reserve(sizeof("table_key_") - 1 + op.key_len);
        key.append("table_key_", sizeof("table_key_") - 1);
        key.append(reinterpret_cast<const char*>(op.key_ptr), op.key_len);
        return key;
    };

    auto make_ttl_meta_key = [](const std::string& user_key) {
        std::string meta_key;
        meta_key.reserve(sizeof("\x01TTL:") - 1 + user_key.size());
        meta_key.append("\x01TTL:", sizeof("\x01TTL:") - 1);
        meta_key.append(user_key);
        return meta_key;
    };

    auto make_set_member_key = [](const std::string& set_key, const std::string& member) {
        std::string key;
        key.reserve(sizeof("\x01S:") - 1 + 8 + set_key.size() + member.size());
        key.append("\x01S:", sizeof("\x01S:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            key.push_back(static_cast<char>((static_cast<uint64_t>(set_key.size()) >> shift) & 0xff));
        }
        key.append(set_key);
        key.append(member);
        return key;
    };

    auto make_set_member_prefix = [](const std::string& set_key) {
        std::string prefix;
        prefix.reserve(sizeof("\x01S:") - 1 + 8 + set_key.size());
        prefix.append("\x01S:", sizeof("\x01S:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            prefix.push_back(static_cast<char>((static_cast<uint64_t>(set_key.size()) >> shift) & 0xff));
        }
        prefix.append(set_key);
        return prefix;
    };

    auto make_set_meta_key = [](const std::string& set_key) {
        std::string key;
        key.reserve(sizeof("\x01S#:") - 1 + 8 + set_key.size());
        key.append("\x01S#:", sizeof("\x01S#:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            key.push_back(static_cast<char>((static_cast<uint64_t>(set_key.size()) >> shift) & 0xff));
        }
        key.append(set_key);
        return key;
    };

    auto make_hash_field_prefix = [](const std::string& hash_key) {
        std::string prefix;
        prefix.reserve(sizeof("\x01H:") - 1 + 8 + hash_key.size());
        prefix.append("\x01H:", sizeof("\x01H:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            prefix.push_back(static_cast<char>((static_cast<uint64_t>(hash_key.size()) >> shift) & 0xff));
        }
        prefix.append(hash_key);
        return prefix;
    };

    auto make_hash_field_key = [&](const std::string& hash_key, const std::string& field) {
        std::string key = make_hash_field_prefix(hash_key);
        key.append(field);
        return key;
    };

    auto make_hash_meta_key = [](const std::string& hash_key) {
        std::string key;
        key.reserve(sizeof("\x01H#:") - 1 + 8 + hash_key.size());
        key.append("\x01H#:", sizeof("\x01H#:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            key.push_back(static_cast<char>((static_cast<uint64_t>(hash_key.size()) >> shift) & 0xff));
        }
        key.append(hash_key);
        return key;
    };

    // Per-field expiration (HEXPIRE family): one side key beside the field key
    // holding the absolute Unix millisecond time as decimal text. The "\x01HX:"
    // prefix sorts outside the "\x01H:" range a hash field scan walks, so
    // collect_hash_entries never sees these records.
    auto make_hash_field_ttl_prefix = [](const std::string& hash_key) {
        std::string prefix;
        prefix.reserve(sizeof("\x01HX:") - 1 + 8 + hash_key.size());
        prefix.append("\x01HX:", sizeof("\x01HX:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            prefix.push_back(static_cast<char>((static_cast<uint64_t>(hash_key.size()) >> shift) & 0xff));
        }
        prefix.append(hash_key);
        return prefix;
    };

    auto make_hash_field_ttl_key = [&](const std::string& hash_key, const std::string& field) {
        std::string key = make_hash_field_ttl_prefix(hash_key);
        key.append(field);
        return key;
    };

    auto append_u64_be = [](std::string& out, uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    };

    auto make_list_element_prefix = [&](const std::string& list_key) {
        std::string prefix;
        prefix.reserve(sizeof("\x01L:") - 1 + 8 + list_key.size());
        prefix.append("\x01L:", sizeof("\x01L:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            prefix.push_back(static_cast<char>((static_cast<uint64_t>(list_key.size()) >> shift) & 0xff));
        }
        prefix.append(list_key);
        return prefix;
    };

    auto make_list_element_key = [&](const std::string& list_key, int64_t index) {
        std::string key = make_list_element_prefix(list_key);
        append_u64_be(key, static_cast<uint64_t>(index) ^ (1ULL << 63));
        return key;
    };

    auto make_list_meta_key = [](const std::string& list_key) {
        std::string key;
        key.reserve(sizeof("\x01L#:") - 1 + 8 + list_key.size());
        key.append("\x01L#:", sizeof("\x01L#:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            key.push_back(static_cast<char>((static_cast<uint64_t>(list_key.size()) >> shift) & 0xff));
        }
        key.append(list_key);
        return key;
    };

    auto make_zset_member_prefix = [](const std::string& zset_key) {
        std::string prefix;
        prefix.reserve(sizeof("\x01Z:") - 1 + 8 + zset_key.size());
        prefix.append("\x01Z:", sizeof("\x01Z:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            prefix.push_back(static_cast<char>((static_cast<uint64_t>(zset_key.size()) >> shift) & 0xff));
        }
        prefix.append(zset_key);
        return prefix;
    };

    auto make_zset_member_key = [&](const std::string& zset_key, const std::string& member) {
        std::string key = make_zset_member_prefix(zset_key);
        key.append(member);
        return key;
    };

    auto make_zset_score_prefix = [](const std::string& zset_key) {
        std::string prefix;
        prefix.reserve(sizeof("\x01ZS:") - 1 + 8 + zset_key.size());
        prefix.append("\x01ZS:", sizeof("\x01ZS:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            prefix.push_back(static_cast<char>((static_cast<uint64_t>(zset_key.size()) >> shift) & 0xff));
        }
        prefix.append(zset_key);
        return prefix;
    };

    auto encode_zset_score = [&](double score) {
        uint64_t raw = std::bit_cast<uint64_t>(score);
        uint64_t encoded = (raw & (1ULL << 63)) != 0 ? ~raw : (raw ^ (1ULL << 63));
        std::string out;
        append_u64_be(out, encoded);
        return out;
    };

    auto make_zset_score_key = [&](const std::string& zset_key, double score, const std::string& member) {
        std::string key = make_zset_score_prefix(zset_key);
        key.append(encode_zset_score(score));
        key.append(member);
        return key;
    };

    auto make_zset_meta_key = [](const std::string& zset_key) {
        std::string key;
        key.reserve(sizeof("\x01Z#:") - 1 + 8 + zset_key.size());
        key.append("\x01Z#:", sizeof("\x01Z#:") - 1);
        for (int shift = 0; shift < 64; shift += 8) {
            key.push_back(static_cast<char>((static_cast<uint64_t>(zset_key.size()) >> shift) & 0xff));
        }
        key.append(zset_key);
        return key;
    };

    auto copy_result_value = [](TxnOpResult& result, const std::string& value) {
        result.value_present = true;
        if (value.empty()) {
            return true;
        }
        result.data_len = value.size();
        result.data_ptr = static_cast<uint8_t*>(std::malloc(result.data_len));
        if (!result.data_ptr) {
            result.success = false;
            return false;
        }
        std::memcpy(result.data_ptr, value.data(), result.data_len);
        return true;
    };

    auto append_u64_le = [](std::string& out, uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    };

    auto read_u64_le = [](const uint8_t* data, size_t len, size_t& pos, uint64_t& out) {
        if (len - pos < 8) {
            return false;
        }
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            value |= static_cast<uint64_t>(data[pos++]) << shift;
        }
        out = value;
        return true;
    };

    auto unpack_bytes_list = [&](const uint8_t* data, size_t len, std::vector<std::string>& out) {
        out.clear();
        if (data == nullptr && len != 0) {
            return false;
        }
        size_t pos = 0;
        uint64_t count = 0;
        if (!read_u64_le(data, len, pos, count)) {
            return false;
        }
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t item_len = 0;
            if (!read_u64_le(data, len, pos, item_len)
                || item_len > static_cast<uint64_t>(len - pos)) {
                return false;
            }
            out.emplace_back(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(item_len));
            pos += static_cast<size_t>(item_len);
        }
        return pos == len;
    };

    auto append_i64_le = [](std::string& out, int64_t value) {
        uint64_t raw = static_cast<uint64_t>(value);
        for (int shift = 0; shift < 64; shift += 8) {
            out.push_back(static_cast<char>((raw >> shift) & 0xff));
        }
    };

    auto read_i64_le_from_string = [](const std::string& input, size_t& pos, int64_t& out) {
        if (input.size() - pos < 8) {
            return false;
        }
        uint64_t raw = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            raw |= static_cast<uint64_t>(static_cast<unsigned char>(input[pos++])) << shift;
        }
        out = static_cast<int64_t>(raw);
        return true;
    };

    auto pack_list_meta = [&](int64_t head, int64_t tail) {
        std::string payload;
        append_i64_le(payload, head);
        append_i64_le(payload, tail);
        return payload;
    };

    auto pack_bytes_list = [&](const std::vector<std::string>& items) {
        std::string payload;
        append_u64_le(payload, items.size());
        for (const auto& item : items) {
            append_u64_le(payload, item.size());
            payload.append(item);
        }
        return payload;
    };

    auto parse_int64 = [](const std::string& input, int64_t& out) {
        if (input.empty() || std::isspace(static_cast<unsigned char>(input.front()))) {
            return false;
        }
        errno = 0;
        char* end = nullptr;
        long long parsed = std::strtoll(input.c_str(), &end, 10);
        if (end == input.c_str() || end != input.c_str() + input.size() || errno == ERANGE) {
            return false;
        }
        out = static_cast<int64_t>(parsed);
        return true;
    };

    // ----- Stream storage keys -----
    //
    // Every family is "<tag>" + u64le(len(stream key)) + stream key, so one
    // stream's records are a contiguous prefix range and no key can be spelled
    // by another. Entry and PEL records then carry the 128-bit ID big-endian,
    // which makes lexicographic order the same as ID order.
    auto make_stream_family_prefix = [](const char* tag, size_t tag_len, const std::string& stream_key) {
        std::string prefix;
        prefix.reserve(tag_len + 8 + stream_key.size());
        prefix.append(tag, tag_len);
        stream_put_u64_le(prefix, static_cast<uint64_t>(stream_key.size()));
        prefix.append(stream_key);
        return prefix;
    };

    auto make_stream_meta_key = [&](const std::string& stream_key) {
        return make_stream_family_prefix("\x01X#:", sizeof("\x01X#:") - 1, stream_key);
    };

    auto make_stream_entry_prefix = [&](const std::string& stream_key) {
        return make_stream_family_prefix("\x01X:", sizeof("\x01X:") - 1, stream_key);
    };

    auto make_stream_entry_key = [&](const std::string& stream_key, const RedisStreamId& id) {
        std::string key = make_stream_entry_prefix(stream_key);
        stream_put_u64_be(key, id.ms);
        stream_put_u64_be(key, id.seq);
        return key;
    };

    auto make_stream_group_prefix = [&](const std::string& stream_key) {
        return make_stream_family_prefix("\x01XG:", sizeof("\x01XG:") - 1, stream_key);
    };

    auto make_stream_group_key = [&](const std::string& stream_key, const std::string& group) {
        std::string key = make_stream_group_prefix(stream_key);
        stream_put_u64_le(key, static_cast<uint64_t>(group.size()));
        key.append(group);
        return key;
    };

    auto make_stream_consumer_prefix = [&](const std::string& stream_key, const std::string& group) {
        std::string key = make_stream_family_prefix("\x01XC:", sizeof("\x01XC:") - 1, stream_key);
        stream_put_u64_le(key, static_cast<uint64_t>(group.size()));
        key.append(group);
        return key;
    };

    auto make_stream_consumer_key = [&](const std::string& stream_key,
                                        const std::string& group,
                                        const std::string& consumer) {
        std::string key = make_stream_consumer_prefix(stream_key, group);
        key.append(consumer);
        return key;
    };

    auto make_stream_pel_prefix = [&](const std::string& stream_key, const std::string& group) {
        std::string key = make_stream_family_prefix("\x01XP:", sizeof("\x01XP:") - 1, stream_key);
        stream_put_u64_le(key, static_cast<uint64_t>(group.size()));
        key.append(group);
        return key;
    };

    auto make_stream_pel_key = [&](const std::string& stream_key,
                                   const std::string& group,
                                   const RedisStreamId& id) {
        std::string key = make_stream_pel_prefix(stream_key, group);
        stream_put_u64_be(key, id.ms);
        stream_put_u64_be(key, id.seq);
        return key;
    };

    // The ID a 16-byte big-endian suffix spells, used to read an entry or PEL
    // key back after a scan.
    auto stream_id_from_key_suffix = [](const std::string& storage_key, size_t prefix_len, RedisStreamId& id) {
        if (storage_key.size() != prefix_len + 16) {
            return false;
        }
        uint64_t ms = 0;
        uint64_t seq = 0;
        for (size_t i = 0; i < 8; ++i) {
            ms = (ms << 8) | static_cast<unsigned char>(storage_key[prefix_len + i]);
            seq = (seq << 8) | static_cast<unsigned char>(storage_key[prefix_len + 8 + i]);
        }
        id.ms = ms;
        id.seq = seq;
        return true;
    };

    auto read_raw = [&](void* txn, const std::string& key, std::string& value, bool& exists) {
        value.clear();
        // The write buffer is this transaction's own view of storage, so it
        // answers first: storage still holds the previous value (or nothing at
        // all) until flush_pending_writes() runs just before commit.
        if (!pending_writes.empty()) {
            auto pending_it = pending_writes.find(key);
            if (pending_it != pending_writes.end()) {
                value = pending_it->second;
                exists = true;
                return mako::Status::OK();
            }
        }
        // A key whose removal is pending is already gone as far as this
        // transaction is concerned; the record is still in storage only
        // because the physical remove waits for commit.
        if (!pending_deletes.empty() && pending_deletes.count(key) != 0) {
            exists = false;
            return mako::Status::OK();
        }
        mako::Status s = redis_table_get(txn, key, value);
        if (s.ok()) {
            exists = true;
            return s;
        }
        if (s.IsNotFound()) {
            exists = false;
            return mako::Status::OK();
        }
        return s;
    };

    auto read_raw_exists = [&](void* txn, const std::string& key, bool& exists) {
        std::string ignored;
        return read_raw(txn, key, ignored, exists);
    };

    // Records the removal of a storage key instead of issuing it. The record
    // is cancelled by a later put_raw of the same key (which then overwrites
    // the row in place) and is turned into a real tx_remove by
    // flush_pending_deletes() at the end of the transaction. See the comment on
    // pending_deletes for why the remove cannot be issued here.
    auto delete_raw_if_exists = [&](void* txn, const std::string& key) {
        (void)txn;
        // A buffered write is simply dropped. If the key only ever existed
        // inside this transaction, dropping it is the whole removal: nothing
        // was written to storage, so the pending delete below finds nothing
        // and issues no tx_remove. If the key existed before the transaction,
        // the pending delete is what removes it.
        if (!pending_writes.empty()) {
            pending_writes.erase(key);
        }
        // Recorded unconditionally, including for a key this transaction has
        // already marked absent: several callers (write_set_cardinality and
        // the other meta writers) set batch_exists[key] = false before asking
        // for the removal, so trusting that cache here would drop the removal
        // altogether. The flush reads the key once and skips what is not
        // there, which costs the same as the existence check this helper used
        // to do.
        pending_deletes.insert(key);
        batch_exists[key] = false;
        batch_values.erase(key);
        return mako::Status::OK();
    };

    auto now_unix_ms = []() {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    auto read_ttl_meta = [&](void* txn, const std::string& user_key, int64_t& expire_at_ms, bool& exists) {
        auto batch_ttl_it = batch_ttls.find(user_key);
        if (batch_ttl_it != batch_ttls.end()) {
            expire_at_ms = batch_ttl_it->second;
            exists = expire_at_ms >= 0;
            return mako::Status::OK();
        }
        const std::string meta_key = make_ttl_meta_key(user_key);
        std::string ttl_value;
        bool ttl_exists = false;
        mako::Status s = read_raw(txn, meta_key, ttl_value, ttl_exists);
        if (!s.ok() || !ttl_exists) {
            return s;
        }
        exists = true;
        if (!parse_int64(ttl_value, expire_at_ms)) {
            exists = false;
            return mako::Status::OK();
        }
        return mako::Status::OK();
    };

    auto expire_if_needed = [&](void* txn, const std::string& user_key, const std::string& storage_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, user_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists) {
            return mako::Status::OK();
        }
        if (expire_at_ms > now_unix_ms()) {
            return mako::Status::OK();
        }
        s = delete_raw_if_exists(txn, storage_key);
        if (!s.ok()) {
            return s;
        }
        s = delete_raw_if_exists(txn, make_ttl_meta_key(user_key));
        if (s.ok()) {
            batch_exists[storage_key] = false;
            batch_values.erase(storage_key);
            batch_ttls[user_key] = -1;
        }
        return s;
    };

    auto read_current = [&](void* txn, const std::string& user_key, const std::string& key, std::string& value, bool& exists) {
        auto batch_it = batch_exists.find(key);
        if (batch_it != batch_exists.end()) {
            exists = batch_it->second;
            if (!exists) {
                value.clear();
                return mako::Status::OK();
            }
            auto val_it = batch_values.find(key);
            if (val_it != batch_values.end()) {
                value = val_it->second;
                return mako::Status::OK();
            }
        }

        mako::Status s = expire_if_needed(txn, user_key, key);
        if (!s.ok()) {
            return s;
        }
        batch_it = batch_exists.find(key);
        if (batch_it != batch_exists.end() && !batch_it->second) {
            exists = false;
            value.clear();
            return mako::Status::OK();
        }
        return read_raw(txn, key, value, exists);
    };

    // Buffers a raw write instead of issuing it; last write to a key wins.
    // Nothing reaches storage until flush_pending_writes() runs at the end of
    // the transaction, which is what lets the same transaction write a key
    // twice, or write and then delete it (see pending_writes).
    auto put_raw = [&](void* txn, const std::string& key, const std::string& raw_value) {
        (void)txn;
        // Writing a key that was deleted earlier in this transaction cancels
        // the pending removal: the row is overwritten in place, which is the
        // only shape the storage layer supports (see pending_deletes).
        if (!pending_deletes.empty()) {
            pending_deletes.erase(key);
        }
        pending_writes[key] = raw_value;
        batch_exists[key] = true;
        // The buffered value is the transaction's own view of the key, so the
        // read caches carry it too: read_current and read_internal_current
        // answer from batch_values without consulting storage, and the scan
        // collectors merge batch_exists/batch_values over what they scanned.
        batch_values[key] = raw_value;
        return mako::Status::OK();
    };

    // Issues the buffered writes. Called once, after every op and every staged
    // collection has been written, so each storage key is created or updated
    // exactly once with the value the transaction ended up with.
    auto flush_pending_writes = [&](void* txn) {
        for (const auto& [storage_key, raw_value] : pending_writes) {
            owned_encoded_vals.push_back(mako::Encode(raw_value));
            mako::Status s = redis_table_put(txn, storage_key, owned_encoded_vals.back());
            if (!s.ok()) {
                return s;
            }
        }
        pending_writes.clear();
        return mako::Status::OK();
    };

    // Issues the removals recorded by delete_raw_if_exists. Called once, after
    // every op and every staged collection has been written, so a key that was
    // deleted and written again in the same transaction is never removed.
    auto flush_pending_deletes = [&](void* txn) {
        for (const auto& storage_key : pending_deletes) {
            mako::Status s = redis_table_delete(txn, storage_key);
            if (!s.ok() && !s.IsNotFound()) {
                return s;
            }
        }
        pending_deletes.clear();
        return mako::Status::OK();
    };

    auto write_ttl_meta = [&](void* txn, const std::string& user_key, int64_t expire_at_ms) {
        if (expire_at_ms < 0) {
            return mako::Status::OK();
        }
        std::string meta_key = make_ttl_meta_key(user_key);
        mako::Status s = put_raw(txn, meta_key, std::to_string(expire_at_ms));
        if (s.ok()) {
            batch_ttls[user_key] = expire_at_ms;
        }
        return s;
    };

    auto clear_ttl_meta = [&](void* txn, const std::string& user_key) {
        mako::Status s = delete_raw_if_exists(txn, make_ttl_meta_key(user_key));
        if (s.ok()) {
            batch_ttls[user_key] = -1;
        }
        return s;
    };

    auto storage_prefix_upper = [](const std::string& prefix) {
        std::optional<std::string> upper = prefix;
        for (size_t i = upper->size(); i > 0; --i) {
            unsigned char c = static_cast<unsigned char>((*upper)[i - 1]);
            if (c != 0xff) {
                (*upper)[i - 1] = static_cast<char>(c + 1);
                upper->resize(i);
                return upper;
            }
        }
        return std::optional<std::string>{};
    };

    // DBSIZE over the whole of database 0 walks the keyspace in its own chunked
    // transactions, so it cannot run inside an interactive session: the session
    // has to stay open for the ops after it. A session answers DBSIZE from the
    // general TXN_OP_SCAN branch instead, inside the session's transaction.
    if (!session.interactive
        && request->num_ops == 1 && request->ops != nullptr
        && request->ops[0].op == TXN_OP_SCAN
        && (request->ops[0].flags & TXN_FLAG_SCAN_COUNT_ONLY) != 0
        && request->ops[0].key_len == 0
        && request->ops[0].val_len == 0) {
        g_mako_db->Rollback(txn);
        session.active = false;

        constexpr size_t kDbSizeChunkSize = 1024;
        int64_t visible_count = 0;
        bool ok = true;
        std::string scan_start = "table_key_";
        const std::string storage_prefix = "table_key_";
        std::optional<std::string> scan_end = storage_prefix_upper(storage_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class DbSizeChunkScanCallback : public oi_scan_callback {
        public:
            DbSizeChunkScanCallback(
                size_t limit,
                int64_t& visible_count,
                std::vector<std::string>& expired_user_keys,
                std::string& last_storage_key,
                const std::function<bool(const std::string&)>& is_expired)
                : limit_(limit),
                  visible_count_(visible_count),
                  expired_user_keys_(expired_user_keys),
                  last_storage_key_(last_storage_key),
                  is_expired_(is_expired) {}

            bool invoke(const char* keyp, size_t keylen, const std::string&) override {
                std::string storage_key(keyp, keylen);
                constexpr std::string_view kStoragePrefix = "table_key_";
                if (storage_key.rfind(kStoragePrefix, 0) != 0) {
                    return true;
                }

                last_storage_key_ = storage_key;
                std::string user_key = storage_key.substr(kStoragePrefix.size());
                // This chunked path only ever answers DBSIZE for database 0
                // (its caller requires an empty scan prefix), so a key that
                // belongs to another logical database is not counted, exactly
                // as the general TXN_OP_SCAN path skips it.
                if (!user_key.empty()
                    && (static_cast<unsigned char>(user_key[0]) == 0x01
                        || static_cast<unsigned char>(user_key[0]) == kRedisDbMarker)) {
                    return ++seen_ < limit_;
                }

                if (is_expired_(user_key)) {
                    expired_user_keys_.push_back(std::move(user_key));
                } else {
                    ++visible_count_;
                }
                return ++seen_ < limit_;
            }

            size_t seen() const {
                return seen_;
            }

        private:
            size_t limit_;
            size_t seen_ = 0;
            int64_t& visible_count_;
            std::vector<std::string>& expired_user_keys_;
            std::string& last_storage_key_;
            const std::function<bool(const std::string&)>& is_expired_;
        };

        for (;;) {
            if (tl_arena) {
                tl_arena->reset();
            }

            void* scan_txn = g_mako_db->BeginTransaction();
            std::vector<std::string> expired_user_keys;
            std::string last_storage_key;

            try {
                std::function<bool(const std::string&)> is_expired =
                    [&](const std::string& scanned_user_key) {
                        int64_t expire_at_ms = 0;
                        bool ttl_exists = false;
                        mako::Status ttl_status =
                            read_ttl_meta(scan_txn, scanned_user_key, expire_at_ms, ttl_exists);
                        if (!ttl_status.ok() || !ttl_exists) {
                            return false;
                        }
                        return expire_at_ms <= now_unix_ms();
                    };

                DbSizeChunkScanCallback callback(
                    kDbSizeChunkSize,
                    visible_count,
                    expired_user_keys,
                    last_storage_key,
                    is_expired);
                tx_scan(g_table, scan_txn, scan_start, scan_end_ptr, callback, tl_arena);

                for (const auto& expired_user_key : expired_user_keys) {
                    mako::Status s = delete_raw_if_exists(scan_txn, "table_key_" + expired_user_key);
                    if (s.ok()) {
                        s = clear_ttl_meta(scan_txn, expired_user_key);
                    }
                    if (!s.ok()) {
                        ok = false;
                        break;
                    }
                }

                if (!ok) {
                    pending_writes.clear();
                    pending_deletes.clear();
                    g_mako_db->Rollback(scan_txn);
                    break;
                }

                // This chunk runs in its own transaction, so its buffered
                // writes and deferred removals are issued against that
                // transaction, not the one execute_transaction opened.
                if (expired_user_keys.empty()) {
                    pending_writes.clear();
                    pending_deletes.clear();
                    g_mako_db->Rollback(scan_txn);
                } else {
                    mako::Status flush_status = flush_pending_writes(scan_txn);
                    if (flush_status.ok()) {
                        flush_status = flush_pending_deletes(scan_txn);
                    }
                    if (!flush_status.ok()) {
                        pending_writes.clear();
                        pending_deletes.clear();
                        g_mako_db->Rollback(scan_txn);
                        ok = false;
                        break;
                    }
                    g_mako_db->Commit(scan_txn);
                    wait_for_redis_replication();
                }

                if (callback.seen() < kDbSizeChunkSize || last_storage_key.empty()) {
                    break;
                }
                scan_start = last_storage_key;
                scan_start.push_back('\0');
            } catch (abstract_db::abstract_abort_exception&) {
                pending_writes.clear();
                pending_deletes.clear();
                g_mako_db->Rollback(scan_txn);
                ok = false;
                break;
            } catch (...) {
                pending_writes.clear();
                pending_deletes.clear();
                g_mako_db->Rollback(scan_txn);
                ok = false;
                break;
            }
        }

        response->transaction_success = ok;
        response->results[0].success = ok;
        response->results[0].value_present = ok;
        if (ok) {
            response->results[0].int_value = visible_count;
            g_mako_txn_commits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    auto set_storage_key = [&](const std::string& set_key, const std::string& member) {
        return make_set_member_key(set_key, member);
    };

    auto set_meta_storage_key = [&](const std::string& set_key) {
        return make_set_meta_key(set_key);
    };

    auto hash_field_storage_key = [&](const std::string& hash_key, const std::string& field) {
        return make_hash_field_key(hash_key, field);
    };

    auto hash_meta_storage_key = [&](const std::string& hash_key) {
        return make_hash_meta_key(hash_key);
    };

    auto zset_member_storage_key = [&](const std::string& zset_key, const std::string& member) {
        return make_zset_member_key(zset_key, member);
    };

    auto zset_score_storage_key = [&](const std::string& zset_key, double score, const std::string& member) {
        return make_zset_score_key(zset_key, score, member);
    };

    auto zset_meta_storage_key = [&](const std::string& zset_key) {
        return make_zset_meta_key(zset_key);
    };

    auto read_set_cardinality = [&](void* txn, const std::string& set_key, int64_t& count) {
        auto staged_it = staged_sets.find(set_key);
        if (staged_it != staged_sets.end()) {
            count = static_cast<int64_t>(staged_it->second.size());
            return mako::Status::OK();
        }
        std::string meta_key = set_meta_storage_key(set_key);
        auto batch_it = batch_exists.find(meta_key);
        if (batch_it != batch_exists.end()) {
            if (!batch_it->second) {
                count = 0;
                return mako::Status::OK();
            }
            auto value_it = batch_values.find(meta_key);
            if (value_it != batch_values.end() && parse_int64(value_it->second, count)) {
                return mako::Status::OK();
            }
        }
        std::string value;
        bool exists = false;
        mako::Status s = read_raw(txn, meta_key, value, exists);
        if (!s.ok()) {
            return s;
        }
        if (!exists) {
            count = 0;
            return mako::Status::OK();
        }
        if (!parse_int64(value, count) || count < 0) {
            count = 0;
        }
        return mako::Status::OK();
    };

    auto write_set_cardinality = [&](void* txn, const std::string& set_key, int64_t count) {
        std::string meta_key = set_meta_storage_key(set_key);
        if (count <= 0) {
            batch_exists[meta_key] = false;
            batch_values.erase(meta_key);
            return delete_raw_if_exists(txn, meta_key);
        }
        mako::Status s = put_raw(txn, meta_key, std::to_string(count));
        if (s.ok()) {
            batch_exists[meta_key] = true;
            batch_values[meta_key] = std::to_string(count);
        }
        return s;
    };

    auto read_hash_cardinality = [&](void* txn, const std::string& hash_key, int64_t& count) {
        std::string meta_key = hash_meta_storage_key(hash_key);
        auto batch_it = batch_exists.find(meta_key);
        if (batch_it != batch_exists.end()) {
            if (!batch_it->second) {
                count = 0;
                return mako::Status::OK();
            }
            auto value_it = batch_values.find(meta_key);
            if (value_it != batch_values.end() && parse_int64(value_it->second, count)) {
                return mako::Status::OK();
            }
        }
        std::string value;
        bool exists = false;
        mako::Status s = read_raw(txn, meta_key, value, exists);
        if (!s.ok()) {
            return s;
        }
        if (!exists) {
            count = 0;
            return mako::Status::OK();
        }
        if (!parse_int64(value, count) || count < 0) {
            count = 0;
        }
        return mako::Status::OK();
    };

    auto write_hash_cardinality = [&](void* txn, const std::string& hash_key, int64_t count) {
        std::string meta_key = hash_meta_storage_key(hash_key);
        if (count <= 0) {
            batch_exists[meta_key] = false;
            batch_values.erase(meta_key);
            return delete_raw_if_exists(txn, meta_key);
        }
        std::string payload = std::to_string(count);
        mako::Status s = put_raw(txn, meta_key, payload);
        if (s.ok()) {
            batch_exists[meta_key] = true;
            batch_values[meta_key] = payload;
        }
        return s;
    };

    auto read_internal_current = [&](void* txn, const std::string& key, std::string& value, bool& exists) {
        auto batch_it = batch_exists.find(key);
        if (batch_it != batch_exists.end()) {
            exists = batch_it->second;
            if (!exists) {
                value.clear();
                return mako::Status::OK();
            }
            auto value_it = batch_values.find(key);
            if (value_it != batch_values.end()) {
                value = value_it->second;
                return mako::Status::OK();
            }
        }
        return read_raw(txn, key, value, exists);
    };

    auto parse_zset_score_value = [](const std::string& input, double& out) {
        if (input.empty()) {
            return false;
        }
        errno = 0;
        char* end = nullptr;
        double parsed = std::strtod(input.c_str(), &end);
        if (end == input.c_str() || *end != '\0' || std::isnan(parsed)) {
            return false;
        }
        out = parsed;
        return true;
    };

    auto format_zset_score = [](double value) {
        if (value == 0.0) {
            return std::string("0");
        }
        std::ostringstream oss;
        oss << std::setprecision(17) << value;
        std::string out = oss.str();
        if (out.find('.') != std::string::npos && out.find('e') == std::string::npos
            && out.find('E') == std::string::npos) {
            while (!out.empty() && out.back() == '0') {
                out.pop_back();
            }
            if (!out.empty() && out.back() == '.') {
                out.pop_back();
            }
        }
        return out;
    };

    auto read_zset_cardinality = [&](void* txn, const std::string& zset_key, int64_t& count) {
        std::string meta_key = zset_meta_storage_key(zset_key);
        std::string value;
        bool exists = false;
        mako::Status s = read_internal_current(txn, meta_key, value, exists);
        if (!s.ok()) {
            return s;
        }
        if (!exists) {
            count = 0;
            return mako::Status::OK();
        }
        if (!parse_int64(value, count) || count < 0) {
            count = 0;
        }
        return mako::Status::OK();
    };

    auto write_zset_cardinality = [&](void* txn, const std::string& zset_key, int64_t count) {
        std::string meta_key = zset_meta_storage_key(zset_key);
        if (count <= 0) {
            batch_exists[meta_key] = false;
            batch_values.erase(meta_key);
            return delete_raw_if_exists(txn, meta_key);
        }
        std::string payload = std::to_string(count);
        mako::Status s = put_raw(txn, meta_key, payload);
        if (s.ok()) {
            batch_exists[meta_key] = true;
            batch_values[meta_key] = payload;
        }
        return s;
    };

    auto read_list_meta = [&](void* txn, const std::string& list_key, int64_t& head, int64_t& tail) {
        head = 0;
        tail = 0;
        std::string meta_key = make_list_meta_key(list_key);
        std::string value;
        bool exists = false;
        mako::Status s = read_internal_current(txn, meta_key, value, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        size_t pos = 0;
        if (!read_i64_le_from_string(value, pos, head) || !read_i64_le_from_string(value, pos, tail)
            || pos != value.size() || tail < head) {
            head = 0;
            tail = 0;
        }
        return mako::Status::OK();
    };

    auto write_list_meta = [&](void* txn, const std::string& list_key, int64_t head, int64_t tail) {
        std::string meta_key = make_list_meta_key(list_key);
        if (tail <= head) {
            batch_exists[meta_key] = false;
            batch_values.erase(meta_key);
            return delete_raw_if_exists(txn, meta_key);
        }
        std::string payload = pack_list_meta(head, tail);
        mako::Status s = put_raw(txn, meta_key, payload);
        if (s.ok()) {
            batch_exists[meta_key] = true;
            batch_values[meta_key] = payload;
        }
        return s;
    };

    auto read_list_values = [&](void* txn, const std::string& list_key, std::vector<std::string>& values) {
        values.clear();
        int64_t head = 0;
        int64_t tail = 0;
        mako::Status s = read_list_meta(txn, list_key, head, tail);
        if (!s.ok()) {
            return s;
        }
        values.reserve(static_cast<size_t>(std::max<int64_t>(0, tail - head)));
        for (int64_t index = head; index < tail; ++index) {
            std::string element;
            bool exists = false;
            s = read_internal_current(txn, make_list_element_key(list_key, index), element, exists);
            if (!s.ok()) {
                return s;
            }
            if (exists) {
                values.push_back(element);
            }
        }
        return mako::Status::OK();
    };

    auto clear_list_elements = [&](void* txn, const std::string& list_key, int64_t head, int64_t tail) {
        mako::Status s = mako::Status::OK();
        for (int64_t index = head; index < tail; ++index) {
            std::string element_key = make_list_element_key(list_key, index);
            s = delete_raw_if_exists(txn, element_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[element_key] = false;
            batch_values.erase(element_key);
        }
        return s;
    };

    auto delete_list = [&](void* txn, const std::string& list_key) {
        int64_t head = 0;
        int64_t tail = 0;
        mako::Status s = read_list_meta(txn, list_key, head, tail);
        if (!s.ok()) {
            return s;
        }
        s = clear_list_elements(txn, list_key, head, tail);
        if (s.ok()) {
            s = write_list_meta(txn, list_key, 0, 0);
        }
        if (s.ok()) {
            staged_lists.erase(list_key);
            staged_lists_loaded.erase(list_key);
            dirty_lists.erase(list_key);
        }
        return s;
    };

    auto rewrite_list_values = [&](void* txn, const std::string& list_key, const std::vector<std::string>& values) {
        int64_t old_head = 0;
        int64_t old_tail = 0;
        mako::Status s = read_list_meta(txn, list_key, old_head, old_tail);
        if (!s.ok()) {
            return s;
        }
        int64_t new_head = old_tail;
        if (!values.empty()
            && old_tail > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(values.size())) {
            if (old_head < std::numeric_limits<int64_t>::min() + static_cast<int64_t>(values.size())) {
                return mako::Status::InvalidArgument("list index overflow");
            }
            new_head = old_head - static_cast<int64_t>(values.size());
        }
        int64_t index = new_head;
        for (const auto& value : values) {
            std::string element_key = make_list_element_key(list_key, index++);
            s = put_raw(txn, element_key, value);
            if (!s.ok()) {
                return s;
            }
            batch_exists[element_key] = true;
            batch_values[element_key] = value;
        }
        s = clear_list_elements(txn, list_key, old_head, old_tail);
        if (!s.ok()) {
            return s;
        }
        return write_list_meta(txn, list_key, new_head, new_head + static_cast<int64_t>(values.size()));
    };

    auto read_list_length = [&](void* txn, const std::string& list_key, int64_t& length) {
        int64_t head = 0;
        int64_t tail = 0;
        mako::Status s = read_list_meta(txn, list_key, head, tail);
        length = std::max<int64_t>(0, tail - head);
        return s;
    };

    auto collect_zset_values = [&](void* txn, const std::string& zset_key, std::map<std::string, double>& values) {
        values.clear();
        const std::string member_prefix = make_zset_member_prefix(zset_key);
        std::optional<std::string> scan_end = storage_prefix_upper(member_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class ZSetMemberScanCallback : public oi_scan_callback {
        public:
            ZSetMemberScanCallback(
                std::map<std::string, double>& values,
                std::string_view member_prefix,
                size_t member_prefix_len,
                const std::function<bool(const std::string&, double&)>& parse_score)
                : values_(values),
                  member_prefix_(member_prefix),
                  member_prefix_len_(member_prefix_len),
                  parse_score_(parse_score) {}

            bool invoke(const char* keyp, size_t keylen, const std::string& value) override {
                std::string_view storage_key(keyp, keylen);
                if (storage_key.rfind(member_prefix_, 0) != 0) {
                    return true;
                }
                double score = 0.0;
                if (!parse_score_(value, score)) {
                    return true;
                }
                values_[std::string(storage_key.substr(member_prefix_len_))] = score;
                return true;
            }

        private:
            std::map<std::string, double>& values_;
            std::string_view member_prefix_;
            size_t member_prefix_len_;
            const std::function<bool(const std::string&, double&)>& parse_score_;
        };

        std::function<bool(const std::string&, double&)> parse_score =
            [&](const std::string& input, double& out) {
                return parse_zset_score_value(input, out);
            };
        ZSetMemberScanCallback callback(values, member_prefix, member_prefix.size(), parse_score);
        tx_scan(g_table, txn, member_prefix, scan_end_ptr, callback, tl_arena);

        for (const auto& [storage_key, exists] : batch_exists) {
            if (storage_key.rfind(member_prefix, 0) != 0) {
                continue;
            }
            std::string member = storage_key.substr(member_prefix.size());
            if (!exists) {
                values.erase(member);
                continue;
            }
            auto value_it = batch_values.find(storage_key);
            if (value_it == batch_values.end()) {
                continue;
            }
            double score = 0.0;
            if (parse_zset_score_value(value_it->second, score)) {
                values[member] = score;
            }
        }
        return mako::Status::OK();
    };

    auto zset_ordered_items = [](const std::map<std::string, double>& values) {
        std::vector<std::pair<std::string, double>> items(values.begin(), values.end());
        std::sort(items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.second < rhs.second) {
                return true;
            }
            if (lhs.second > rhs.second) {
                return false;
            }
            return lhs.first < rhs.first;
        });
        return items;
    };

    struct ZScoreBound {
        double value = 0.0;
        bool exclusive = false;
    };

    auto parse_zset_score_bound = [&](const std::string& raw, ZScoreBound& bound) {
        std::string input = raw;
        bound.exclusive = false;
        if (!input.empty() && input[0] == '(') {
            bound.exclusive = true;
            input.erase(input.begin());
        }
        if (input == "-inf") {
            bound.value = -std::numeric_limits<double>::infinity();
            return true;
        }
        if (input == "+inf" || input == "inf") {
            bound.value = std::numeric_limits<double>::infinity();
            return true;
        }
        return parse_zset_score_value(input, bound.value);
    };

    struct ZLexBound {
        int kind = 0; // -1 is -inf, 0 is value, 1 is +inf.
        std::string value;
        bool exclusive = false;
    };

    auto parse_zset_lex_bound = [](const std::string& raw, ZLexBound& bound) {
        bound = ZLexBound{};
        if (raw == "-") {
            bound.kind = -1;
            return true;
        }
        if (raw == "+") {
            bound.kind = 1;
            return true;
        }
        if (!raw.empty() && (raw[0] == '[' || raw[0] == '(')) {
            bound.kind = 0;
            bound.exclusive = raw[0] == '(';
            bound.value = raw.substr(1);
            return true;
        }
        return false;
    };

    auto zscore_in_range = [](double score, const ZScoreBound& min, const ZScoreBound& max) {
        const bool above_min = min.exclusive ? score > min.value : score >= min.value;
        const bool below_max = max.exclusive ? score < max.value : score <= max.value;
        return above_min && below_max;
    };

    auto zlex_above_min = [](const std::string& member, const ZLexBound& min) {
        if (min.kind < 0) {
            return true;
        }
        if (min.kind > 0) {
            return false;
        }
        const int cmp = member.compare(min.value);
        return min.exclusive ? cmp > 0 : cmp >= 0;
    };

    auto zlex_below_max = [](const std::string& member, const ZLexBound& max) {
        if (max.kind > 0) {
            return true;
        }
        if (max.kind < 0) {
            return false;
        }
        const int cmp = member.compare(max.value);
        return max.exclusive ? cmp < 0 : cmp <= 0;
    };

    auto apply_zrange_limit = [&](std::vector<std::pair<std::string, double>>& selected,
                                 const std::vector<std::string>& bounds) {
        if (bounds.size() < 4) {
            return true;
        }
        int64_t offset = 0;
        int64_t count = 0;
        if (!parse_int64(bounds[2], offset) || !parse_int64(bounds[3], count)) {
            return false;
        }
        if (offset < 0 || count <= 0 || offset >= static_cast<int64_t>(selected.size())) {
            selected.clear();
            return true;
        }
        auto begin = selected.begin() + static_cast<size_t>(offset);
        auto end = begin + std::min<size_t>(
            static_cast<size_t>(count),
            static_cast<size_t>(selected.end() - begin));
        selected.assign(begin, end);
        return true;
    };

    auto select_zset_rank_range = [&](const std::map<std::string, double>& values,
                                      const std::vector<std::string>& bounds,
                                      bool reverse,
                                      std::vector<std::pair<std::string, double>>& selected) {
        auto items = zset_ordered_items(values);
        if (reverse) {
            std::reverse(items.begin(), items.end());
        }
        int64_t start_index = 0;
        int64_t stop_index = 0;
        if (bounds.size() < 2 || !parse_int64(bounds[0], start_index) || !parse_int64(bounds[1], stop_index)) {
            return false;
        }
        const int64_t length = static_cast<int64_t>(items.size());
        if (start_index < 0) {
            start_index += length;
        }
        if (stop_index < 0) {
            stop_index += length;
        }
        start_index = std::max<int64_t>(0, start_index);
        stop_index = std::min<int64_t>(length - 1, stop_index);
        selected.clear();
        if (length > 0 && start_index <= stop_index && start_index < length) {
            selected.assign(
                items.begin() + static_cast<size_t>(start_index),
                items.begin() + static_cast<size_t>(stop_index + 1));
        }
        return true;
    };

    auto select_zset_score_range = [&](const std::map<std::string, double>& values,
                                       const std::vector<std::string>& bounds,
                                       bool reverse,
                                       std::vector<std::pair<std::string, double>>& selected) {
        if (bounds.size() < 2) {
            return false;
        }
        ZScoreBound min_bound;
        ZScoreBound max_bound;
        if (!parse_zset_score_bound(bounds[0], min_bound) || !parse_zset_score_bound(bounds[1], max_bound)) {
            return false;
        }
        selected.clear();
        for (const auto& item : zset_ordered_items(values)) {
            if (zscore_in_range(item.second, min_bound, max_bound)) {
                selected.push_back(item);
            }
        }
        if (reverse) {
            std::reverse(selected.begin(), selected.end());
        }
        return apply_zrange_limit(selected, bounds);
    };

    auto select_zset_lex_range = [&](const std::map<std::string, double>& values,
                                     const std::vector<std::string>& bounds,
                                     bool reverse,
                                     std::vector<std::pair<std::string, double>>& selected) {
        if (bounds.size() < 2) {
            return false;
        }
        ZLexBound min_bound;
        ZLexBound max_bound;
        if (!parse_zset_lex_bound(bounds[0], min_bound) || !parse_zset_lex_bound(bounds[1], max_bound)) {
            return false;
        }
        selected.clear();
        for (const auto& [member, score] : values) {
            if (zlex_above_min(member, min_bound) && zlex_below_max(member, max_bound)) {
                selected.emplace_back(member, score);
            }
        }
        if (reverse) {
            std::reverse(selected.begin(), selected.end());
        }
        return apply_zrange_limit(selected, bounds);
    };

    auto combine_zset_aggregate_score = [](double current, double incoming, int64_t aggregate) {
        if (aggregate == 1) {
            return std::min(current, incoming);
        }
        if (aggregate == 2) {
            return std::max(current, incoming);
        }
        if (std::isinf(current) && std::isinf(incoming)
            && std::signbit(current) != std::signbit(incoming)) {
            return 0.0;
        }
        return current + incoming;
    };

    auto delete_zset = [&](void* txn, const std::string& zset_key) {
        std::map<std::string, double> values;
        mako::Status s = collect_zset_values(txn, zset_key, values);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [member, score] : values) {
            std::string member_key = zset_member_storage_key(zset_key, member);
            s = delete_raw_if_exists(txn, member_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[member_key] = false;
            batch_values.erase(member_key);

            std::string score_key = zset_score_storage_key(zset_key, score, member);
            s = delete_raw_if_exists(txn, score_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[score_key] = false;
            batch_values.erase(score_key);
        }
        if (s.ok()) {
            s = write_zset_cardinality(txn, zset_key, 0);
        }
        if (s.ok()) {
            staged_zsets.erase(zset_key);
            staged_zsets_loaded.erase(zset_key);
            dirty_zsets.erase(zset_key);
        }
        return s;
    };

    auto rewrite_zset_values = [&](void* txn, const std::string& zset_key, const std::map<std::string, double>& values) {
        std::map<std::string, double> existing;
        mako::Status s = collect_zset_values(txn, zset_key, existing);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [member, old_score] : existing) {
            auto next_it = values.find(member);
            if (next_it != values.end() && next_it->second == old_score) {
                continue;
            }
            if (next_it == values.end()) {
                std::string member_key = zset_member_storage_key(zset_key, member);
                s = delete_raw_if_exists(txn, member_key);
                if (!s.ok()) {
                    return s;
                }
                batch_exists[member_key] = false;
                batch_values.erase(member_key);
            }

            std::string score_key = zset_score_storage_key(zset_key, old_score, member);
            s = delete_raw_if_exists(txn, score_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[score_key] = false;
            batch_values.erase(score_key);
        }
        for (const auto& [member, score] : values) {
            auto old_it = existing.find(member);
            if (old_it != existing.end() && old_it->second == score) {
                continue;
            }
            std::string score_text = format_zset_score(score);
            std::string member_key = zset_member_storage_key(zset_key, member);
            s = put_raw(txn, member_key, score_text);
            if (!s.ok()) {
                return s;
            }
            batch_exists[member_key] = true;
            batch_values[member_key] = score_text;

            std::string score_key = zset_score_storage_key(zset_key, score, member);
            s = put_raw(txn, score_key, "1");
            if (!s.ok()) {
                return s;
            }
            batch_exists[score_key] = true;
            batch_values[score_key] = "1";
        }
        return write_zset_cardinality(txn, zset_key, static_cast<int64_t>(values.size()));
    };

    // ----- Stream storage access -----
    //
    // Every family is walked the same way: one ordered range scan of storage
    // followed by a merge of this transaction's own buffered writes and
    // deferred deletes, exactly as collect_zset_values does, so a stream reads
    // back what the same transaction just wrote. `limit` stops the storage walk
    // early once that many records are in hand, after allowing for the deletes
    // that will drop out of the range; a limit of 0 collects the whole range.
    auto collect_stream_range = [&](void* txn,
                                    const std::string& prefix,
                                    const std::string& scan_start,
                                    const std::string& scan_end,
                                    size_t limit,
                                    std::vector<std::pair<std::string, std::string>>& out) {
        out.clear();
        auto key_in_range = [&](const std::string& storage_key) {
            return storage_key.size() >= prefix.size()
                && storage_key.compare(0, prefix.size(), prefix) == 0
                && storage_key >= scan_start
                && (scan_end.empty() || storage_key < scan_end);
        };

        size_t scan_limit = 0;
        if (limit != 0) {
            size_t deletes_in_range = 0;
            for (const auto& storage_key : pending_deletes) {
                if (key_in_range(storage_key)) {
                    ++deletes_in_range;
                }
            }
            scan_limit = limit + deletes_in_range;
        }

        std::map<std::string, std::string> merged;

        class StreamRangeScanCallback : public oi_scan_callback {
        public:
            StreamRangeScanCallback(
                std::map<std::string, std::string>& merged,
                std::string_view prefix,
                const std::string& scan_end,
                size_t limit)
                : merged_(merged), prefix_(prefix), scan_end_(scan_end), limit_(limit) {}

            bool invoke(const char* keyp, size_t keylen, const std::string& value) override {
                std::string storage_key(keyp, keylen);
                if (storage_key.rfind(prefix_, 0) != 0) {
                    return false;
                }
                if (!scan_end_.empty() && storage_key >= scan_end_) {
                    return false;
                }
                merged_.emplace(std::move(storage_key), value);
                return limit_ == 0 || merged_.size() < limit_;
            }

        private:
            std::map<std::string, std::string>& merged_;
            std::string_view prefix_;
            const std::string& scan_end_;
            size_t limit_;
        };

        StreamRangeScanCallback callback(merged, prefix, scan_end, scan_limit);
        const std::string* scan_end_ptr = scan_end.empty() ? nullptr : &scan_end;
        tx_scan(g_table, txn, scan_start, scan_end_ptr, callback, tl_arena);

        // The transaction's own view wins over what storage returned. A key the
        // transaction wrote is inserted even if the limited walk stopped before
        // it, and one it deleted is dropped.
        for (const auto& [storage_key, exists] : batch_exists) {
            if (!key_in_range(storage_key)) {
                continue;
            }
            if (!exists) {
                merged.erase(storage_key);
                continue;
            }
            auto value_it = batch_values.find(storage_key);
            if (value_it == batch_values.end()) {
                continue;
            }
            merged[storage_key] = value_it->second;
        }

        out.reserve(limit != 0 ? std::min(limit, merged.size()) : merged.size());
        for (auto& entry : merged) {
            out.emplace_back(entry.first, entry.second);
            if (limit != 0 && out.size() >= limit) {
                break;
            }
        }
        return mako::Status::OK();
    };

    auto read_stream_meta = [&](void* txn, const std::string& stream_key, RedisStreamMeta& meta, bool& exists) {
        meta = RedisStreamMeta{};
        std::string value;
        exists = false;
        mako::Status s = read_internal_current(txn, make_stream_meta_key(stream_key), value, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        if (!stream_unpack_meta(value, meta)) {
            meta = RedisStreamMeta{};
        }
        return mako::Status::OK();
    };

    auto write_stream_meta = [&](void* txn, const std::string& stream_key, const RedisStreamMeta& meta) {
        return put_raw(txn, make_stream_meta_key(stream_key), stream_pack_meta(meta));
    };

    // A stream exists exactly while its meta record does: XADD MAXLEN 0 and
    // XGROUP CREATE MKSTREAM both leave a stream with no entries, and Redis
    // reports that key as existing and typed `stream`.
    auto stream_exists = [&](void* txn, const std::string& stream_key, bool& exists) {
        std::string value;
        exists = false;
        return read_internal_current(txn, make_stream_meta_key(stream_key), value, exists);
    };

    auto read_stream_entry = [&](void* txn,
                                 const std::string& stream_key,
                                 const RedisStreamId& id,
                                 std::string& fields,
                                 bool& exists) {
        return read_internal_current(txn, make_stream_entry_key(stream_key, id), fields, exists);
    };

    // The smallest ID still stored, which is what XINFO reports as
    // recorded-first-entry-id and what the lag arithmetic calls first_id.
    auto read_stream_first_id = [&](void* txn, const std::string& stream_key, RedisStreamId& id, bool& found) {
        const std::string prefix = make_stream_entry_prefix(stream_key);
        std::optional<std::string> upper = storage_prefix_upper(prefix);
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = collect_stream_range(
            txn, prefix, prefix, upper ? *upper : std::string(), 1, records);
        if (!s.ok()) {
            return s;
        }
        found = !records.empty() && stream_id_from_key_suffix(records[0].first, prefix.size(), id);
        if (!found) {
            id = RedisStreamId{};
        }
        return mako::Status::OK();
    };

    // XRANGE/XREVRANGE/XREAD in one call: the inclusive [start, end] ID range
    // becomes a storage key range, so COUNT on a forward read stops the walk
    // instead of filtering afterwards. A reverse read has to see the whole
    // range before it knows which records are the last `count`, because the
    // ordered index only walks forward.
    auto collect_stream_entries = [&](void* txn,
                                      const std::string& stream_key,
                                      const RedisStreamId& start,
                                      const RedisStreamId& end,
                                      size_t count,
                                      bool reverse,
                                      std::vector<std::pair<RedisStreamId, std::string>>& out) {
        out.clear();
        if (stream_id_compare(start, end) > 0) {
            return mako::Status::OK();
        }
        const std::string prefix = make_stream_entry_prefix(stream_key);
        const std::string scan_start = make_stream_entry_key(stream_key, start);
        std::string scan_end = make_stream_entry_key(stream_key, end);
        scan_end.push_back('\0');
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = collect_stream_range(
            txn, prefix, scan_start, scan_end, reverse ? 0 : count, records);
        if (!s.ok()) {
            return s;
        }
        size_t first = 0;
        if (reverse && count != 0 && records.size() > count) {
            first = records.size() - count;
        }
        out.reserve(records.size() - first);
        for (size_t i = first; i < records.size(); ++i) {
            RedisStreamId id;
            if (!stream_id_from_key_suffix(records[i].first, prefix.size(), id)) {
                continue;
            }
            out.emplace_back(id, records[i].second);
        }
        if (reverse) {
            std::reverse(out.begin(), out.end());
        }
        return mako::Status::OK();
    };

    auto collect_stream_group_names = [&](void* txn, const std::string& stream_key, std::vector<std::string>& names) {
        names.clear();
        const std::string prefix = make_stream_group_prefix(stream_key);
        std::optional<std::string> upper = storage_prefix_upper(prefix);
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = collect_stream_range(
            txn, prefix, prefix, upper ? *upper : std::string(), 0, records);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [storage_key, value] : records) {
            if (storage_key.size() < prefix.size() + 8) {
                continue;
            }
            const uint64_t name_len = stream_get_u64_le(storage_key, prefix.size());
            if (storage_key.size() != prefix.size() + 8 + name_len) {
                continue;
            }
            names.emplace_back(storage_key, prefix.size() + 8, static_cast<size_t>(name_len));
        }
        return mako::Status::OK();
    };

    auto read_stream_group = [&](void* txn,
                                 const std::string& stream_key,
                                 const std::string& group_name,
                                 RedisStreamGroup& group,
                                 bool& exists) {
        group = RedisStreamGroup{};
        std::string value;
        exists = false;
        mako::Status s = read_internal_current(
            txn, make_stream_group_key(stream_key, group_name), value, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        if (!stream_unpack_group(value, group)) {
            group = RedisStreamGroup{};
        }
        return mako::Status::OK();
    };

    auto write_stream_group = [&](void* txn,
                                  const std::string& stream_key,
                                  const std::string& group_name,
                                  const RedisStreamGroup& group) {
        return put_raw(txn, make_stream_group_key(stream_key, group_name), stream_pack_group(group));
    };

    auto collect_stream_consumer_names = [&](void* txn,
                                             const std::string& stream_key,
                                             const std::string& group_name,
                                             std::vector<std::pair<std::string, RedisStreamConsumer>>& consumers) {
        consumers.clear();
        const std::string prefix = make_stream_consumer_prefix(stream_key, group_name);
        std::optional<std::string> upper = storage_prefix_upper(prefix);
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = collect_stream_range(
            txn, prefix, prefix, upper ? *upper : std::string(), 0, records);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [storage_key, value] : records) {
            RedisStreamConsumer consumer;
            if (!stream_unpack_consumer(value, consumer)) {
                continue;
            }
            consumers.emplace_back(
                std::string(storage_key, prefix.size(), storage_key.size() - prefix.size()),
                consumer);
        }
        return mako::Status::OK();
    };

    auto read_stream_consumer = [&](void* txn,
                                    const std::string& stream_key,
                                    const std::string& group_name,
                                    const std::string& consumer_name,
                                    RedisStreamConsumer& consumer,
                                    bool& exists) {
        consumer = RedisStreamConsumer{};
        std::string value;
        exists = false;
        mako::Status s = read_internal_current(
            txn, make_stream_consumer_key(stream_key, group_name, consumer_name), value, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        if (!stream_unpack_consumer(value, consumer)) {
            consumer = RedisStreamConsumer{};
        }
        return mako::Status::OK();
    };

    auto write_stream_consumer = [&](void* txn,
                                     const std::string& stream_key,
                                     const std::string& group_name,
                                     const std::string& consumer_name,
                                     const RedisStreamConsumer& consumer) {
        return put_raw(
            txn,
            make_stream_consumer_key(stream_key, group_name, consumer_name),
            stream_pack_consumer(consumer));
    };

    auto read_stream_nack = [&](void* txn,
                                const std::string& stream_key,
                                const std::string& group_name,
                                const RedisStreamId& id,
                                RedisStreamNack& nack,
                                bool& exists) {
        nack = RedisStreamNack{};
        std::string value;
        exists = false;
        mako::Status s = read_internal_current(
            txn, make_stream_pel_key(stream_key, group_name, id), value, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        if (!stream_unpack_nack(value, nack)) {
            exists = false;
        }
        return mako::Status::OK();
    };

    auto write_stream_nack = [&](void* txn,
                                 const std::string& stream_key,
                                 const std::string& group_name,
                                 const RedisStreamId& id,
                                 const RedisStreamNack& nack) {
        return put_raw(txn, make_stream_pel_key(stream_key, group_name, id), stream_pack_nack(nack));
    };

    // The group PEL in ID order. A per-consumer view is this list filtered by
    // owner, which is what Redis's per-consumer PEL is a materialized copy of.
    auto collect_stream_pel = [&](void* txn,
                                  const std::string& stream_key,
                                  const std::string& group_name,
                                  const RedisStreamId& start,
                                  const RedisStreamId& end,
                                  size_t count,
                                  std::vector<std::pair<RedisStreamId, RedisStreamNack>>& out) {
        out.clear();
        if (stream_id_compare(start, end) > 0) {
            return mako::Status::OK();
        }
        const std::string prefix = make_stream_pel_prefix(stream_key, group_name);
        const std::string scan_start = make_stream_pel_key(stream_key, group_name, start);
        std::string scan_end = make_stream_pel_key(stream_key, group_name, end);
        scan_end.push_back('\0');
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = collect_stream_range(txn, prefix, scan_start, scan_end, count, records);
        if (!s.ok()) {
            return s;
        }
        out.reserve(records.size());
        for (const auto& [storage_key, value] : records) {
            RedisStreamId id;
            RedisStreamNack nack;
            if (!stream_id_from_key_suffix(storage_key, prefix.size(), id)
                || !stream_unpack_nack(value, nack)) {
                continue;
            }
            out.emplace_back(id, nack);
        }
        return mako::Status::OK();
    };

    // Drop one PEL record and decrement both counters that track it.
    auto remove_stream_nack = [&](void* txn,
                                  const std::string& stream_key,
                                  const std::string& group_name,
                                  RedisStreamGroup& group,
                                  const RedisStreamId& id,
                                  const RedisStreamNack& nack) {
        mako::Status s = delete_raw_if_exists(txn, make_stream_pel_key(stream_key, group_name, id));
        if (!s.ok()) {
            return s;
        }
        if (group.pel > 0) {
            group.pel -= 1;
        }
        RedisStreamConsumer owner;
        bool owner_exists = false;
        s = read_stream_consumer(txn, stream_key, group_name, nack.consumer, owner, owner_exists);
        if (!s.ok() || !owner_exists) {
            return s;
        }
        if (owner.pending > 0) {
            owner.pending -= 1;
        }
        return write_stream_consumer(txn, stream_key, group_name, nack.consumer, owner);
    };

    // Redis creates a consumer on first mention and keeps two clocks on it:
    // seen-time on every command it issues, active-time only when it is handed
    // an entry.
    auto touch_stream_consumer = [&](void* txn,
                                     const std::string& stream_key,
                                     const std::string& group_name,
                                     RedisStreamGroup& group,
                                     const std::string& consumer_name,
                                     bool delivered,
                                     RedisStreamConsumer& consumer,
                                     bool& created) {
        bool exists = false;
        mako::Status s = read_stream_consumer(txn, stream_key, group_name, consumer_name, consumer, exists);
        if (!s.ok()) {
            return s;
        }
        created = !exists;
        const int64_t now = now_unix_ms();
        if (!exists) {
            consumer = RedisStreamConsumer{};
            consumer.active_time_ms = -1;
            group.consumers += 1;
        }
        consumer.seen_time_ms = now;
        if (delivered) {
            consumer.active_time_ms = now;
        }
        return write_stream_consumer(txn, stream_key, group_name, consumer_name, consumer);
    };

    // Everything a group owns: the PEL, the consumers and the group record.
    auto delete_stream_group_records = [&](void* txn, const std::string& stream_key, const std::string& group_name) {
        const std::string pel_prefix = make_stream_pel_prefix(stream_key, group_name);
        std::optional<std::string> pel_upper = storage_prefix_upper(pel_prefix);
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = collect_stream_range(
            txn, pel_prefix, pel_prefix, pel_upper ? *pel_upper : std::string(), 0, records);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [storage_key, value] : records) {
            (void)value;
            s = delete_raw_if_exists(txn, storage_key);
            if (!s.ok()) {
                return s;
            }
        }
        const std::string consumer_prefix = make_stream_consumer_prefix(stream_key, group_name);
        std::optional<std::string> consumer_upper = storage_prefix_upper(consumer_prefix);
        s = collect_stream_range(
            txn, consumer_prefix, consumer_prefix,
            consumer_upper ? *consumer_upper : std::string(), 0, records);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [storage_key, value] : records) {
            (void)value;
            s = delete_raw_if_exists(txn, storage_key);
            if (!s.ok()) {
                return s;
            }
        }
        return delete_raw_if_exists(txn, make_stream_group_key(stream_key, group_name));
    };

    // DEL/expiry/RENAME of the key: every record of every family goes.
    auto delete_stream = [&](void* txn, const std::string& stream_key) {
        bool exists = false;
        mako::Status s = stream_exists(txn, stream_key, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        std::vector<std::string> group_names;
        s = collect_stream_group_names(txn, stream_key, group_names);
        if (!s.ok()) {
            return s;
        }
        for (const auto& group_name : group_names) {
            s = delete_stream_group_records(txn, stream_key, group_name);
            if (!s.ok()) {
                return s;
            }
        }
        const std::string entry_prefix = make_stream_entry_prefix(stream_key);
        std::optional<std::string> entry_upper = storage_prefix_upper(entry_prefix);
        std::vector<std::pair<std::string, std::string>> records;
        s = collect_stream_range(
            txn, entry_prefix, entry_prefix, entry_upper ? *entry_upper : std::string(), 0, records);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [storage_key, value] : records) {
            (void)value;
            s = delete_raw_if_exists(txn, storage_key);
            if (!s.ok()) {
                return s;
            }
        }
        return delete_raw_if_exists(txn, make_stream_meta_key(stream_key));
    };

    // XTRIM, and the trailing half of an XADD that carried MAXLEN/MINID.
    // `limit` is Redis's LIMIT: the most entries one call may remove.
    auto trim_stream = [&](void* txn,
                           const std::string& stream_key,
                           RedisStreamMeta& meta,
                           const std::string& strategy,
                           const std::string& threshold,
                           int64_t limit,
                           int64_t& removed) {
        removed = 0;
        if (strategy.empty()) {
            return mako::Status::OK();
        }
        const std::string prefix = make_stream_entry_prefix(stream_key);
        std::vector<std::pair<std::string, std::string>> records;
        mako::Status s = mako::Status::OK();
        if (strategy == "MAXLEN") {
            uint64_t target = 0;
            if (!stream_parse_u64(threshold.data(), threshold.size(), target)
                || meta.length <= target) {
                return mako::Status::OK();
            }
            uint64_t to_remove = meta.length - target;
            if (limit > 0 && static_cast<uint64_t>(limit) < to_remove) {
                to_remove = static_cast<uint64_t>(limit);
            }
            std::optional<std::string> upper = storage_prefix_upper(prefix);
            s = collect_stream_range(
                txn, prefix, prefix, upper ? *upper : std::string(),
                static_cast<size_t>(to_remove), records);
        } else {
            RedisStreamId minid;
            if (!stream_id_parse(threshold, minid)) {
                return mako::Status::OK();
            }
            const std::string scan_end = make_stream_entry_key(stream_key, minid);
            s = collect_stream_range(
                txn, prefix, prefix, scan_end,
                limit > 0 ? static_cast<size_t>(limit) : 0, records);
        }
        if (!s.ok()) {
            return s;
        }
        for (const auto& [storage_key, value] : records) {
            (void)value;
            s = delete_raw_if_exists(txn, storage_key);
            if (!s.ok()) {
                return s;
            }
        }
        removed = static_cast<int64_t>(records.size());
        if (removed == 0) {
            return mako::Status::OK();
        }
        meta.length -= static_cast<uint64_t>(removed);
        bool found_first = false;
        s = read_stream_first_id(txn, stream_key, meta.first_id, found_first);
        if (!s.ok()) {
            return s;
        }
        if (!found_first) {
            meta.first_id = RedisStreamId{};
        }
        return mako::Status::OK();
    };

    // The group half of an XINFO reply, flattened into `payload` so Rust can
    // walk it with a cursor. `full` selects XINFO STREAM FULL's shape (every
    // group with its PEL and its consumers); otherwise it is XINFO GROUPS' one
    // summary row per group. `only_group`, when set, restricts the walk to that
    // one group, which is how XINFO CONSUMERS reads it.
    auto append_stream_group_info = [&](void* txn,
                                        const std::string& stream_key,
                                        const RedisStreamMeta& meta,
                                        const std::string& only_group,
                                        size_t pel_limit,
                                        bool full,
                                        std::vector<std::string>& payload) {
        std::vector<std::string> group_names;
        mako::Status s = collect_stream_group_names(txn, stream_key, group_names);
        if (!s.ok()) {
            return s;
        }
        if (!only_group.empty()) {
            std::vector<std::string> filtered;
            for (const auto& name : group_names) {
                if (name == only_group) {
                    filtered.push_back(name);
                }
            }
            group_names.swap(filtered);
        }
        RedisStreamId min_id;
        RedisStreamId max_id;
        max_id.ms = std::numeric_limits<uint64_t>::max();
        max_id.seq = std::numeric_limits<uint64_t>::max();
        payload.push_back(std::to_string(group_names.size()));
        for (const auto& group_name : group_names) {
            RedisStreamGroup group;
            bool group_exists = false;
            s = read_stream_group(txn, stream_key, group_name, group, group_exists);
            if (!s.ok()) {
                return s;
            }
            int64_t lag = 0;
            const bool lag_known = stream_group_lag(meta, group, lag);
            payload.push_back(group_name);
            if (!full) {
                payload.push_back(std::to_string(group.consumers));
                payload.push_back(std::to_string(group.pel));
                payload.push_back(stream_id_format(group.last_id));
                payload.push_back(group.entries_read >= 0 ? std::to_string(group.entries_read) : std::string());
                payload.push_back(lag_known ? std::to_string(lag) : std::string());
                continue;
            }
            payload.push_back(stream_id_format(group.last_id));
            payload.push_back(group.entries_read >= 0 ? std::to_string(group.entries_read) : std::string());
            payload.push_back(lag_known ? std::to_string(lag) : std::string());
            payload.push_back(std::to_string(group.pel));
            std::vector<std::pair<RedisStreamId, RedisStreamNack>> pel;
            s = collect_stream_pel(txn, stream_key, group_name, min_id, max_id, pel_limit, pel);
            if (!s.ok()) {
                return s;
            }
            payload.push_back(std::to_string(pel.size()));
            for (const auto& [id, nack] : pel) {
                payload.push_back(stream_id_format(id));
                payload.push_back(nack.consumer);
                payload.push_back(std::to_string(nack.delivery_time_ms));
                payload.push_back(std::to_string(nack.delivery_count));
            }
            std::vector<std::pair<std::string, RedisStreamConsumer>> consumers;
            s = collect_stream_consumer_names(txn, stream_key, group_name, consumers);
            if (!s.ok()) {
                return s;
            }
            payload.push_back(std::to_string(consumers.size()));
            for (const auto& [consumer_name, consumer] : consumers) {
                payload.push_back(consumer_name);
                payload.push_back(std::to_string(consumer.seen_time_ms));
                payload.push_back(std::to_string(consumer.active_time_ms));
                payload.push_back(std::to_string(consumer.pending));
                // A consumer's PEL view is the group's PEL filtered by owner.
                std::vector<std::string> own;
                size_t shown = 0;
                for (const auto& [id, nack] : pel) {
                    if (nack.consumer != consumer_name) {
                        continue;
                    }
                    own.push_back(stream_id_format(id));
                    own.push_back(std::to_string(nack.delivery_time_ms));
                    own.push_back(std::to_string(nack.delivery_count));
                    shown += 1;
                    if (pel_limit != 0 && shown >= pel_limit) {
                        break;
                    }
                }
                payload.push_back(std::to_string(shown));
                for (const auto& item : own) {
                    payload.push_back(item);
                }
            }
        }
        return mako::Status::OK();
    };

    // XINFO CONSUMERS: one row per consumer of one group.
    auto append_stream_consumer_info = [&](void* txn,
                                           const std::string& stream_key,
                                           const std::string& group_name,
                                           std::vector<std::string>& payload) {
        std::vector<std::pair<std::string, RedisStreamConsumer>> consumers;
        mako::Status s = collect_stream_consumer_names(txn, stream_key, group_name, consumers);
        if (!s.ok()) {
            return s;
        }
        const int64_t now = now_unix_ms();
        payload.push_back(std::to_string(consumers.size()));
        for (const auto& [consumer_name, consumer] : consumers) {
            payload.push_back(consumer_name);
            payload.push_back(std::to_string(consumer.pending));
            payload.push_back(std::to_string(std::max<int64_t>(0, now - consumer.seen_time_ms)));
            payload.push_back(consumer.active_time_ms < 0
                ? std::string("-1")
                : std::to_string(std::max<int64_t>(0, now - consumer.active_time_ms)));
        }
        return mako::Status::OK();
    };

    // Every stream family is <tag> + u64le(len(stream key)) + stream key +
    // <suffix>, and the suffix carries everything that identifies the record
    // inside the stream: the entry or PEL ID, the group name, the consumer
    // name. Reading each record's family letter, suffix and value is therefore
    // the whole stream, and writing them back under another name rebuilds it.
    // RENAME, COPY, MOVE and DUMP/RESTORE all go through this pair, so a stream
    // carries its groups, consumers and pending-entry lists wherever it goes.
    auto stream_family_tag = [](char letter, size_t& tag_len) -> const char* {
        switch (letter) {
            case 'M': tag_len = sizeof("\x01X#:") - 1; return "\x01X#:";
            case 'E': tag_len = sizeof("\x01X:") - 1; return "\x01X:";
            case 'G': tag_len = sizeof("\x01XG:") - 1; return "\x01XG:";
            case 'C': tag_len = sizeof("\x01XC:") - 1; return "\x01XC:";
            case 'P': tag_len = sizeof("\x01XP:") - 1; return "\x01XP:";
            default: tag_len = 0; return nullptr;
        }
    };

    auto collect_stream_records = [&](void* txn,
                                      const std::string& stream_key,
                                      std::vector<std::string>& out) {
        out.clear();
        static const char kFamilies[] = {'M', 'E', 'G', 'C', 'P'};
        for (const char letter : kFamilies) {
            size_t tag_len = 0;
            const char* tag = stream_family_tag(letter, tag_len);
            const std::string prefix = make_stream_family_prefix(tag, tag_len, stream_key);
            std::optional<std::string> upper = storage_prefix_upper(prefix);
            std::vector<std::pair<std::string, std::string>> records;
            mako::Status s = collect_stream_range(
                txn, prefix, prefix, upper ? *upper : std::string(), 0, records);
            if (!s.ok()) {
                return s;
            }
            for (const auto& [storage_key, value] : records) {
                out.emplace_back(1, letter);
                out.emplace_back(storage_key, prefix.size(), storage_key.size() - prefix.size());
                out.push_back(value);
            }
        }
        return mako::Status::OK();
    };

    auto write_stream_records = [&](void* txn,
                                    const std::string& stream_key,
                                    const std::vector<std::string>& records) {
        if (records.size() % 3 != 0) {
            return mako::Status::InvalidArgument("malformed stream record list");
        }
        for (size_t i = 0; i + 2 < records.size(); i += 3) {
            if (records[i].size() != 1) {
                return mako::Status::InvalidArgument("malformed stream record list");
            }
            size_t tag_len = 0;
            const char* tag = stream_family_tag(records[i][0], tag_len);
            if (tag == nullptr) {
                return mako::Status::InvalidArgument("malformed stream record list");
            }
            std::string storage_key = make_stream_family_prefix(tag, tag_len, stream_key);
            storage_key.append(records[i + 1]);
            mako::Status s = put_raw(txn, storage_key, records[i + 2]);
            if (!s.ok()) {
                return s;
            }
        }
        return mako::Status::OK();
    };

    auto collect_set_members = [&](void* txn, const std::string& set_key, std::vector<std::string>& members) {
        members.clear();
        const std::string user_prefix = make_set_member_prefix(set_key);
        const std::string storage_prefix = user_prefix;
        std::optional<std::string> scan_end = storage_prefix_upper(storage_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class SetScanCallback : public oi_scan_callback {
        public:
            SetScanCallback(
                std::vector<std::string>& members,
                std::string_view storage_prefix,
                size_t user_prefix_len)
                : members_(members),
                  storage_prefix_(storage_prefix),
                  user_prefix_len_(user_prefix_len) {}

            bool invoke(const char* keyp, size_t keylen, const std::string&) override {
                std::string_view storage_key(keyp, keylen);
                if (storage_key.rfind(storage_prefix_, 0) != 0) {
                    return true;
                }
                members_.emplace_back(storage_key.substr(user_prefix_len_));
                return true;
            }

        private:
            std::vector<std::string>& members_;
            std::string_view storage_prefix_;
            size_t user_prefix_len_;
        };

        SetScanCallback callback(members, storage_prefix, user_prefix.size());
        tx_scan(g_table, txn, storage_prefix, scan_end_ptr, callback, tl_arena);
        std::unordered_set<std::string> merged(members.begin(), members.end());
        for (const auto& [storage_key, exists] : batch_exists) {
            if (storage_key.rfind(storage_prefix, 0) != 0) {
                continue;
            }
            std::string member = storage_key.substr(user_prefix.size());
            if (exists) {
                merged.insert(std::move(member));
            } else {
                merged.erase(member);
            }
        }
        members.assign(merged.begin(), merged.end());
        return mako::Status::OK();
    };

    auto rewrite_set_values = [&](void* txn, const std::string& set_key, const std::unordered_set<std::string>& values) {
        std::vector<std::string> existing_members;
        mako::Status s = collect_set_members(txn, set_key, existing_members);
        if (!s.ok()) {
            return s;
        }
        std::unordered_set<std::string> existing(existing_members.begin(), existing_members.end());
        for (const auto& member : existing) {
            if (values.find(member) != values.end()) {
                continue;
            }
            std::string member_key = set_storage_key(set_key, member);
            s = delete_raw_if_exists(txn, member_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[member_key] = false;
            batch_values.erase(member_key);
        }
        for (const auto& member : values) {
            if (existing.find(member) != existing.end()) {
                continue;
            }
            std::string member_key = set_storage_key(set_key, member);
            s = put_raw(txn, member_key, "1");
            if (!s.ok()) {
                return s;
            }
            batch_exists[member_key] = true;
            batch_values[member_key] = "1";
        }
        return write_set_cardinality(txn, set_key, static_cast<int64_t>(values.size()));
    };

    auto read_hash_field_ttl = [&](void* txn, const std::string& hash_key, const std::string& field,
                                   int64_t& expire_at_ms, bool& exists) {
        expire_at_ms = -1;
        exists = false;
        std::string value;
        bool raw_exists = false;
        mako::Status s = read_internal_current(
            txn, make_hash_field_ttl_key(hash_key, field), value, raw_exists);
        if (!s.ok() || !raw_exists) {
            return s;
        }
        if (!parse_int64(value, expire_at_ms)) {
            expire_at_ms = -1;
            return mako::Status::OK();
        }
        exists = true;
        return mako::Status::OK();
    };

    auto write_hash_field_ttl = [&](void* txn, const std::string& hash_key, const std::string& field,
                                    int64_t expire_at_ms) {
        std::string ttl_key = make_hash_field_ttl_key(hash_key, field);
        std::string payload = std::to_string(expire_at_ms);
        mako::Status s = put_raw(txn, ttl_key, payload);
        if (s.ok()) {
            batch_exists[ttl_key] = true;
            batch_values[ttl_key] = payload;
        }
        return s;
    };

    auto clear_hash_field_ttl = [&](void* txn, const std::string& hash_key, const std::string& field) {
        std::string ttl_key = make_hash_field_ttl_key(hash_key, field);
        auto batch_it = batch_exists.find(ttl_key);
        if (batch_it != batch_exists.end() && !batch_it->second) {
            // Already removed earlier in this transaction; deleting the same
            // record twice inside one transaction is not worth the risk.
            return mako::Status::OK();
        }
        mako::Status s = delete_raw_if_exists(txn, ttl_key);
        if (s.ok()) {
            batch_exists[ttl_key] = false;
            batch_values.erase(ttl_key);
        }
        return s;
    };

    // Every field of the hash that carries an expiration, with its absolute
    // Unix ms. One range read over the hash's TTL prefix, so the cost is
    // proportional to the number of fields with a TTL, not to the hash size.
    auto collect_hash_field_ttls = [&](void* txn, const std::string& hash_key,
                                       std::map<std::string, int64_t>& ttls) {
        ttls.clear();
        const std::string ttl_prefix = make_hash_field_ttl_prefix(hash_key);
        std::optional<std::string> scan_end = storage_prefix_upper(ttl_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class HashFieldTtlScanCallback : public oi_scan_callback {
        public:
            HashFieldTtlScanCallback(
                std::map<std::string, std::string>& rows,
                std::string_view ttl_prefix,
                size_t ttl_prefix_len)
                : rows_(rows),
                  ttl_prefix_(ttl_prefix),
                  ttl_prefix_len_(ttl_prefix_len) {}

            bool invoke(const char* keyp, size_t keylen, const std::string& value) override {
                std::string_view storage_key(keyp, keylen);
                if (storage_key.rfind(ttl_prefix_, 0) != 0) {
                    return true;
                }
                rows_[std::string(storage_key.substr(ttl_prefix_len_))] = value;
                return true;
            }

        private:
            std::map<std::string, std::string>& rows_;
            std::string_view ttl_prefix_;
            size_t ttl_prefix_len_;
        };

        std::map<std::string, std::string> rows;
        HashFieldTtlScanCallback callback(rows, ttl_prefix, ttl_prefix.size());
        tx_scan(g_table, txn, ttl_prefix, scan_end_ptr, callback, tl_arena);
        for (const auto& [storage_key, exists] : batch_exists) {
            if (storage_key.rfind(ttl_prefix, 0) != 0) {
                continue;
            }
            std::string field = storage_key.substr(ttl_prefix.size());
            if (!exists) {
                rows.erase(field);
                continue;
            }
            auto value_it = batch_values.find(storage_key);
            if (value_it != batch_values.end()) {
                rows[field] = value_it->second;
            }
        }
        for (const auto& [field, raw] : rows) {
            int64_t parsed = 0;
            if (parse_int64(raw, parsed)) {
                ttls[field] = parsed;
            }
        }
        return mako::Status::OK();
    };

    auto clear_all_hash_field_ttls = [&](void* txn, const std::string& hash_key) {
        std::map<std::string, int64_t> ttls;
        mako::Status s = collect_hash_field_ttls(txn, hash_key, ttls);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [field, expire_at_ms] : ttls) {
            (void)expire_at_ms;
            s = clear_hash_field_ttl(txn, hash_key, field);
            if (!s.ok()) {
                return s;
            }
        }
        return mako::Status::OK();
    };

    auto collect_hash_entries = [&](void* txn, const std::string& hash_key, std::map<std::string, std::string>& entries) {
        entries.clear();
        const std::string field_prefix = make_hash_field_prefix(hash_key);
        std::optional<std::string> scan_end = storage_prefix_upper(field_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class HashScanCallback : public oi_scan_callback {
        public:
            HashScanCallback(
                std::map<std::string, std::string>& entries,
                std::string_view field_prefix,
                size_t field_prefix_len)
                : entries_(entries),
                  field_prefix_(field_prefix),
                  field_prefix_len_(field_prefix_len) {}

            bool invoke(const char* keyp, size_t keylen, const std::string& value) override {
                std::string_view storage_key(keyp, keylen);
                if (storage_key.rfind(field_prefix_, 0) != 0) {
                    return true;
                }
                entries_[std::string(storage_key.substr(field_prefix_len_))] = value;
                return true;
            }

        private:
            std::map<std::string, std::string>& entries_;
            std::string_view field_prefix_;
            size_t field_prefix_len_;
        };

        HashScanCallback callback(entries, field_prefix, field_prefix.size());
        tx_scan(g_table, txn, field_prefix, scan_end_ptr, callback, tl_arena);
        for (const auto& [storage_key, exists] : batch_exists) {
            if (storage_key.rfind(field_prefix, 0) != 0) {
                continue;
            }
            std::string field = storage_key.substr(field_prefix.size());
            if (!exists) {
                entries.erase(field);
                continue;
            }
            auto value_it = batch_values.find(storage_key);
            if (value_it != batch_values.end()) {
                entries[field] = value_it->second;
            }
        }
        return mako::Status::OK();
    };

    auto delete_hash = [&](void* txn, const std::string& hash_key) {
        std::map<std::string, std::string> entries;
        mako::Status s = collect_hash_entries(txn, hash_key, entries);
        if (!s.ok()) {
            return s;
        }
        for (const auto& [field, _] : entries) {
            std::string field_key = hash_field_storage_key(hash_key, field);
            s = delete_raw_if_exists(txn, field_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[field_key] = false;
            batch_values.erase(field_key);
        }
        // Every path that removes or replaces a hash wholesale (DEL, FLUSHDB,
        // RESTORE, SORT STORE, RENAME, COPY, key-level expiry) funnels through
        // here, so clearing the side keys here clears them everywhere.
        s = clear_all_hash_field_ttls(txn, hash_key);
        if (!s.ok()) {
            return s;
        }
        return write_hash_cardinality(txn, hash_key, 0);
    };

    // Lazy per-field expiry for a single field: if it carries an expiration
    // that has passed, drop the field and its side key and shrink the hash,
    // deleting the whole hash when it was the last field.
    auto expire_hash_field_if_needed = [&](void* txn, const std::string& hash_key,
                                           const std::string& field, bool& expired) {
        expired = false;
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_hash_field_ttl(txn, hash_key, field, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        std::string field_key = hash_field_storage_key(hash_key, field);
        std::string ignored;
        bool field_exists = false;
        s = read_internal_current(txn, field_key, ignored, field_exists);
        if (!s.ok()) {
            return s;
        }
        s = clear_hash_field_ttl(txn, hash_key, field);
        if (!s.ok() || !field_exists) {
            return s;
        }
        s = delete_raw_if_exists(txn, field_key);
        if (!s.ok()) {
            return s;
        }
        batch_exists[field_key] = false;
        batch_values.erase(field_key);
        expired = true;
        int64_t cardinality = 0;
        s = read_hash_cardinality(txn, hash_key, cardinality);
        if (!s.ok()) {
            return s;
        }
        cardinality -= 1;
        s = write_hash_cardinality(txn, hash_key, cardinality);
        if (s.ok() && cardinality <= 0) {
            // The last field went, so the key itself disappears, as HDEL does.
            s = delete_hash(txn, hash_key);
            if (s.ok()) {
                s = clear_ttl_meta(txn, hash_key);
            }
        }
        return s;
    };

    // Lazy per-field expiry for the whole-hash paths: one range read of the
    // hash's TTL prefix, then drop every field whose time has passed.
    auto expire_hash_fields_due = [&](void* txn, const std::string& hash_key) {
        std::map<std::string, int64_t> ttls;
        mako::Status s = collect_hash_field_ttls(txn, hash_key, ttls);
        if (!s.ok() || ttls.empty()) {
            return s;
        }
        const int64_t now_ms = now_unix_ms();
        int64_t removed = 0;
        for (const auto& [field, expire_at_ms] : ttls) {
            if (expire_at_ms > now_ms) {
                continue;
            }
            std::string field_key = hash_field_storage_key(hash_key, field);
            std::string ignored;
            bool field_exists = false;
            s = read_internal_current(txn, field_key, ignored, field_exists);
            if (!s.ok()) {
                return s;
            }
            s = clear_hash_field_ttl(txn, hash_key, field);
            if (!s.ok()) {
                return s;
            }
            if (!field_exists) {
                continue;
            }
            s = delete_raw_if_exists(txn, field_key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[field_key] = false;
            batch_values.erase(field_key);
            ++removed;
        }
        if (removed == 0) {
            return mako::Status::OK();
        }
        int64_t cardinality = 0;
        s = read_hash_cardinality(txn, hash_key, cardinality);
        if (!s.ok()) {
            return s;
        }
        cardinality -= removed;
        s = write_hash_cardinality(txn, hash_key, cardinality);
        if (s.ok() && cardinality <= 0) {
            s = delete_hash(txn, hash_key);
            if (s.ok()) {
                s = clear_ttl_meta(txn, hash_key);
            }
        }
        return s;
    };

    // Hash cardinality with expired fields already dropped. The sweep only runs
    // when the key really is a hash, so EXISTS/TYPE/DUMP of anything else costs
    // nothing extra.
    auto read_hash_cardinality_live = [&](void* txn, const std::string& hash_key, int64_t& count) {
        mako::Status s = read_hash_cardinality(txn, hash_key, count);
        if (!s.ok() || count <= 0) {
            return s;
        }
        s = expire_hash_fields_due(txn, hash_key);
        if (!s.ok()) {
            return s;
        }
        return read_hash_cardinality(txn, hash_key, count);
    };

    auto delete_set = [&](void* txn, const std::string& set_key) {
        std::vector<std::string> members;
        mako::Status s = collect_set_members(txn, set_key, members);
        if (!s.ok()) {
            return s;
        }
        for (const auto& member : members) {
            std::string key = set_storage_key(set_key, member);
            s = delete_raw_if_exists(txn, key);
            if (!s.ok()) {
                return s;
            }
            batch_exists[key] = false;
            batch_values.erase(key);
        }
        s = write_set_cardinality(txn, set_key, 0);
        if (s.ok()) {
            staged_sets.erase(set_key);
            staged_sets_loaded.erase(set_key);
            dirty_sets.erase(set_key);
        }
        return s;
    };

    auto expire_set_if_needed = [&](void* txn, const std::string& set_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, set_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        s = delete_set(txn, set_key);
        if (s.ok()) {
            s = clear_ttl_meta(txn, set_key);
        }
        return s;
    };

    auto expire_list_if_needed = [&](void* txn, const std::string& list_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, list_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        s = delete_list(txn, list_key);
        if (s.ok()) {
            s = clear_ttl_meta(txn, list_key);
        }
        return s;
    };

    auto expire_zset_if_needed = [&](void* txn, const std::string& zset_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, zset_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        s = delete_zset(txn, zset_key);
        if (s.ok()) {
            s = clear_ttl_meta(txn, zset_key);
        }
        return s;
    };

    auto expire_hash_if_needed = [&](void* txn, const std::string& hash_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, hash_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        s = delete_hash(txn, hash_key);
        if (s.ok()) {
            s = clear_ttl_meta(txn, hash_key);
        }
        return s;
    };

    auto expire_stream_if_needed = [&](void* txn, const std::string& stream_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, stream_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        s = delete_stream(txn, stream_key);
        if (s.ok()) {
            s = clear_ttl_meta(txn, stream_key);
        }
        return s;
    };

    auto expire_logical_key_if_needed = [&](void* txn, const std::string& user_key, const std::string& storage_key) {
        int64_t expire_at_ms = 0;
        bool ttl_exists = false;
        mako::Status s = read_ttl_meta(txn, user_key, expire_at_ms, ttl_exists);
        if (!s.ok() || !ttl_exists || expire_at_ms > now_unix_ms()) {
            return s;
        }
        s = delete_raw_if_exists(txn, storage_key);
        if (!s.ok()) {
            return s;
        }
        batch_exists[storage_key] = false;
        batch_values.erase(storage_key);
        s = delete_set(txn, user_key);
        if (s.ok()) {
            s = delete_list(txn, user_key);
        }
        if (s.ok()) {
            s = delete_zset(txn, user_key);
        }
        if (s.ok()) {
            s = delete_hash(txn, user_key);
        }
        if (s.ok()) {
            s = delete_stream(txn, user_key);
        }
        if (s.ok()) {
            s = clear_ttl_meta(txn, user_key);
        }
        return s;
    };

    auto read_string_exists_no_expire = [&](void* txn, const std::string& storage_key, bool& exists) {
        auto batch_it = batch_exists.find(storage_key);
        if (batch_it != batch_exists.end()) {
            exists = batch_it->second;
            return mako::Status::OK();
        }
        return read_raw_exists(txn, storage_key, exists);
    };

    auto read_logical_exists = [&](void* txn, const std::string& user_key, const std::string& storage_key, bool& exists) {
        mako::Status s = expire_logical_key_if_needed(txn, user_key, storage_key);
        if (!s.ok()) {
            return s;
        }
        bool string_exists = false;
        s = read_string_exists_no_expire(txn, storage_key, string_exists);
        if (!s.ok() || string_exists) {
            exists = string_exists;
            return s;
        }
        int64_t set_cardinality = 0;
        s = read_set_cardinality(txn, user_key, set_cardinality);
        if (!s.ok() || set_cardinality > 0) {
            exists = set_cardinality > 0;
            return s;
        }
        int64_t hash_count = 0;
        // A hash whose every field has expired is gone, so EXISTS/TYPE/DEL and
        // the key-level TTL commands must see it that way too.
        s = read_hash_cardinality_live(txn, user_key, hash_count);
        if (!s.ok() || hash_count > 0) {
            exists = hash_count > 0;
            return s;
        }
        auto staged_it = staged_lists.find(user_key);
        if (staged_it != staged_lists.end()) {
            exists = !staged_it->second.empty();
            return mako::Status::OK();
        }
        int64_t list_length = 0;
        s = read_list_length(txn, user_key, list_length);
        if (!s.ok() || list_length > 0) {
            exists = list_length > 0;
            return s;
        }
        auto staged_zset_it = staged_zsets.find(user_key);
        if (staged_zset_it != staged_zsets.end()) {
            exists = !staged_zset_it->second.empty();
            return mako::Status::OK();
        }
        int64_t zset_count = 0;
        s = read_zset_cardinality(txn, user_key, zset_count);
        if (!s.ok() || zset_count > 0) {
            exists = zset_count > 0;
            return s;
        }
        // A stream exists for as long as its meta record does, even with no
        // entries at all: XADD MAXLEN 0 and XGROUP CREATE MKSTREAM both leave
        // one behind and Redis reports that key as existing.
        return stream_exists(txn, user_key, exists);
    };

    auto read_set_member_exists = [&](void* txn, const std::string& set_key, const std::string& member, bool& exists) {
        auto staged_it = staged_sets.find(set_key);
        if (staged_it != staged_sets.end()) {
            exists = staged_it->second.find(member) != staged_it->second.end();
            return mako::Status::OK();
        }
        std::string member_key = set_storage_key(set_key, member);
        auto batch_it = batch_exists.find(member_key);
        if (batch_it != batch_exists.end()) {
            exists = batch_it->second;
            return mako::Status::OK();
        }
        return read_raw_exists(txn, member_key, exists);
    };

    auto set_key_allowed = [&](void* txn, const std::string& set_key, TxnOpResult& result, bool& allowed) {
        std::string string_storage_key = "table_key_" + set_key;
        mako::Status s = expire_logical_key_if_needed(txn, set_key, string_storage_key);
        if (!s.ok()) {
            return s;
        }
        bool string_exists = false;
        s = read_string_exists_no_expire(txn, string_storage_key, string_exists);
        if (!s.ok()) {
            return s;
        }
        int64_t list_length = 0;
        s = read_list_length(txn, set_key, list_length);
        if (!s.ok()) {
            return s;
        }
        int64_t zset_count = 0;
        s = read_zset_cardinality(txn, set_key, zset_count);
        if (!s.ok()) {
            return s;
        }
        int64_t hash_count = 0;
        s = read_hash_cardinality(txn, set_key, hash_count);
        if (!s.ok()) {
            return s;
        }
        bool stream_here = false;
        s = stream_exists(txn, set_key, stream_here);
        if (!s.ok()) {
            return s;
        }
        allowed = !string_exists && list_length == 0 && zset_count == 0 && hash_count == 0;
        allowed = allowed && !stream_here;
        if (!allowed) {
            result.success = false;
        }
        return mako::Status::OK();
    };

    auto list_key_allowed = [&](void* txn, const std::string& list_key, TxnOpResult& result, bool& allowed) {
        std::string string_storage_key = "table_key_" + list_key;
        mako::Status s = expire_logical_key_if_needed(txn, list_key, string_storage_key);
        if (!s.ok()) {
            return s;
        }
        bool string_exists = false;
        s = read_string_exists_no_expire(txn, string_storage_key, string_exists);
        if (!s.ok()) {
            return s;
        }
        int64_t set_cardinality = 0;
        s = read_set_cardinality(txn, list_key, set_cardinality);
        if (!s.ok()) {
            return s;
        }
        int64_t zset_count = 0;
        s = read_zset_cardinality(txn, list_key, zset_count);
        if (!s.ok()) {
            return s;
        }
        int64_t hash_count = 0;
        s = read_hash_cardinality(txn, list_key, hash_count);
        if (!s.ok()) {
            return s;
        }
        bool stream_here = false;
        s = stream_exists(txn, list_key, stream_here);
        if (!s.ok()) {
            return s;
        }
        allowed = !string_exists && set_cardinality == 0 && zset_count == 0 && hash_count == 0;
        allowed = allowed && !stream_here;
        if (!allowed) {
            result.success = false;
        }
        return mako::Status::OK();
    };

    auto zset_key_allowed = [&](void* txn, const std::string& zset_key, TxnOpResult& result, bool& allowed) {
        std::string string_storage_key = "table_key_" + zset_key;
        mako::Status s = expire_logical_key_if_needed(txn, zset_key, string_storage_key);
        if (!s.ok()) {
            return s;
        }
        bool string_exists = false;
        s = read_string_exists_no_expire(txn, string_storage_key, string_exists);
        if (!s.ok()) {
            return s;
        }
        int64_t set_cardinality = 0;
        s = read_set_cardinality(txn, zset_key, set_cardinality);
        if (!s.ok()) {
            return s;
        }
        int64_t list_length = 0;
        s = read_list_length(txn, zset_key, list_length);
        if (!s.ok()) {
            return s;
        }
        int64_t hash_count = 0;
        s = read_hash_cardinality(txn, zset_key, hash_count);
        if (!s.ok()) {
            return s;
        }
        bool stream_here = false;
        s = stream_exists(txn, zset_key, stream_here);
        if (!s.ok()) {
            return s;
        }
        allowed = !string_exists && set_cardinality == 0 && list_length == 0 && hash_count == 0;
        allowed = allowed && !stream_here;
        if (!allowed) {
            result.success = false;
        }
        return mako::Status::OK();
    };

    auto hash_key_allowed = [&](void* txn, const std::string& hash_key, TxnOpResult& result, bool& allowed) {
        std::string string_storage_key = "table_key_" + hash_key;
        mako::Status s = expire_logical_key_if_needed(txn, hash_key, string_storage_key);
        if (!s.ok()) {
            return s;
        }
        bool string_exists = false;
        s = read_string_exists_no_expire(txn, string_storage_key, string_exists);
        if (!s.ok()) {
            return s;
        }
        int64_t set_cardinality = 0;
        s = read_set_cardinality(txn, hash_key, set_cardinality);
        if (!s.ok()) {
            return s;
        }
        int64_t list_length = 0;
        s = read_list_length(txn, hash_key, list_length);
        if (!s.ok()) {
            return s;
        }
        int64_t zset_count = 0;
        s = read_zset_cardinality(txn, hash_key, zset_count);
        if (!s.ok()) {
            return s;
        }
        bool stream_here = false;
        s = stream_exists(txn, hash_key, stream_here);
        if (!s.ok()) {
            return s;
        }
        allowed = !string_exists && set_cardinality == 0 && list_length == 0 && zset_count == 0
            && !stream_here;
        if (!allowed) {
            result.success = false;
        }
        return mako::Status::OK();
    };

    auto string_key_allowed = [&](void* txn, const std::string& user_key, const std::string& storage_key, TxnOpResult& result, bool& allowed) {
        mako::Status s = expire_logical_key_if_needed(txn, user_key, storage_key);
        if (!s.ok()) {
            return s;
        }
        int64_t set_cardinality = 0;
        s = read_set_cardinality(txn, user_key, set_cardinality);
        if (!s.ok()) {
            return s;
        }
        int64_t list_length = 0;
        auto staged_list_it = staged_lists.find(user_key);
        if (staged_list_it != staged_lists.end()) {
            list_length = static_cast<int64_t>(staged_list_it->second.size());
        } else {
            s = read_list_length(txn, user_key, list_length);
            if (!s.ok()) {
                return s;
            }
        }
        int64_t zset_count = 0;
        auto staged_zset_it = staged_zsets.find(user_key);
        if (staged_zset_it != staged_zsets.end()) {
            zset_count = static_cast<int64_t>(staged_zset_it->second.size());
        } else {
            s = read_zset_cardinality(txn, user_key, zset_count);
            if (!s.ok()) {
                return s;
            }
        }
        int64_t hash_count = 0;
        s = read_hash_cardinality(txn, user_key, hash_count);
        if (!s.ok()) {
            return s;
        }
        bool stream_here = false;
        s = stream_exists(txn, user_key, stream_here);
        if (!s.ok()) {
            return s;
        }
        allowed = set_cardinality == 0 && list_length == 0 && zset_count == 0 && hash_count == 0
            && !stream_here;
        if (!allowed) {
            result.success = false;
        }
        return mako::Status::OK();
    };

    auto stream_key_allowed = [&](void* txn, const std::string& stream_key, TxnOpResult& result, bool& allowed) {
        std::string string_storage_key = "table_key_" + stream_key;
        mako::Status s = expire_logical_key_if_needed(txn, stream_key, string_storage_key);
        if (!s.ok()) {
            return s;
        }
        bool string_exists = false;
        s = read_string_exists_no_expire(txn, string_storage_key, string_exists);
        if (!s.ok()) {
            return s;
        }
        int64_t set_cardinality = 0;
        s = read_set_cardinality(txn, stream_key, set_cardinality);
        if (!s.ok()) {
            return s;
        }
        int64_t list_length = 0;
        s = read_list_length(txn, stream_key, list_length);
        if (!s.ok()) {
            return s;
        }
        int64_t zset_count = 0;
        s = read_zset_cardinality(txn, stream_key, zset_count);
        if (!s.ok()) {
            return s;
        }
        int64_t hash_count = 0;
        s = read_hash_cardinality(txn, stream_key, hash_count);
        if (!s.ok()) {
            return s;
        }
        allowed = !string_exists && set_cardinality == 0 && list_length == 0
            && zset_count == 0 && hash_count == 0;
        if (!allowed) {
            result.success = false;
        }
        return mako::Status::OK();
    };

    auto load_zset_stage = [&](void* txn, const std::string& zset_key, TxnOpResult& result, bool& allowed) {
        auto loaded_it = staged_zsets_loaded.find(zset_key);
        if (loaded_it != staged_zsets_loaded.end()) {
            allowed = true;
            return mako::Status::OK();
        }
        mako::Status s = zset_key_allowed(txn, zset_key, result, allowed);
        if (!s.ok() || !allowed) {
            return s;
        }
        s = expire_zset_if_needed(txn, zset_key);
        if (!s.ok()) {
            return s;
        }
        std::map<std::string, double> values;
        s = collect_zset_values(txn, zset_key, values);
        if (s.ok()) {
            staged_zsets[zset_key] = std::move(values);
            staged_zsets_loaded.insert(zset_key);
        }
        return s;
    };

    auto load_list_stage = [&](void* txn, const std::string& list_key, TxnOpResult& result, bool& allowed) {
        auto loaded_it = staged_lists_loaded.find(list_key);
        if (loaded_it != staged_lists_loaded.end()) {
            allowed = true;
            return mako::Status::OK();
        }
        mako::Status s = list_key_allowed(txn, list_key, result, allowed);
        if (!s.ok() || !allowed) {
            return s;
        }
        s = expire_list_if_needed(txn, list_key);
        if (!s.ok()) {
            return s;
        }
        std::vector<std::string> values;
        s = read_list_values(txn, list_key, values);
        if (s.ok()) {
            staged_lists[list_key] = std::move(values);
            staged_lists_loaded.insert(list_key);
        }
        return s;
    };

    auto load_set_stage = [&](void* txn, const std::string& set_key, TxnOpResult& result, bool& allowed) {
        auto loaded_it = staged_sets_loaded.find(set_key);
        if (loaded_it != staged_sets_loaded.end()) {
            allowed = true;
            return mako::Status::OK();
        }
        mako::Status s = set_key_allowed(txn, set_key, result, allowed);
        if (!s.ok() || !allowed) {
            return s;
        }
        s = expire_set_if_needed(txn, set_key);
        if (!s.ok()) {
            return s;
        }
        std::vector<std::string> members;
        s = collect_set_members(txn, set_key, members);
        if (s.ok()) {
            staged_sets[set_key] = std::unordered_set<std::string>(members.begin(), members.end());
            staged_sets_loaded.insert(set_key);
        }
        return s;
    };

    auto read_set_members = [&](void* txn, const std::string& set_key, std::vector<std::string>& members) {
        auto staged_it = staged_sets.find(set_key);
        if (staged_it != staged_sets.end()) {
            members.assign(staged_it->second.begin(), staged_it->second.end());
            return mako::Status::OK();
        }
        bool allowed = false;
        TxnOpResult ignored{};
        mako::Status type_status = set_key_allowed(txn, set_key, ignored, allowed);
        if (!type_status.ok()) {
            members.clear();
            return type_status;
        }
        if (!allowed) {
            members.clear();
            return mako::Status::InvalidArgument("wrong type");
        }
        mako::Status expire_status = expire_set_if_needed(txn, set_key);
        if (!expire_status.ok()) {
            return expire_status;
        }
        auto meta_it = batch_exists.find(set_meta_storage_key(set_key));
        if (meta_it != batch_exists.end() && !meta_it->second) {
            members.clear();
            return mako::Status::OK();
        }
        return collect_set_members(txn, set_key, members);
    };

    auto read_user_exists = [&](void* txn, const std::string& user_key, const std::string& storage_key, bool& exists) {
        auto batch_it = batch_exists.find(storage_key);
        if (batch_it != batch_exists.end()) {
            exists = batch_it->second;
            return mako::Status::OK();
        }
        mako::Status expire_status = expire_if_needed(txn, user_key, storage_key);
        if (!expire_status.ok()) {
            return expire_status;
        }
        batch_it = batch_exists.find(storage_key);
        if (batch_it != batch_exists.end()) {
            exists = batch_it->second;
            return mako::Status::OK();
        }
        return read_raw_exists(txn, storage_key, exists);
    };

    auto add_int64 = [](int64_t base, int64_t delta, int64_t& out) {
        if ((delta > 0 && base > std::numeric_limits<int64_t>::max() - delta)
            || (delta < 0 && base < std::numeric_limits<int64_t>::min() - delta)) {
            return false;
        }
        out = base + delta;
        return true;
    };

    auto parse_float = [](const std::string& input, long double& out) {
        if (input.empty() || std::isspace(static_cast<unsigned char>(input.front()))) {
            return false;
        }
        errno = 0;
        char* end = nullptr;
        long double parsed = std::strtold(input.c_str(), &end);
        if (end == input.c_str() || end != input.c_str() + input.size() || !std::isfinite(parsed)) {
            return false;
        }
        out = parsed;
        return true;
    };

    // INCRBYFLOAT's parser, matching Redis's string2ld: the infinity spellings
    // are accepted as values, NaN is not, and a strtold overflow is a parse
    // failure. Accepting the infinities is what makes "INCRBYFLOAT k +inf"
    // answer "increment would produce NaN or Infinity" like Redis, rather than
    // "value is not a valid float". parse_float, which every other command
    // uses, keeps rejecting anything non-finite.
    auto parse_incr_float = [](const std::string& input, long double& out) {
        if (input.empty() || std::isspace(static_cast<unsigned char>(input.front()))) {
            return false;
        }
        std::string lowered;
        lowered.reserve(input.size());
        for (char ch : input) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (lowered == "inf" || lowered == "+inf"
            || lowered == "infinity" || lowered == "+infinity") {
            out = std::numeric_limits<long double>::infinity();
            return true;
        }
        if (lowered == "-inf" || lowered == "-infinity") {
            out = -std::numeric_limits<long double>::infinity();
            return true;
        }
        errno = 0;
        char* end = nullptr;
        long double parsed = std::strtold(input.c_str(), &end);
        if (end == input.c_str() || end != input.c_str() + input.size()
            || std::isnan(parsed) || !std::isfinite(parsed)) {
            return false;
        }
        out = parsed;
        return true;
    };

    auto format_float = [](long double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(17) << value;
        std::string out = oss.str();
        if (out.find('.') != std::string::npos) {
            while (!out.empty() && out.back() == '0') {
                out.pop_back();
            }
            if (!out.empty() && out.back() == '.') {
                out.pop_back();
            }
        }
        if (out == "-0") {
            out = "0";
        }
        return out;
    };

    try {
        // Execute each operation within the transaction
        for (size_t i = 0; i < request->num_ops; i++) {
            const TxnOperation& op = request->ops[i];
            TxnOpResult& result = response->results[i];
            result.success = false;
            result.value_present = false;
            result.data_ptr = nullptr;
            result.data_len = 0;

            // Build key with prefix
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + op.key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(op.key_ptr), op.key_len);
            std::string user_key(reinterpret_cast<const char*>(op.key_ptr), op.key_len);

            if (op.op == TXN_OP_GET) {
                bool exists = false;
                std::string current;
                mako::Status s = read_current(txn, user_key, tl_key_buf, current, exists);
                result.success = s.ok();
                if (!s.ok()) {
                    all_success = false;
                } else if (exists && !copy_result_value(result, current)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SET) {
                std::string old_value;
                bool string_existed = false;
                bool logical_existed = false;
                bool expired_for_set = false;
                int64_t set_expire_at_ms = 0;
                bool set_ttl_exists = false;
                mako::Status s = read_ttl_meta(txn, user_key, set_expire_at_ms, set_ttl_exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (set_ttl_exists && set_expire_at_ms <= now_unix_ms()) {
                    expired_for_set = true;
                    s = clear_ttl_meta(txn, user_key);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                } else {
                    s = read_current(txn, user_key, tl_key_buf, old_value, string_existed);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (string_existed) {
                        logical_existed = true;
                    } else {
                        s = read_logical_exists(txn, user_key, tl_key_buf, logical_existed);
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                    }
                }

                bool group_write = true;
                if ((op.flags & TXN_FLAG_SET_REQUIRE_ABSENT_GROUP) != 0 && op.group_id != 0) {
                    auto group_it = group_can_write.find(op.group_id);
                    if (group_it == group_can_write.end()) {
                        bool can_write = true;
                        for (size_t j = i; j < request->num_ops; ++j) {
                            const TxnOperation& group_op = request->ops[j];
                            if (group_op.group_id != op.group_id) {
                                continue;
                            }
                            std::string group_key = make_prefixed_key(group_op);
                            std::string group_user_key(
                                reinterpret_cast<const char*>(group_op.key_ptr),
                                group_op.key_len);
                            bool group_exists = false;
                            mako::Status group_status = read_logical_exists(
                                txn, group_user_key, group_key, group_exists);
                            if (!group_status.ok()) {
                                all_success = false;
                                can_write = false;
                                break;
                            }
                            if (group_exists) {
                                can_write = false;
                                break;
                            }
                        }
                        group_can_write[op.group_id] = can_write;
                        group_write = can_write;
                    } else {
                        group_write = group_it->second;
                    }
                }

                bool write_allowed = group_write;
                const bool absent_group =
                    (op.flags & TXN_FLAG_SET_REQUIRE_ABSENT_GROUP) != 0 && op.group_id != 0;
                if ((op.flags & TXN_FLAG_SET_RETURN_OLD) != 0 && logical_existed && !string_existed) {
                    result.success = false;
                    continue;
                }
                if ((op.flags & TXN_FLAG_SET_NX) != 0 && logical_existed && !absent_group) {
                    write_allowed = false;
                }
                if ((op.flags & TXN_FLAG_SET_XX) != 0 && !logical_existed) {
                    write_allowed = false;
                }

                result.success = true;
                if ((op.flags & TXN_FLAG_SET_RETURN_OLD) != 0 && string_existed) {
                    if (!copy_result_value(result, old_value)) {
                        all_success = false;
                        continue;
                    }
                }

                if (!write_allowed && expired_for_set) {
                    s = delete_raw_if_exists(txn, tl_key_buf);
                    if (s.ok()) {
                        s = delete_set(txn, user_key);
                    }
                    if (s.ok()) {
                        s = delete_list(txn, user_key);
                    }
                    if (s.ok()) {
                        s = delete_zset(txn, user_key);
                    }
                    if (s.ok()) {
                        s = delete_hash(txn, user_key);
                    }
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                }

                if (write_allowed) {
                    std::string raw_val(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                    const bool replacing_non_string = expired_for_set || (logical_existed && !string_existed);
                    if (replacing_non_string) {
                        s = delete_set(txn, user_key);
                        if (!s.ok()) {
                            result.success = false;
                            all_success = false;
                            continue;
                        }
                        s = delete_list(txn, user_key);
                        if (!s.ok()) {
                            result.success = false;
                            all_success = false;
                            continue;
                        }
                        s = delete_zset(txn, user_key);
                        if (!s.ok()) {
                            result.success = false;
                            all_success = false;
                            continue;
                        }
                        s = delete_hash(txn, user_key);
                        if (!s.ok()) {
                            result.success = false;
                            all_success = false;
                            continue;
                        }
                    }
                    s = put_raw(txn, tl_key_buf, raw_val);
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    if (op.expire_at_ms >= 0) {
                        s = write_ttl_meta(txn, user_key, op.expire_at_ms);
                    } else if ((op.flags & TXN_FLAG_SET_KEEP_TTL) == 0) {
                        s = clear_ttl_meta(txn, user_key);
                    } else {
                        s = mako::Status::OK();
                    }
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    batch_exists[tl_key_buf] = true;
                    batch_values[tl_key_buf] = raw_val;
                }
                if ((op.flags & TXN_FLAG_SET_RETURN_OLD) == 0) {
                    result.value_present = write_allowed;
                }
            } else if (op.op == TXN_OP_DEL) {
                // DEL operation
                // @unsafe { redis_table_delete calls non-borrow-checked Masstree code }
                bool exists = false;
                mako::Status exists_status = read_logical_exists(txn, user_key, tl_key_buf, exists);
                if (!exists_status.ok()) {
                    all_success = false;
                    continue;
                }
                bool string_exists = false;
                mako::Status string_status = read_string_exists_no_expire(txn, tl_key_buf, string_exists);
                if (!string_status.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t set_cardinality = 0;
                mako::Status set_status = read_set_cardinality(txn, user_key, set_cardinality);
                if (!set_status.ok()) {
                    all_success = false;
                    continue;
                }
                bool set_exists = set_cardinality > 0;
                int64_t list_length = 0;
                mako::Status list_status = read_list_length(txn, user_key, list_length);
                if (!list_status.ok()) {
                    all_success = false;
                    continue;
                }
                auto staged_list_it = staged_lists.find(user_key);
                if (staged_list_it != staged_lists.end()) {
                    list_length = static_cast<int64_t>(staged_list_it->second.size());
                }
                bool list_exists = list_length > 0;
                int64_t zset_count = 0;
                mako::Status zset_status = read_zset_cardinality(txn, user_key, zset_count);
                if (!zset_status.ok()) {
                    all_success = false;
                    continue;
                }
                auto staged_zset_it = staged_zsets.find(user_key);
                if (staged_zset_it != staged_zsets.end()) {
                    zset_count = static_cast<int64_t>(staged_zset_it->second.size());
                }
                bool zset_exists = zset_count > 0;
                int64_t hash_count = 0;
                mako::Status hash_status = read_hash_cardinality(txn, user_key, hash_count);
                if (!hash_status.ok()) {
                    all_success = false;
                    continue;
                }
                bool hash_exists = hash_count > 0;
                bool stream_here = false;
                mako::Status stream_status = stream_exists(txn, user_key, stream_here);
                if (!stream_status.ok()) {
                    all_success = false;
                    continue;
                }
                mako::Status s = mako::Status::OK();
                if (set_exists) {
                    s = delete_set(txn, user_key);
                }
                if (s.ok() && list_exists) {
                    s = delete_list(txn, user_key);
                }
                if (s.ok() && zset_exists) {
                    s = delete_zset(txn, user_key);
                }
                if (s.ok() && hash_exists) {
                    s = delete_hash(txn, user_key);
                }
                if (s.ok() && stream_here) {
                    s = delete_stream(txn, user_key);
                }
                if (s.ok() && string_exists) {
                    s = delete_raw_if_exists(txn, tl_key_buf);
                }
                result.success = s.ok();
                result.value_present = exists;
                if (!s.ok()) {
                    all_success = false;
                } else {
                    if (exists) {
                        s = clear_ttl_meta(txn, user_key);
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                    }
                    batch_exists[tl_key_buf] = false;
                    batch_values.erase(tl_key_buf);
                }
            } else if (op.op == TXN_OP_EXISTS) {
                bool exists = false;
                mako::Status s = read_logical_exists(txn, user_key, tl_key_buf, exists);
                result.success = s.ok();
                result.value_present = exists;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_APPEND) {
                std::string current;
                bool exists = false;
                mako::Status s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                std::string suffix(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                std::string next = exists ? current + suffix : suffix;
                s = put_raw(txn, tl_key_buf, next);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = static_cast<int64_t>(next.size());
                if (!s.ok()) {
                    all_success = false;
                } else {
                    batch_exists[tl_key_buf] = true;
                    batch_values[tl_key_buf] = next;
                }
            } else if (op.op == TXN_OP_STRLEN) {
                std::string current;
                bool exists = false;
                mako::Status s = read_current(txn, user_key, tl_key_buf, current, exists);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = exists ? static_cast<int64_t>(current.size()) : 0;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SETBIT || op.op == TXN_OP_GETBIT) {
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string current;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                const uint64_t bit_offset = static_cast<uint64_t>(op.expire_at_ms);
                const size_t byte_index = static_cast<size_t>(bit_offset / 8);
                const uint8_t mask = static_cast<uint8_t>(1u << (7 - (bit_offset % 8)));
                const int64_t old_bit =
                    exists && byte_index < current.size()
                        ? ((static_cast<uint8_t>(current[byte_index]) & mask) != 0)
                        : 0;
                result.success = true;
                result.value_present = true;
                result.int_value = old_bit;
                if (op.op == TXN_OP_SETBIT) {
                    std::string bit_value(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                    const bool set_bit = bit_value == "1";
                    if (current.size() <= byte_index) {
                        current.resize(byte_index + 1, '\0');
                    }
                    uint8_t byte = static_cast<uint8_t>(current[byte_index]);
                    if (set_bit) {
                        byte |= mask;
                    } else {
                        byte &= static_cast<uint8_t>(~mask);
                    }
                    current[byte_index] = static_cast<char>(byte);
                    s = put_raw(txn, tl_key_buf, current);
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                    } else {
                        batch_exists[tl_key_buf] = true;
                        batch_values[tl_key_buf] = current;
                    }
                }
            } else if (op.op == TXN_OP_SETRANGE || op.op == TXN_OP_GETRANGE) {
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string current;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (op.op == TXN_OP_SETRANGE) {
                    std::string patch(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                    if (!exists && patch.empty()) {
                        result.success = true;
                        result.value_present = true;
                        result.int_value = 0;
                        continue;
                    }
                    const size_t offset = static_cast<size_t>(op.expire_at_ms);
                    if (!patch.empty()) {
                        const size_t needed = offset + patch.size();
                        if (current.size() < needed) {
                            current.resize(needed, '\0');
                        }
                        std::copy(patch.begin(), patch.end(), current.begin() + offset);
                        s = put_raw(txn, tl_key_buf, current);
                        if (!s.ok()) {
                            result.success = false;
                            all_success = false;
                            continue;
                        }
                        batch_exists[tl_key_buf] = true;
                        batch_values[tl_key_buf] = current;
                    }
                    result.success = true;
                    result.value_present = true;
                    result.int_value = static_cast<int64_t>(current.size());
                } else {
                    std::vector<std::string> bounds;
                    if (!unpack_bytes_list(op.val_ptr, op.val_len, bounds) || bounds.size() != 2) {
                        all_success = false;
                        continue;
                    }
                    int64_t start_index = 0;
                    int64_t stop_index = 0;
                    if (!parse_int64(bounds[0], start_index) || !parse_int64(bounds[1], stop_index)) {
                        all_success = false;
                        continue;
                    }
                    const int64_t length = exists ? static_cast<int64_t>(current.size()) : 0;
                    if (start_index < 0) {
                        start_index += length;
                    }
                    if (stop_index < 0) {
                        stop_index += length;
                    }
                    start_index = std::max<int64_t>(0, start_index);
                    stop_index = std::min<int64_t>(length - 1, stop_index);
                    std::string selected;
                    if (length > 0 && start_index <= stop_index && start_index < length) {
                        selected.assign(
                            current.begin() + static_cast<size_t>(start_index),
                            current.begin() + static_cast<size_t>(stop_index + 1));
                    }
                    result.success = true;
                    result.value_present = true;
                    if (!copy_result_value(result, selected)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_INCRBY) {
                // A key holding another type is WRONGTYPE, not a silent
                // overwrite of the collection with a string.
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string current;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t base = 0;
                int64_t delta = 0;
                std::string delta_str(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                // A bad stored value or a bad increment is a command error:
                // report it to Rust and leave the rest of the transaction
                // alone, instead of aborting it into "ERR backend".
                if ((exists && !parse_int64(current, base)) || !parse_int64(delta_str, delta)) {
                    result.success = false;
                    result.int_value = TXN_INCR_ERR_NOT_INTEGER;
                    continue;
                }
                int64_t next = 0;
                if (!add_int64(base, delta, next)) {
                    result.success = false;
                    result.int_value = TXN_INCR_ERR_OVERFLOW;
                    continue;
                }
                std::string next_str = std::to_string(next);
                s = put_raw(txn, tl_key_buf, next_str);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = next;
                if (!s.ok()) {
                    all_success = false;
                } else {
                    batch_exists[tl_key_buf] = true;
                    batch_values[tl_key_buf] = next_str;
                }
            } else if (op.op == TXN_OP_INCRBYFLOAT) {
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string current;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                long double base = 0;
                long double delta = 0;
                std::string delta_str(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                if ((exists && !parse_incr_float(current, base))
                    || !parse_incr_float(delta_str, delta)) {
                    result.success = false;
                    result.int_value = TXN_INCR_ERR_NOT_FLOAT;
                    continue;
                }
                long double next = base + delta;
                if (!std::isfinite(next)) {
                    result.success = false;
                    result.int_value = TXN_INCR_ERR_NAN_OR_INF;
                    continue;
                }
                std::string next_str = format_float(next);
                s = put_raw(txn, tl_key_buf, next_str);
                result.success = s.ok();
                if (!s.ok()) {
                    all_success = false;
                } else if (!copy_result_value(result, next_str)) {
                    all_success = false;
                } else {
                    batch_exists[tl_key_buf] = true;
                    batch_values[tl_key_buf] = next_str;
                }
            } else if (op.op == TXN_OP_EXPIRE) {
                bool exists = false;
                mako::Status s = read_logical_exists(txn, user_key, tl_key_buf, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.int_value = 0;
                result.value_present = exists;
                if (!exists) {
                    continue;
                }
                int64_t current_expire_at_ms = 0;
                bool ttl_exists = false;
                s = read_ttl_meta(txn, user_key, current_expire_at_ms, ttl_exists);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                bool should_update = true;
                if ((op.flags & TXN_FLAG_EXPIRE_NX) != 0) {
                    should_update = !ttl_exists;
                }
                if ((op.flags & TXN_FLAG_EXPIRE_XX) != 0) {
                    should_update = should_update && ttl_exists;
                }
                if ((op.flags & TXN_FLAG_EXPIRE_GT) != 0) {
                    should_update = should_update && ttl_exists
                        && op.expire_at_ms > current_expire_at_ms;
                }
                if ((op.flags & TXN_FLAG_EXPIRE_LT) != 0) {
                    should_update = should_update
                        && (!ttl_exists || op.expire_at_ms < current_expire_at_ms);
                }
                if (!should_update) {
                    continue;
                }
                result.int_value = 1;
                if (op.expire_at_ms <= now_unix_ms()) {
                    auto staged_list_it = staged_lists.find(user_key);
                    if (staged_list_it != staged_lists.end() && !staged_list_it->second.empty()) {
                        staged_list_it->second.clear();
                        dirty_lists.insert(user_key);
                        s = mako::Status::OK();
                    } else {
                        auto staged_zset_it = staged_zsets.find(user_key);
                        if (staged_zset_it != staged_zsets.end() && !staged_zset_it->second.empty()) {
                            staged_zset_it->second.clear();
                            dirty_zsets.insert(user_key);
                            s = mako::Status::OK();
                        } else {
                            int64_t set_cardinality = 0;
                            mako::Status set_status = read_set_cardinality(txn, user_key, set_cardinality);
                            if (set_status.ok() && set_cardinality > 0) {
                                s = delete_set(txn, user_key);
                            } else {
                                int64_t list_length = 0;
                                mako::Status list_status = read_list_length(txn, user_key, list_length);
                                if (list_status.ok() && list_length > 0) {
                                    s = delete_list(txn, user_key);
                                } else {
                                    int64_t zset_count = 0;
                                    mako::Status zset_status = read_zset_cardinality(txn, user_key, zset_count);
                                    if (zset_status.ok() && zset_count > 0) {
                                        s = delete_zset(txn, user_key);
                                    } else {
                                        int64_t hash_count = 0;
                                        mako::Status hash_status = read_hash_cardinality(txn, user_key, hash_count);
                                        if (hash_status.ok() && hash_count > 0) {
                                            s = delete_hash(txn, user_key);
                                        } else {
                                            s = delete_raw_if_exists(txn, tl_key_buf);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (s.ok()) {
                        s = clear_ttl_meta(txn, user_key);
                    }
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    batch_exists[tl_key_buf] = false;
                    batch_values.erase(tl_key_buf);
                } else {
                    s = write_ttl_meta(txn, user_key, op.expire_at_ms);
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                }
            } else if (op.op == TXN_OP_TTL) {
                bool exists = false;
                mako::Status s = read_logical_exists(txn, user_key, tl_key_buf, exists);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                if (!exists) {
                    result.int_value = -2;
                    continue;
                }
                int64_t expire_at_ms = 0;
                bool ttl_exists = false;
                s = read_ttl_meta(txn, user_key, expire_at_ms, ttl_exists);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                if (!ttl_exists) {
                    result.int_value = -1;
                    continue;
                }
                const int64_t remaining_ms = expire_at_ms - now_unix_ms();
                if (remaining_ms <= 0) {
                    auto staged_list_it = staged_lists.find(user_key);
                    if (staged_list_it != staged_lists.end() && !staged_list_it->second.empty()) {
                        staged_list_it->second.clear();
                        dirty_lists.insert(user_key);
                        s = mako::Status::OK();
                    } else {
                        auto staged_zset_it = staged_zsets.find(user_key);
                        if (staged_zset_it != staged_zsets.end() && !staged_zset_it->second.empty()) {
                            staged_zset_it->second.clear();
                            dirty_zsets.insert(user_key);
                            s = mako::Status::OK();
                        } else {
                            int64_t set_cardinality = 0;
                            mako::Status set_status = read_set_cardinality(txn, user_key, set_cardinality);
                            if (set_status.ok() && set_cardinality > 0) {
                                s = delete_set(txn, user_key);
                            } else {
                                int64_t list_length = 0;
                                mako::Status list_status = read_list_length(txn, user_key, list_length);
                                if (list_status.ok() && list_length > 0) {
                                    s = delete_list(txn, user_key);
                                } else {
                                    int64_t zset_count = 0;
                                    mako::Status zset_status = read_zset_cardinality(txn, user_key, zset_count);
                                    if (zset_status.ok() && zset_count > 0) {
                                        s = delete_zset(txn, user_key);
                                    } else {
                                        int64_t hash_count = 0;
                                        mako::Status hash_status = read_hash_cardinality(txn, user_key, hash_count);
                                        if (hash_status.ok() && hash_count > 0) {
                                            s = delete_hash(txn, user_key);
                                        } else {
                                            s = delete_raw_if_exists(txn, tl_key_buf);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (s.ok()) {
                        s = clear_ttl_meta(txn, user_key);
                    }
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    batch_exists[tl_key_buf] = false;
                    batch_values.erase(tl_key_buf);
                    result.int_value = -2;
                } else if ((op.flags & TXN_FLAG_TTL_MILLISECONDS) != 0) {
                    result.int_value = remaining_ms;
                } else {
                    result.int_value = (remaining_ms + 999) / 1000;
                }
            } else if (op.op == TXN_OP_PERSIST) {
                bool exists = false;
                mako::Status s = read_logical_exists(txn, user_key, tl_key_buf, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = exists;
                if (!exists) {
                    result.int_value = 0;
                    continue;
                }
                int64_t expire_at_ms = 0;
                bool ttl_exists = false;
                s = read_ttl_meta(txn, user_key, expire_at_ms, ttl_exists);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                if (!ttl_exists) {
                    result.int_value = 0;
                    continue;
                }
                s = clear_ttl_meta(txn, user_key);
                result.success = s.ok();
                result.int_value = s.ok() ? 1 : 0;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_TYPE) {
                mako::Status s = expire_logical_key_if_needed(txn, user_key, tl_key_buf);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                bool string_exists = false;
                s = read_string_exists_no_expire(txn, tl_key_buf, string_exists);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                int64_t set_count = 0;
                s = read_set_cardinality(txn, user_key, set_count);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                int64_t list_length = 0;
                auto staged_list_it = staged_lists.find(user_key);
                if (staged_list_it != staged_lists.end()) {
                    list_length = static_cast<int64_t>(staged_list_it->second.size());
                    s = mako::Status::OK();
                } else {
                    s = read_list_length(txn, user_key, list_length);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                int64_t zset_count = 0;
                auto staged_zset_it = staged_zsets.find(user_key);
                if (staged_zset_it != staged_zsets.end()) {
                    zset_count = static_cast<int64_t>(staged_zset_it->second.size());
                    s = mako::Status::OK();
                } else {
                    s = read_zset_cardinality(txn, user_key, zset_count);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                int64_t hash_count = 0;
                s = read_hash_cardinality_live(txn, user_key, hash_count);
                bool stream_here = false;
                if (s.ok()) {
                    s = stream_exists(txn, user_key, stream_here);
                }
                result.success = s.ok();
                result.value_present = true;
                result.int_value = string_exists ? 1
                    : (set_count > 0 ? 2
                    : (list_length > 0 ? 3
                    : (zset_count > 0 ? 4
                    : (hash_count > 0 ? 5
                    : (stream_here ? 6 : 0)))));
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_RENAME) {
                std::string destination(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                std::string source_storage_key = "table_key_" + user_key;
                std::string destination_storage_key = "table_key_" + destination;
                mako::Status s = expire_logical_key_if_needed(txn, user_key, source_storage_key);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }

                bool string_exists = false;
                s = read_string_exists_no_expire(txn, source_storage_key, string_exists);
                int64_t set_cardinality = 0;
                if (s.ok()) {
                    s = read_set_cardinality(txn, user_key, set_cardinality);
                }
                int64_t list_length = 0;
                if (s.ok()) {
                    s = read_list_length(txn, user_key, list_length);
                }
                int64_t zset_count = 0;
                if (s.ok()) {
                    s = read_zset_cardinality(txn, user_key, zset_count);
                }
                int64_t hash_count = 0;
                if (s.ok()) {
                    s = read_hash_cardinality(txn, user_key, hash_count);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                bool source_stream = false;
                s = stream_exists(txn, user_key, source_stream);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                if (!string_exists && set_cardinality == 0 && list_length == 0
                    && zset_count == 0 && hash_count == 0 && !source_stream) {
                    result.success = false;
                    result.int_value = -1;
                    continue;
                }
                const bool rename_nx = op.expire_at_ms == 1;
                if (user_key == destination) {
                    result.success = true;
                    result.value_present = true;
                    result.int_value = rename_nx ? 0 : 1;
                    continue;
                }
                if (rename_nx) {
                    mako::Status ds = expire_logical_key_if_needed(txn, destination, destination_storage_key);
                    bool destination_string_exists = false;
                    if (ds.ok()) {
                        ds = read_string_exists_no_expire(txn, destination_storage_key, destination_string_exists);
                    }
                    int64_t destination_set_cardinality = 0;
                    if (ds.ok()) {
                        ds = read_set_cardinality(txn, destination, destination_set_cardinality);
                    }
                    int64_t destination_list_length = 0;
                    if (ds.ok()) {
                        ds = read_list_length(txn, destination, destination_list_length);
                    }
                    int64_t destination_zset_count = 0;
                    if (ds.ok()) {
                        ds = read_zset_cardinality(txn, destination, destination_zset_count);
                    }
                    int64_t destination_hash_count = 0;
                    if (ds.ok()) {
                        ds = read_hash_cardinality(txn, destination, destination_hash_count);
                    }
                    bool destination_stream = false;
                    if (ds.ok()) {
                        ds = stream_exists(txn, destination, destination_stream);
                    }
                    if (!ds.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    if (destination_string_exists || destination_set_cardinality > 0
                        || destination_list_length > 0 || destination_zset_count > 0
                        || destination_hash_count > 0 || destination_stream) {
                        result.success = true;
                        result.value_present = true;
                        result.int_value = 0;
                        continue;
                    }
                }

                std::string string_value;
                std::vector<std::string> set_members;
                std::vector<std::string> list_values;
                std::map<std::string, double> zset_values;
                std::map<std::string, std::string> hash_entries;
                // RENAME and COPY move the whole object, so the per-field
                // expirations travel with the fields, and a stream carries its
                // groups, consumers and pending-entry lists.
                std::map<std::string, int64_t> hash_field_ttls;
                std::vector<std::string> stream_records;
                if (string_exists) {
                    bool exists = false;
                    s = read_internal_current(txn, source_storage_key, string_value, exists);
                    if (s.ok() && !exists) {
                        result.success = false;
                        result.int_value = -1;
                        continue;
                    }
                } else if (set_cardinality > 0) {
                    s = read_set_members(txn, user_key, set_members);
                } else if (list_length > 0) {
                    s = read_list_values(txn, user_key, list_values);
                } else if (zset_count > 0) {
                    s = collect_zset_values(txn, user_key, zset_values);
                } else if (source_stream) {
                    s = collect_stream_records(txn, user_key, stream_records);
                } else {
                    s = collect_hash_entries(txn, user_key, hash_entries);
                    if (s.ok()) {
                        s = collect_hash_field_ttls(txn, user_key, hash_field_ttls);
                    }
                }
                int64_t source_expire_at_ms = -1;
                bool source_ttl_exists = false;
                if (s.ok()) {
                    s = read_ttl_meta(txn, user_key, source_expire_at_ms, source_ttl_exists);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }

                auto delete_logical_key_for_rename = [&](const std::string& logical_key, bool keep_string_value = false) {
                    std::string storage_key = "table_key_" + logical_key;
                    mako::Status ds = mako::Status::OK();
                    if (!keep_string_value) {
                        ds = delete_raw_if_exists(txn, storage_key);
                        if (!ds.ok()) {
                            return ds;
                        }
                        batch_exists[storage_key] = false;
                        batch_values.erase(storage_key);
                    }
                    ds = delete_set(txn, logical_key);
                    if (ds.ok()) {
                        ds = delete_list(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = delete_zset(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = delete_hash(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = delete_stream(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = clear_ttl_meta(txn, logical_key);
                    }
                    return ds;
                };

                s = delete_logical_key_for_rename(destination, string_exists);
                if (s.ok() && string_exists) {
                    s = put_raw(txn, destination_storage_key, string_value);
                    if (s.ok()) {
                        batch_exists[destination_storage_key] = true;
                        batch_values[destination_storage_key] = string_value;
                    }
                } else if (s.ok() && !set_members.empty()) {
                    for (const auto& member : set_members) {
                        std::string member_key = set_storage_key(destination, member);
                        s = put_raw(txn, member_key, "1");
                        if (!s.ok()) {
                            break;
                        }
                        batch_exists[member_key] = true;
                        batch_values[member_key] = "1";
                    }
                    if (s.ok()) {
                        s = write_set_cardinality(txn, destination, static_cast<int64_t>(set_members.size()));
                    }
                } else if (s.ok() && !list_values.empty()) {
                    s = rewrite_list_values(txn, destination, list_values);
                } else if (s.ok() && !zset_values.empty()) {
                    // Through rewrite_zset_values, exactly like COPY: it is the
                    // one place that formats a stored score, with
                    // format_zset_score. Writing the member record here with
                    // std::to_string(score) instead stored "1.000000" where
                    // every other path stores "1", so ZSCORE answered a
                    // different string after a RENAME than before it.
                    s = rewrite_zset_values(txn, destination, zset_values);
                } else if (s.ok() && !stream_records.empty()) {
                    s = write_stream_records(txn, destination, stream_records);
                } else if (s.ok()) {
                    for (const auto& [field, value] : hash_entries) {
                        std::string field_key = hash_field_storage_key(destination, field);
                        s = put_raw(txn, field_key, value);
                        if (!s.ok()) {
                            break;
                        }
                        batch_exists[field_key] = true;
                        batch_values[field_key] = value;
                    }
                    for (const auto& [field, expire_at_ms] : hash_field_ttls) {
                        if (!s.ok()) {
                            break;
                        }
                        if (hash_entries.find(field) == hash_entries.end()) {
                            continue;
                        }
                        s = write_hash_field_ttl(txn, destination, field, expire_at_ms);
                    }
                    if (s.ok()) {
                        s = write_hash_cardinality(txn, destination, static_cast<int64_t>(hash_entries.size()));
                    }
                }
                if (s.ok() && source_ttl_exists) {
                    s = write_ttl_meta(txn, destination, source_expire_at_ms);
                }
                if (s.ok()) {
                    s = delete_logical_key_for_rename(user_key);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = rename_nx ? 1 : 0;
            } else if (op.op == TXN_OP_COPY || op.op == TXN_OP_MOVE) {
                // MOVE is COPY without REPLACE plus deletion of the source, in
                // one transaction: the two keys are the same Redis-visible name
                // under two logical-database prefixes, so every type, the TTL
                // and the hash field expirations travel exactly as they do for
                // COPY. COPY carries its destination raw in val_ptr; MOVE
                // carries it as a one-item packed list, because it needs the
                // packed form for redis_request_lock_stripes.
                const bool is_move = op.op == TXN_OP_MOVE;
                std::string destination;
                if (is_move) {
                    std::vector<std::string> move_args;
                    if (!unpack_bytes_list(op.val_ptr, op.val_len, move_args)
                        || move_args.empty()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    destination = move_args[0];
                } else {
                    destination.assign(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                }
                std::string source_storage_key = "table_key_" + user_key;
                std::string destination_storage_key = "table_key_" + destination;
                mako::Status s = expire_logical_key_if_needed(txn, user_key, source_storage_key);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }

                bool string_exists = false;
                s = read_string_exists_no_expire(txn, source_storage_key, string_exists);
                int64_t set_cardinality = 0;
                if (s.ok()) {
                    s = read_set_cardinality(txn, user_key, set_cardinality);
                }
                int64_t list_length = 0;
                if (s.ok()) {
                    s = read_list_length(txn, user_key, list_length);
                }
                int64_t zset_count = 0;
                if (s.ok()) {
                    s = read_zset_cardinality(txn, user_key, zset_count);
                }
                int64_t hash_count = 0;
                if (s.ok()) {
                    s = read_hash_cardinality(txn, user_key, hash_count);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                bool source_stream = false;
                s = stream_exists(txn, user_key, source_stream);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                if (!string_exists && set_cardinality == 0 && list_length == 0
                    && zset_count == 0 && hash_count == 0 && !source_stream) {
                    result.success = true;
                    result.value_present = true;
                    result.int_value = 0;
                    continue;
                }
                const bool replace = !is_move && op.expire_at_ms == 1;
                if (user_key == destination) {
                    result.success = true;
                    result.value_present = true;
                    result.int_value = 1;
                    continue;
                }

                bool destination_string_exists = false;
                mako::Status ds = expire_logical_key_if_needed(txn, destination, destination_storage_key);
                if (ds.ok()) {
                    ds = read_string_exists_no_expire(txn, destination_storage_key, destination_string_exists);
                }
                int64_t destination_set_cardinality = 0;
                if (ds.ok()) {
                    ds = read_set_cardinality(txn, destination, destination_set_cardinality);
                }
                int64_t destination_list_length = 0;
                if (ds.ok()) {
                    ds = read_list_length(txn, destination, destination_list_length);
                }
                int64_t destination_zset_count = 0;
                if (ds.ok()) {
                    ds = read_zset_cardinality(txn, destination, destination_zset_count);
                }
                int64_t destination_hash_count = 0;
                if (ds.ok()) {
                    ds = read_hash_cardinality(txn, destination, destination_hash_count);
                }
                bool destination_stream = false;
                if (ds.ok()) {
                    ds = stream_exists(txn, destination, destination_stream);
                }
                if (!ds.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                const bool destination_exists = destination_string_exists
                    || destination_set_cardinality > 0 || destination_list_length > 0
                    || destination_zset_count > 0 || destination_hash_count > 0
                    || destination_stream;
                if (destination_exists && !replace) {
                    result.success = true;
                    result.value_present = true;
                    result.int_value = 0;
                    continue;
                }

                std::string string_value;
                std::vector<std::string> set_members;
                std::vector<std::string> list_values;
                std::map<std::string, double> zset_values;
                std::map<std::string, std::string> hash_entries;
                // RENAME and COPY move the whole object, so the per-field
                // expirations travel with the fields, and a stream carries its
                // groups, consumers and pending-entry lists.
                std::map<std::string, int64_t> hash_field_ttls;
                std::vector<std::string> stream_records;
                if (string_exists) {
                    bool exists = false;
                    s = read_internal_current(txn, source_storage_key, string_value, exists);
                    if (s.ok() && !exists) {
                        result.success = true;
                        result.value_present = true;
                        result.int_value = 0;
                        continue;
                    }
                } else if (set_cardinality > 0) {
                    s = read_set_members(txn, user_key, set_members);
                } else if (list_length > 0) {
                    s = read_list_values(txn, user_key, list_values);
                } else if (zset_count > 0) {
                    s = collect_zset_values(txn, user_key, zset_values);
                } else if (source_stream) {
                    s = collect_stream_records(txn, user_key, stream_records);
                } else {
                    s = collect_hash_entries(txn, user_key, hash_entries);
                    if (s.ok()) {
                        s = collect_hash_field_ttls(txn, user_key, hash_field_ttls);
                    }
                }
                int64_t source_expire_at_ms = -1;
                bool source_ttl_exists = false;
                if (s.ok()) {
                    s = read_ttl_meta(txn, user_key, source_expire_at_ms, source_ttl_exists);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }

                auto delete_logical_key_for_copy = [&](const std::string& logical_key, bool keep_string_value = false) {
                    std::string storage_key = "table_key_" + logical_key;
                    mako::Status delete_status = mako::Status::OK();
                    if (!keep_string_value) {
                        delete_status = delete_raw_if_exists(txn, storage_key);
                        if (!delete_status.ok()) {
                            return delete_status;
                        }
                        batch_exists[storage_key] = false;
                        batch_values.erase(storage_key);
                    }
                    delete_status = delete_set(txn, logical_key);
                    if (delete_status.ok()) {
                        delete_status = delete_list(txn, logical_key);
                    }
                    if (delete_status.ok()) {
                        delete_status = delete_zset(txn, logical_key);
                    }
                    if (delete_status.ok()) {
                        delete_status = delete_hash(txn, logical_key);
                    }
                    if (delete_status.ok()) {
                        delete_status = delete_stream(txn, logical_key);
                    }
                    if (delete_status.ok()) {
                        delete_status = clear_ttl_meta(txn, logical_key);
                    }
                    return delete_status;
                };

                s = delete_logical_key_for_copy(destination, string_exists);
                if (s.ok() && string_exists) {
                    s = put_raw(txn, destination_storage_key, string_value);
                    if (s.ok()) {
                        batch_exists[destination_storage_key] = true;
                        batch_values[destination_storage_key] = string_value;
                    }
                } else if (s.ok() && !set_members.empty()) {
                    for (const auto& member : set_members) {
                        std::string member_key = set_storage_key(destination, member);
                        s = put_raw(txn, member_key, "1");
                        if (!s.ok()) {
                            break;
                        }
                        batch_exists[member_key] = true;
                        batch_values[member_key] = "1";
                    }
                    if (s.ok()) {
                        s = write_set_cardinality(txn, destination, static_cast<int64_t>(set_members.size()));
                    }
                } else if (s.ok() && !list_values.empty()) {
                    s = rewrite_list_values(txn, destination, list_values);
                } else if (s.ok() && !zset_values.empty()) {
                    s = rewrite_zset_values(txn, destination, zset_values);
                } else if (s.ok() && !stream_records.empty()) {
                    s = write_stream_records(txn, destination, stream_records);
                } else if (s.ok()) {
                    for (const auto& [field, value] : hash_entries) {
                        std::string field_key = hash_field_storage_key(destination, field);
                        s = put_raw(txn, field_key, value);
                        if (!s.ok()) {
                            break;
                        }
                        batch_exists[field_key] = true;
                        batch_values[field_key] = value;
                    }
                    for (const auto& [field, expire_at_ms] : hash_field_ttls) {
                        if (!s.ok()) {
                            break;
                        }
                        if (hash_entries.find(field) == hash_entries.end()) {
                            continue;
                        }
                        s = write_hash_field_ttl(txn, destination, field, expire_at_ms);
                    }
                    if (s.ok()) {
                        s = write_hash_cardinality(txn, destination, static_cast<int64_t>(hash_entries.size()));
                    }
                }
                if (s.ok() && source_ttl_exists) {
                    s = write_ttl_meta(txn, destination, source_expire_at_ms);
                }
                if (s.ok() && is_move) {
                    // The object is now under the destination name, so the
                    // source name goes away in the same transaction.
                    s = delete_logical_key_for_copy(user_key);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = 1;
            } else if (op.op == TXN_OP_DUMP) {
                mako::Status s = expire_logical_key_if_needed(txn, user_key, tl_key_buf);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                bool string_exists = false;
                s = read_string_exists_no_expire(txn, tl_key_buf, string_exists);
                int64_t list_length = 0;
                if (s.ok()) {
                    s = read_list_length(txn, user_key, list_length);
                }
                int64_t hash_count = 0;
                if (s.ok()) {
                    // DUMP must not serialize a field whose time has passed.
                    s = read_hash_cardinality_live(txn, user_key, hash_count);
                }
                int64_t set_count = 0;
                if (s.ok()) {
                    s = read_set_cardinality(txn, user_key, set_count);
                }
                int64_t zset_count = 0;
                if (s.ok()) {
                    s = read_zset_cardinality(txn, user_key, zset_count);
                }
                bool stream_here = false;
                if (s.ok()) {
                    s = stream_exists(txn, user_key, stream_here);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                std::string payload;
                if (string_exists) {
                    std::string value;
                    bool exists = false;
                    s = read_internal_current(txn, tl_key_buf, value, exists);
                    if (s.ok() && exists) {
                        payload = std::string("MAKO_STRING_DUMP\0", 17) + value;
                    }
                } else if (list_length > 0) {
                    std::vector<std::string> values;
                    s = read_list_values(txn, user_key, values);
                    if (s.ok()) {
                        payload = std::string("MAKO_LIST_DUMP\0", 15) + pack_bytes_list(values);
                    }
                } else if (hash_count > 0) {
                    std::map<std::string, std::string> entries;
                    s = collect_hash_entries(txn, user_key, entries);
                    if (s.ok()) {
                        std::vector<std::string> fields;
                        fields.reserve(entries.size() * 2);
                        for (const auto& [field, value] : entries) {
                            fields.push_back(field);
                            fields.push_back(value);
                        }
                        payload = std::string("MAKO_HASH_DUMP\0", 15) + pack_bytes_list(fields);
                    }
                } else if (set_count > 0) {
                    std::vector<std::string> members;
                    s = read_set_members(txn, user_key, members);
                    if (s.ok()) {
                        payload = std::string("MAKO_SET_DUMP\0", 14) + pack_bytes_list(members);
                    }
                } else if (zset_count > 0) {
                    std::map<std::string, double> values;
                    s = collect_zset_values(txn, user_key, values);
                    if (s.ok()) {
                        // Encode as [score, member, ...] so RESTORE can replay it
                        // through the ZADD path unchanged.
                        std::vector<std::string> fields;
                        fields.reserve(values.size() * 2);
                        for (const auto& [member, score] : values) {
                            fields.push_back(format_zset_score(score));
                            fields.push_back(member);
                        }
                        payload = std::string("MAKO_ZSET_DUMP\0", 15) + pack_bytes_list(fields);
                    }
                } else if (stream_here) {
                    // The whole stream, family letter by family letter, so a
                    // RESTORE brings back the entries and the consumer groups
                    // with their pending-entry lists.
                    std::vector<std::string> records;
                    s = collect_stream_records(txn, user_key, records);
                    if (s.ok()) {
                        payload = std::string("MAKO_STREAM_DUMP\0", 17) + pack_bytes_list(records);
                    }
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = !payload.empty();
                if (result.value_present && !copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_BITOP) {
                // BITOP AND|OR|XOR|NOT dest src [src ...]: read every source string
                // and write the combined bytes to dest inside the same transaction.
                std::vector<std::string> items;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, items) || items.size() < 2) {
                    all_success = false;
                    continue;
                }
                std::string operation = items[0];
                std::transform(operation.begin(), operation.end(), operation.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                bool dest_allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, dest_allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!dest_allowed) {
                    continue;
                }
                std::vector<std::string> sources;
                sources.reserve(items.size() - 1);
                size_t max_len = 0;
                bool sources_ok = true;
                for (size_t i = 1; i < items.size(); ++i) {
                    const std::string& src_key = items[i];
                    const std::string src_storage = "table_key_" + src_key;
                    bool src_allowed = false;
                    s = string_key_allowed(txn, src_key, src_storage, result, src_allowed);
                    if (!s.ok()) {
                        all_success = false;
                        sources_ok = false;
                        break;
                    }
                    if (!src_allowed) {
                        sources_ok = false;
                        break;
                    }
                    std::string value;
                    bool exists = false;
                    s = read_current(txn, src_key, src_storage, value, exists);
                    if (!s.ok()) {
                        all_success = false;
                        sources_ok = false;
                        break;
                    }
                    if (!exists) {
                        value.clear();
                    }
                    max_len = std::max(max_len, value.size());
                    sources.push_back(std::move(value));
                }
                if (!sources_ok) {
                    continue;
                }
                std::string combined(max_len, '\0');
                for (size_t i = 0; i < max_len; ++i) {
                    unsigned char acc = 0;
                    if (operation == "NOT") {
                        const unsigned char b =
                            i < sources[0].size() ? static_cast<unsigned char>(sources[0][i]) : 0;
                        acc = static_cast<unsigned char>(~b);
                    } else {
                        bool first_source = true;
                        for (const auto& src : sources) {
                            const unsigned char b =
                                i < src.size() ? static_cast<unsigned char>(src[i]) : 0;
                            if (first_source) {
                                acc = b;
                                first_source = false;
                            } else if (operation == "AND") {
                                acc &= b;
                            } else if (operation == "OR") {
                                acc |= b;
                            } else {
                                acc ^= b;
                            }
                        }
                    }
                    combined[i] = static_cast<char>(acc);
                }
                if (combined.empty()) {
                    // Redis deletes the destination when the result is empty.
                    s = delete_raw_if_exists(txn, tl_key_buf);
                    if (s.ok()) {
                        batch_exists[tl_key_buf] = false;
                        batch_values.erase(tl_key_buf);
                    }
                } else {
                    s = put_raw(txn, tl_key_buf, combined);
                    if (s.ok()) {
                        batch_exists[tl_key_buf] = true;
                        batch_values[tl_key_buf] = combined;
                    }
                }
                if (s.ok()) {
                    s = clear_ttl_meta(txn, user_key);
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = static_cast<int64_t>(combined.size());
            } else if (op.op == TXN_OP_HLL_ADD) {
                // PFADD key [element ...]. Read-modify-write has to happen in a
                // single op because the executor has no interactive transaction.
                std::vector<std::string> elements;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, elements)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;  // string_key_allowed already flagged WRONGTYPE
                }
                std::string current;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                bool changed = false;
                if (!exists) {
                    current = hll_make_empty();
                    changed = true;
                } else if (!hll_value_is_valid(current)) {
                    result.success = false;
                    result.int_value = TXN_HLL_ERR_NOT_HLL;
                    continue;
                }
                uint8_t* registers =
                    reinterpret_cast<uint8_t*>(current.data()) + kHllHeaderSize;
                for (const std::string& element : elements) {
                    size_t index = 0;
                    uint8_t run = 0;
                    hll_element_slot(element.data(), element.size(), index, run);
                    if (registers[index] < run) {
                        registers[index] = run;
                        changed = true;
                    }
                }
                if (changed) {
                    // The sketch stays a plain string value, and PFADD keeps any
                    // TTL the key already had, exactly like SETBIT or APPEND.
                    s = put_raw(txn, tl_key_buf, current);
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    batch_exists[tl_key_buf] = true;
                    batch_values[tl_key_buf] = current;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = changed ? 1 : 0;
            } else if (op.op == TXN_OP_HLL_COUNT) {
                // PFCOUNT key [key ...]. The payload carries every key, including
                // op.key; no key is modified, the registers are merged in memory.
                std::vector<std::string> keys;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, keys) || keys.empty()) {
                    all_success = false;
                    continue;
                }
                std::vector<uint8_t> merged(kHllRegisters, 0);
                bool keys_ok = true;
                bool invalid_hll = false;
                for (const std::string& hll_key : keys) {
                    const std::string storage_key = "table_key_" + hll_key;
                    bool allowed = false;
                    mako::Status s =
                        string_key_allowed(txn, hll_key, storage_key, result, allowed);
                    if (!s.ok()) {
                        all_success = false;
                        keys_ok = false;
                        break;
                    }
                    if (!allowed) {
                        keys_ok = false;
                        break;
                    }
                    std::string value;
                    bool exists = false;
                    s = read_current(txn, hll_key, storage_key, value, exists);
                    if (!s.ok()) {
                        all_success = false;
                        keys_ok = false;
                        break;
                    }
                    if (!exists) {
                        continue;  // a missing key contributes an empty sketch
                    }
                    if (!hll_value_is_valid(value)) {
                        keys_ok = false;
                        invalid_hll = true;
                        break;
                    }
                    const uint8_t* registers =
                        reinterpret_cast<const uint8_t*>(value.data()) + kHllHeaderSize;
                    for (size_t r = 0; r < kHllRegisters; ++r) {
                        if (registers[r] > merged[r]) {
                            merged[r] = registers[r];
                        }
                    }
                }
                if (!keys_ok) {
                    if (invalid_hll) {
                        result.success = false;
                        result.int_value = TXN_HLL_ERR_NOT_HLL;
                    }
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = hll_estimate(merged.data());
            } else if (op.op == TXN_OP_HLL_MERGE) {
                // PFMERGE destkey [sourcekey ...]: destination becomes the union
                // of itself and every source.
                std::vector<std::string> sources;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, sources)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string destination;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, destination, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists) {
                    destination = hll_make_empty();
                } else if (!hll_value_is_valid(destination)) {
                    result.success = false;
                    result.int_value = TXN_HLL_ERR_NOT_HLL;
                    continue;
                }
                uint8_t* registers =
                    reinterpret_cast<uint8_t*>(destination.data()) + kHllHeaderSize;
                bool sources_ok = true;
                bool invalid_hll = false;
                for (const std::string& src_key : sources) {
                    const std::string src_storage = "table_key_" + src_key;
                    bool src_allowed = false;
                    s = string_key_allowed(txn, src_key, src_storage, result, src_allowed);
                    if (!s.ok()) {
                        all_success = false;
                        sources_ok = false;
                        break;
                    }
                    if (!src_allowed) {
                        sources_ok = false;
                        break;
                    }
                    std::string value;
                    bool src_exists = false;
                    s = read_current(txn, src_key, src_storage, value, src_exists);
                    if (!s.ok()) {
                        all_success = false;
                        sources_ok = false;
                        break;
                    }
                    if (!src_exists) {
                        continue;
                    }
                    if (!hll_value_is_valid(value)) {
                        sources_ok = false;
                        invalid_hll = true;
                        break;
                    }
                    const uint8_t* src_registers =
                        reinterpret_cast<const uint8_t*>(value.data()) + kHllHeaderSize;
                    for (size_t r = 0; r < kHllRegisters; ++r) {
                        if (src_registers[r] > registers[r]) {
                            registers[r] = src_registers[r];
                        }
                    }
                }
                if (!sources_ok) {
                    if (invalid_hll) {
                        result.success = false;
                        result.int_value = TXN_HLL_ERR_NOT_HLL;
                    }
                    continue;
                }
                // Like Redis, PFMERGE rewrites the destination in place and keeps
                // whatever TTL it already carried.
                s = put_raw(txn, tl_key_buf, destination);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                batch_exists[tl_key_buf] = true;
                batch_values[tl_key_buf] = destination;
                result.success = true;
                result.value_present = true;
                result.int_value = 0;
            } else if (op.op == TXN_OP_BITFIELD) {
                // BITFIELD with at least one SET or INCRBY. The whole
                // subcommand list runs here so the read-modify-write stays
                // inside one Mako transaction. Payload layout: groups of four
                // items [kind, encoding, offset, value]; see TXN_OP_BITFIELD in
                // transaction_ffi.h. Rust has already validated every field.
                std::vector<std::string> items;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, items) || (items.size() % 4) != 0) {
                    all_success = false;
                    continue;
                }
                struct BitFieldSub {
                    int kind;  // 0 = GET, 1 = SET, 2 = INCRBY
                    bool sign;
                    uint32_t bits;
                    uint64_t offset;
                    int64_t value;
                    int owtype;
                };
                std::vector<BitFieldSub> subs;
                subs.reserve(items.size() / 4);
                bool payload_ok = true;
                int owtype = kBitFieldWrap;
                uint64_t highest_write_bit = 0;
                bool has_write = false;
                for (size_t i = 0; i + 3 < items.size(); i += 4) {
                    const std::string& kind_text = items[i];
                    if (kind_text == "OVERFLOW") {
                        if (items[i + 1] == "WRAP") {
                            owtype = kBitFieldWrap;
                        } else if (items[i + 1] == "SAT") {
                            owtype = kBitFieldSat;
                        } else if (items[i + 1] == "FAIL") {
                            owtype = kBitFieldFail;
                        } else {
                            payload_ok = false;
                            break;
                        }
                        continue;
                    }
                    BitFieldSub sub{};
                    if (kind_text == "GET") {
                        sub.kind = 0;
                    } else if (kind_text == "SET") {
                        sub.kind = 1;
                    } else if (kind_text == "INCRBY") {
                        sub.kind = 2;
                    } else {
                        payload_ok = false;
                        break;
                    }
                    const std::string& encoding = items[i + 1];
                    if (encoding.size() < 2 || (encoding[0] != 'i' && encoding[0] != 'u')) {
                        payload_ok = false;
                        break;
                    }
                    sub.sign = encoding[0] == 'i';
                    int64_t bits = 0;
                    if (!parse_int64(encoding.substr(1), bits) || bits < 1
                        || bits > (sub.sign ? 64 : 63)) {
                        payload_ok = false;
                        break;
                    }
                    sub.bits = static_cast<uint32_t>(bits);
                    int64_t offset = 0;
                    if (!parse_int64(items[i + 2], offset) || offset < 0) {
                        payload_ok = false;
                        break;
                    }
                    sub.offset = static_cast<uint64_t>(offset);
                    sub.value = 0;
                    if (sub.kind != 0) {
                        if (!parse_int64(items[i + 3], sub.value)) {
                            payload_ok = false;
                            break;
                        }
                        has_write = true;
                        const uint64_t last_bit = sub.offset + sub.bits - 1;
                        if (last_bit > highest_write_bit) {
                            highest_write_bit = last_bit;
                        }
                    }
                    sub.owtype = owtype;
                    subs.push_back(sub);
                }
                if (!payload_ok) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = string_key_allowed(txn, user_key, tl_key_buf, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;  // string_key_allowed already flagged WRONGTYPE
                }
                std::string current;
                bool exists = false;
                s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                // Redis creates or zero-extends the string up front to cover the
                // farthest bit any write touches, even when every write then
                // fails its overflow check.
                bool dirty = false;
                if (has_write) {
                    const size_t needed = static_cast<size_t>(highest_write_bit >> 3) + 1;
                    if (!exists) {
                        current.assign(needed, '\0');
                        dirty = true;
                    } else if (current.size() < needed) {
                        current.resize(needed, '\0');
                        dirty = true;
                    }
                }
                std::vector<std::string> replies;
                replies.reserve(subs.size());
                for (const BitFieldSub& sub : subs) {
                    if (sub.kind == 0) {
                        const int64_t value =
                            sub.sign ? bitfield_get_signed(current, sub.offset, sub.bits)
                                     : static_cast<int64_t>(
                                           bitfield_get_unsigned(current, sub.offset, sub.bits));
                        replies.push_back(std::to_string(value));
                        continue;
                    }
                    const bool is_incr = sub.kind == 2;
                    const int64_t incr = is_incr ? sub.value : 0;
                    if (sub.sign) {
                        const int64_t oldval = bitfield_get_signed(current, sub.offset, sub.bits);
                        const int64_t checked = is_incr ? oldval : sub.value;
                        int64_t wrapped = 0;
                        const int overflow = bitfield_signed_overflow(
                            checked, incr, sub.bits, sub.owtype, wrapped);
                        int64_t newval =
                            is_incr ? static_cast<int64_t>(static_cast<uint64_t>(oldval)
                                                           + static_cast<uint64_t>(incr))
                                    : sub.value;
                        if (overflow != 0) {
                            newval = wrapped;
                        }
                        if (overflow != 0 && sub.owtype == kBitFieldFail) {
                            replies.emplace_back();  // nil: this subcommand writes nothing
                            continue;
                        }
                        replies.push_back(std::to_string(is_incr ? newval : oldval));
                        bitfield_set_bits(
                            current, sub.offset, sub.bits, static_cast<uint64_t>(newval));
                        dirty = true;
                    } else {
                        const uint64_t oldval = bitfield_get_unsigned(current, sub.offset, sub.bits);
                        const uint64_t checked = is_incr ? oldval : static_cast<uint64_t>(sub.value);
                        uint64_t wrapped = 0;
                        const int overflow = bitfield_unsigned_overflow(
                            checked, incr, sub.bits, sub.owtype, wrapped);
                        uint64_t newval = is_incr ? (oldval + static_cast<uint64_t>(incr))
                                                  : static_cast<uint64_t>(sub.value);
                        if (overflow != 0) {
                            newval = wrapped;
                        }
                        if (overflow != 0 && sub.owtype == kBitFieldFail) {
                            replies.emplace_back();  // nil: this subcommand writes nothing
                            continue;
                        }
                        replies.push_back(
                            std::to_string(static_cast<int64_t>(is_incr ? newval : oldval)));
                        bitfield_set_bits(current, sub.offset, sub.bits, newval);
                        dirty = true;
                    }
                }
                if (dirty) {
                    // The value stays a plain string and any TTL the key already
                    // carried is left alone, exactly like SETBIT.
                    s = put_raw(txn, tl_key_buf, current);
                    if (!s.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    batch_exists[tl_key_buf] = true;
                    batch_values[tl_key_buf] = current;
                }
                result.success = true;
                result.int_value = static_cast<int64_t>(replies.size());
                if (!copy_result_value(result, pack_bytes_list(replies))) {
                    all_success = false;
                    continue;
                }
            } else if (op.op == TXN_OP_XADD) {
                // One op: the insert and the MAXLEN/MINID trim that follows it
                // have to be the same transaction, and a request's op list is
                // built before the executor sees it.
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args)
                    || args.size() < 7 || (args.size() - 5) % 2 != 0) {
                    all_success = false;
                    continue;
                }
                const std::string id_spec = args[0];
                const bool nomkstream = args[1] == "1";
                const std::string trim_strategy = args[2];
                const std::string trim_threshold = args[3];
                int64_t trim_limit = 0;
                parse_int64(args[4], trim_limit);

                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists && nomkstream) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NOMKSTREAM;
                    continue;
                }

                RedisStreamId id;
                bool id_ok = true;
                if (id_spec == "*") {
                    const uint64_t now = static_cast<uint64_t>(now_unix_ms());
                    if (now > meta.last_id.ms) {
                        id.ms = now;
                        id.seq = 0;
                    } else {
                        id = meta.last_id;
                        id_ok = stream_id_incr(id);
                    }
                } else if (id_spec.size() > 2
                           && id_spec.compare(id_spec.size() - 2, 2, "-*") == 0) {
                    // "<ms>-*": the sequence continues the last ID when the
                    // milliseconds match and restarts at zero otherwise.
                    uint64_t ms = 0;
                    if (!stream_parse_u64(id_spec.data(), id_spec.size() - 2, ms)) {
                        all_success = false;
                        continue;
                    }
                    if (ms == meta.last_id.ms) {
                        id = meta.last_id;
                        id_ok = stream_id_incr(id) && id.ms == ms;
                    } else {
                        id.ms = ms;
                        id.seq = 0;
                    }
                } else if (!stream_id_parse(id_spec, id)) {
                    all_success = false;
                    continue;
                }
                if (!id_ok || stream_id_compare(id, meta.last_id) <= 0) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_SMALLER_ID;
                    continue;
                }

                std::vector<std::string> fields(args.begin() + 5, args.end());
                s = put_raw(txn, make_stream_entry_key(user_key, id), pack_bytes_list(fields));
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                meta.length += 1;
                meta.entries_added += 1;
                meta.last_id = id;
                if (meta.length == 1) {
                    meta.first_id = id;
                }
                int64_t trimmed = 0;
                s = trim_stream(txn, user_key, meta, trim_strategy, trim_threshold, trim_limit, trimmed);
                if (s.ok()) {
                    s = write_stream_meta(txn, user_key, meta);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.int_value = 0;
                if (!copy_result_value(result, stream_id_format(id))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_XRANGE) {
                // XRANGE, XREVRANGE and one stream of an XREAD. Rust has
                // already turned "-", "+", the bare "<ms>" forms and the "("
                // exclusive markers into an inclusive [start, end] pair.
                std::vector<std::string> args;
                RedisStreamId start;
                RedisStreamId end;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() != 4
                    || !stream_id_parse(args[0], start) || !stream_id_parse(args[1], end)) {
                    all_success = false;
                    continue;
                }
                int64_t count = 0;
                parse_int64(args[2], count);
                const bool reverse = args[3] == "1";
                // Mode "2" asks for the stream's last-generated ID and no
                // entries: it is what the first attempt at a blocking
                // `XREAD ... $` does, where by definition nothing already
                // stored can qualify.
                const bool id_only = args[3] == "2";

                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                std::vector<std::pair<RedisStreamId, std::string>> entries;
                if (s.ok() && !id_only && exists && meta.length > 0) {
                    s = collect_stream_entries(
                        txn, user_key, start, end,
                        count > 0 ? static_cast<size_t>(count) : 0, reverse, entries);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                // Item 0 is the stream's last-generated ID, which is what a
                // blocking XREAD resolves "$" and "+" to when the first attempt
                // found nothing.
                std::vector<std::string> payload;
                payload.reserve(1 + entries.size() * 2);
                payload.push_back(stream_id_format(meta.last_id));
                for (const auto& [entry_id, entry_fields] : entries) {
                    payload.push_back(stream_id_format(entry_id));
                    payload.push_back(entry_fields);
                }
                result.success = true;
                result.int_value = exists ? 1 : 0;
                if (!copy_result_value(result, pack_bytes_list(payload))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_XLEN) {
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = static_cast<int64_t>(meta.length);
            } else if (op.op == TXN_OP_XDEL) {
                std::vector<std::string> ids;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, ids) || ids.empty()) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t deleted = 0;
                if (exists) {
                    for (const auto& id_text : ids) {
                        RedisStreamId id;
                        if (!stream_id_parse(id_text, id)) {
                            continue;
                        }
                        std::string fields;
                        bool entry_exists = false;
                        s = read_stream_entry(txn, user_key, id, fields, entry_exists);
                        if (!s.ok()) {
                            break;
                        }
                        if (!entry_exists) {
                            continue;
                        }
                        s = delete_raw_if_exists(txn, make_stream_entry_key(user_key, id));
                        if (!s.ok()) {
                            break;
                        }
                        deleted += 1;
                        if (meta.length > 0) {
                            meta.length -= 1;
                        }
                        // Redis records the largest ID ever deleted, and never
                        // lowers it: XTRIM does not touch it at all.
                        if (stream_id_compare(id, meta.max_deleted_id) > 0) {
                            meta.max_deleted_id = id;
                        }
                    }
                }
                if (s.ok() && deleted > 0) {
                    bool found_first = false;
                    s = read_stream_first_id(txn, user_key, meta.first_id, found_first);
                    if (s.ok() && !found_first) {
                        meta.first_id = RedisStreamId{};
                    }
                    if (s.ok()) {
                        s = write_stream_meta(txn, user_key, meta);
                    }
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = deleted;
            } else if (op.op == TXN_OP_XTRIM) {
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() != 3) {
                    all_success = false;
                    continue;
                }
                int64_t trim_limit = 0;
                parse_int64(args[2], trim_limit);
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                int64_t trimmed = 0;
                if (s.ok() && exists) {
                    s = trim_stream(txn, user_key, meta, args[0], args[1], trim_limit, trimmed);
                    if (s.ok() && trimmed > 0) {
                        s = write_stream_meta(txn, user_key, meta);
                    }
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = trimmed;
            } else if (op.op == TXN_OP_XSETID) {
                std::vector<std::string> args;
                RedisStreamId id;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() != 3
                    || !stream_id_parse(args[0], id)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NO_SUCH_KEY;
                    continue;
                }
                bool max_deleted_given = false;
                RedisStreamId max_deleted = meta.max_deleted_id;
                if (!args[2].empty()) {
                    if (!stream_id_parse(args[2], max_deleted)) {
                        all_success = false;
                        continue;
                    }
                    max_deleted_given = true;
                }
                if (max_deleted_given && stream_id_compare(id, max_deleted) < 0) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_SETID_TOMBSTONE;
                    continue;
                }
                int64_t entries_added = -1;
                if (!args[1].empty() && !parse_int64(args[1], entries_added)) {
                    all_success = false;
                    continue;
                }
                if (meta.length > 0) {
                    // The check is against the largest ID still stored, not the
                    // last generated one, which may name a deleted entry.
                    std::vector<std::pair<RedisStreamId, std::string>> last_entry;
                    RedisStreamId max_id;
                    RedisStreamId min_id;
                    max_id.ms = std::numeric_limits<uint64_t>::max();
                    max_id.seq = std::numeric_limits<uint64_t>::max();
                    s = collect_stream_entries(txn, user_key, min_id, max_id, 1, true, last_entry);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (!last_entry.empty() && stream_id_compare(id, last_entry[0].first) < 0) {
                        result.success = true;
                        result.value_present = false;
                        result.int_value = TXN_STREAM_ERR_SETID_SMALLER;
                        continue;
                    }
                    if (entries_added >= 0 && meta.length > static_cast<uint64_t>(entries_added)) {
                        result.success = true;
                        result.value_present = false;
                        result.int_value = TXN_STREAM_ERR_SETID_ENTRIES_ADDED;
                        continue;
                    }
                }
                if (!max_deleted_given && stream_id_compare(id, meta.max_deleted_id) < 0) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_SETID_TOMBSTONE;
                    continue;
                }
                meta.last_id = id;
                if (entries_added >= 0) {
                    meta.entries_added = static_cast<uint64_t>(entries_added);
                }
                if (max_deleted_given && !stream_id_is_zero(max_deleted)) {
                    meta.max_deleted_id = max_deleted;
                }
                s = write_stream_meta(txn, user_key, meta);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = 0;
            } else if (op.op == TXN_OP_XINFO) {
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.empty()) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NO_SUCH_KEY;
                    continue;
                }
                RedisStreamId min_id;
                RedisStreamId max_id;
                max_id.ms = std::numeric_limits<uint64_t>::max();
                max_id.seq = std::numeric_limits<uint64_t>::max();
                std::vector<std::string> payload;

                if (args[0] == "STREAM") {
                    const bool full = args.size() > 1 && args[1] == "1";
                    int64_t info_count = 10;
                    if (args.size() > 2) {
                        parse_int64(args[2], info_count);
                    }
                    payload.push_back(std::to_string(meta.length));
                    // One storage record per entry, so the entry count is the
                    // number of keys this stream occupies in the ordered index.
                    payload.push_back(std::to_string(meta.length));
                    payload.push_back(std::to_string(meta.length + 1));
                    payload.push_back(stream_id_format(meta.last_id));
                    payload.push_back(stream_id_format(meta.max_deleted_id));
                    payload.push_back(std::to_string(meta.entries_added));
                    payload.push_back(stream_id_format(meta.first_id));
                    if (!full) {
                        payload.push_back(std::to_string(meta.groups));
                        std::vector<std::pair<RedisStreamId, std::string>> edge;
                        s = collect_stream_entries(txn, user_key, min_id, max_id, 1, false, edge);
                        if (s.ok()) {
                            payload.push_back(edge.empty() ? "0" : "1");
                            payload.push_back(edge.empty() ? std::string() : stream_id_format(edge[0].first));
                            payload.push_back(edge.empty() ? std::string() : edge[0].second);
                            s = collect_stream_entries(txn, user_key, min_id, max_id, 1, true, edge);
                        }
                        if (s.ok()) {
                            payload.push_back(edge.empty() ? "0" : "1");
                            payload.push_back(edge.empty() ? std::string() : stream_id_format(edge[0].first));
                            payload.push_back(edge.empty() ? std::string() : edge[0].second);
                        }
                    } else {
                        std::vector<std::pair<RedisStreamId, std::string>> entries;
                        s = collect_stream_entries(
                            txn, user_key, min_id, max_id,
                            info_count > 0 ? static_cast<size_t>(info_count) : 0, false, entries);
                        if (s.ok()) {
                            payload.push_back(std::to_string(entries.size()));
                            for (const auto& [entry_id, entry_fields] : entries) {
                                payload.push_back(stream_id_format(entry_id));
                                payload.push_back(entry_fields);
                            }
                            s = append_stream_group_info(
                                txn, user_key, meta, std::string(),
                                info_count > 0 ? static_cast<size_t>(info_count) : 0,
                                true, payload);
                        }
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.success = true;
                    result.int_value = full ? 1 : 0;
                    if (!copy_result_value(result, pack_bytes_list(payload))) {
                        all_success = false;
                    }
                } else if (args[0] == "GROUPS") {
                    s = append_stream_group_info(
                        txn, user_key, meta, std::string(), 0, false, payload);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.success = true;
                    result.int_value = 0;
                    if (!copy_result_value(result, pack_bytes_list(payload))) {
                        all_success = false;
                    }
                } else if (args[0] == "CONSUMERS") {
                    if (args.size() < 2) {
                        all_success = false;
                        continue;
                    }
                    RedisStreamGroup group;
                    bool group_exists = false;
                    s = read_stream_group(txn, user_key, args[1], group, group_exists);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (!group_exists) {
                        result.success = true;
                        result.value_present = false;
                        result.int_value = TXN_STREAM_ERR_NOGROUP;
                        continue;
                    }
                    s = append_stream_consumer_info(txn, user_key, args[1], payload);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.success = true;
                    result.int_value = 0;
                    if (!copy_result_value(result, pack_bytes_list(payload))) {
                        all_success = false;
                    }
                } else {
                    all_success = false;
                    continue;
                }
            } else if (op.op == TXN_OP_XGROUP) {
                // XGROUP CREATE/SETID/DESTROY/CREATECONSUMER/DELCONSUMER, one
                // op each: every one of them is a read of the group record
                // followed by a write that depends on it.
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() < 2) {
                    all_success = false;
                    continue;
                }
                const std::string subcommand = args[0];
                const std::string group_name = args[1];

                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;

                if (subcommand == "CREATE") {
                    if (args.size() < 5) {
                        all_success = false;
                        continue;
                    }
                    const bool mkstream = args[3] == "1";
                    if (!exists && !mkstream) {
                        result.value_present = false;
                        result.int_value = TXN_STREAM_ERR_NO_KEY_FOR_GROUP;
                        continue;
                    }
                    RedisStreamGroup group;
                    bool group_exists = false;
                    s = read_stream_group(txn, user_key, group_name, group, group_exists);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (group_exists) {
                        result.value_present = false;
                        result.int_value = TXN_STREAM_ERR_BUSYGROUP;
                        continue;
                    }
                    RedisStreamId id;
                    if (args[2] == "$") {
                        id = meta.last_id;
                    } else if (!stream_id_parse(args[2], id)) {
                        all_success = false;
                        continue;
                    }
                    group = RedisStreamGroup{};
                    group.last_id = id;
                    if (!args[4].empty() && !parse_int64(args[4], group.entries_read)) {
                        all_success = false;
                        continue;
                    }
                    s = write_stream_group(txn, user_key, group_name, group);
                    if (s.ok()) {
                        meta.groups += 1;
                        s = write_stream_meta(txn, user_key, meta);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.int_value = 1;
                    continue;
                }

                RedisStreamGroup group;
                bool group_exists = false;
                if (exists) {
                    s = read_stream_group(txn, user_key, group_name, group, group_exists);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                }

                if (subcommand == "DESTROY") {
                    // The one subcommand Redis answers with a count rather than
                    // an error when the key or the group is not there.
                    if (!group_exists) {
                        result.int_value = 0;
                        continue;
                    }
                    s = delete_stream_group_records(txn, user_key, group_name);
                    if (s.ok()) {
                        if (meta.groups > 0) {
                            meta.groups -= 1;
                        }
                        s = write_stream_meta(txn, user_key, meta);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.int_value = 1;
                    continue;
                }

                if (!group_exists) {
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NOGROUP;
                    continue;
                }

                if (subcommand == "SETID") {
                    if (args.size() < 4) {
                        all_success = false;
                        continue;
                    }
                    RedisStreamId id;
                    if (args[2] == "$") {
                        id = meta.last_id;
                    } else if (!stream_id_parse(args[2], id)) {
                        all_success = false;
                        continue;
                    }
                    group.last_id = id;
                    // Redis resets the read counter unless ENTRIESREAD is given,
                    // because the new position says nothing about how many
                    // entries the group has seen.
                    group.entries_read = -1;
                    if (!args[3].empty() && !parse_int64(args[3], group.entries_read)) {
                        all_success = false;
                        continue;
                    }
                    s = write_stream_group(txn, user_key, group_name, group);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.int_value = 1;
                } else if (subcommand == "CREATECONSUMER") {
                    if (args.size() < 3) {
                        all_success = false;
                        continue;
                    }
                    RedisStreamConsumer consumer;
                    bool consumer_exists = false;
                    s = read_stream_consumer(txn, user_key, group_name, args[2], consumer, consumer_exists);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (consumer_exists) {
                        result.int_value = 0;
                        continue;
                    }
                    bool created = false;
                    s = touch_stream_consumer(
                        txn, user_key, group_name, group, args[2], false, consumer, created);
                    if (s.ok()) {
                        s = write_stream_group(txn, user_key, group_name, group);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.int_value = 1;
                } else if (subcommand == "DELCONSUMER") {
                    if (args.size() < 3) {
                        all_success = false;
                        continue;
                    }
                    RedisStreamConsumer consumer;
                    bool consumer_exists = false;
                    s = read_stream_consumer(txn, user_key, group_name, args[2], consumer, consumer_exists);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (!consumer_exists) {
                        result.int_value = 0;
                        continue;
                    }
                    RedisStreamId min_id;
                    RedisStreamId max_id;
                    max_id.ms = std::numeric_limits<uint64_t>::max();
                    max_id.seq = std::numeric_limits<uint64_t>::max();
                    std::vector<std::pair<RedisStreamId, RedisStreamNack>> pel;
                    s = collect_stream_pel(txn, user_key, group_name, min_id, max_id, 0, pel);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    int64_t removed = 0;
                    for (const auto& [id, nack] : pel) {
                        if (nack.consumer != args[2]) {
                            continue;
                        }
                        s = delete_raw_if_exists(txn, make_stream_pel_key(user_key, group_name, id));
                        if (!s.ok()) {
                            break;
                        }
                        if (group.pel > 0) {
                            group.pel -= 1;
                        }
                        removed += 1;
                    }
                    if (s.ok()) {
                        s = delete_raw_if_exists(
                            txn, make_stream_consumer_key(user_key, group_name, args[2]));
                    }
                    if (s.ok()) {
                        if (group.consumers > 0) {
                            group.consumers -= 1;
                        }
                        s = write_stream_group(txn, user_key, group_name, group);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.int_value = removed;
                } else {
                    all_success = false;
                    continue;
                }
            } else if (op.op == TXN_OP_XREADGROUP) {
                // One op per stream, as XREAD is. ">" hands over new entries and
                // records them in the group's pending-entry list; an explicit ID
                // replays this consumer's own pending entries from there.
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() != 6) {
                    all_success = false;
                    continue;
                }
                const std::string group_name = args[0];
                const std::string consumer_name = args[1];
                const std::string mode = args[2];
                RedisStreamId start;
                if (!stream_id_parse(args[3], start)) {
                    all_success = false;
                    continue;
                }
                int64_t count = 0;
                parse_int64(args[4], count);
                const bool noack = args[5] == "1";

                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                RedisStreamGroup group;
                bool group_exists = false;
                if (s.ok() && exists) {
                    s = read_stream_group(txn, user_key, group_name, group, group_exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists || !group_exists) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NOGROUP;
                    continue;
                }

                const int64_t now = now_unix_ms();
                RedisStreamId max_id;
                max_id.ms = std::numeric_limits<uint64_t>::max();
                max_id.seq = std::numeric_limits<uint64_t>::max();
                std::vector<std::string> payload;
                RedisStreamConsumer consumer;
                bool created = false;

                if (mode == "NEW") {
                    RedisStreamId first_new = group.last_id;
                    if (!stream_id_incr(first_new)) {
                        // The group is already past the largest ID there is.
                        s = touch_stream_consumer(
                            txn, user_key, group_name, group, consumer_name, false, consumer, created);
                        if (s.ok() && created) {
                            s = write_stream_group(txn, user_key, group_name, group);
                        }
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                        result.success = true;
                        result.int_value = 0;
                        if (!copy_result_value(result, pack_bytes_list(payload))) {
                            all_success = false;
                        }
                        continue;
                    }
                    std::vector<std::pair<RedisStreamId, std::string>> entries;
                    s = collect_stream_entries(
                        txn, user_key, first_new, max_id,
                        count > 0 ? static_cast<size_t>(count) : 0, false, entries);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    s = touch_stream_consumer(
                        txn, user_key, group_name, group, consumer_name,
                        !entries.empty(), consumer, created);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    for (const auto& [id, fields] : entries) {
                        payload.push_back(stream_id_format(id));
                        payload.push_back(fields);
                        if (noack) {
                            continue;
                        }
                        RedisStreamNack nack;
                        nack.consumer = consumer_name;
                        nack.delivery_time_ms = now;
                        nack.delivery_count = 1;
                        s = write_stream_nack(txn, user_key, group_name, id, nack);
                        if (!s.ok()) {
                            break;
                        }
                        group.pel += 1;
                        consumer.pending += 1;
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (!entries.empty()) {
                        const RedisStreamId first_served = entries.front().first;
                        group.last_id = entries.back().first;
                        // Redis's rule: the counter can be advanced by simple
                        // addition only when nothing was deleted at or after the
                        // first entry served; otherwise it is re-estimated, and
                        // an estimate is not always possible.
                        if (group.entries_read >= 0
                            && !stream_range_has_tombstones(meta, first_served)) {
                            group.entries_read += static_cast<int64_t>(entries.size());
                        } else {
                            int64_t estimate = 0;
                            group.entries_read =
                                stream_estimate_entries_read(meta, group.last_id, estimate)
                                    ? estimate
                                    : -1;
                        }
                    }
                    s = write_stream_group(txn, user_key, group_name, group);
                    if (s.ok()) {
                        s = write_stream_consumer(txn, user_key, group_name, consumer_name, consumer);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                } else {
                    // History: this consumer's pending entries from `start` on.
                    // An entry that has since been deleted from the stream is
                    // reported as its ID with no fields and stays in the PEL.
                    s = touch_stream_consumer(
                        txn, user_key, group_name, group, consumer_name, false, consumer, created);
                    if (s.ok() && created) {
                        s = write_stream_group(txn, user_key, group_name, group);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (mode == "HISTORY") {
                        std::vector<std::pair<RedisStreamId, RedisStreamNack>> pel;
                        s = collect_stream_pel(txn, user_key, group_name, start, max_id, 0, pel);
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                        int64_t served = 0;
                        for (auto& [id, nack] : pel) {
                            if (nack.consumer != consumer_name) {
                                continue;
                            }
                            if (count > 0 && served >= count) {
                                break;
                            }
                            std::string fields;
                            bool entry_exists = false;
                            s = read_stream_entry(txn, user_key, id, fields, entry_exists);
                            if (!s.ok()) {
                                break;
                            }
                            payload.push_back(stream_id_format(id));
                            payload.push_back(entry_exists ? fields : std::string());
                            served += 1;
                            if (!entry_exists) {
                                continue;
                            }
                            nack.delivery_time_ms = now;
                            nack.delivery_count += 1;
                            s = write_stream_nack(txn, user_key, group_name, id, nack);
                            if (!s.ok()) {
                                break;
                            }
                        }
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                    }
                }
                result.success = true;
                result.int_value = 0;
                if (!copy_result_value(result, pack_bytes_list(payload))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_XACK) {
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() < 2) {
                    all_success = false;
                    continue;
                }
                const std::string group_name = args[0];
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                RedisStreamGroup group;
                bool group_exists = false;
                if (s.ok() && exists) {
                    s = read_stream_group(txn, user_key, group_name, group, group_exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t acknowledged = 0;
                if (group_exists) {
                    for (size_t i = 1; i < args.size(); ++i) {
                        RedisStreamId id;
                        if (!stream_id_parse(args[i], id)) {
                            continue;
                        }
                        RedisStreamNack nack;
                        bool nack_exists = false;
                        s = read_stream_nack(txn, user_key, group_name, id, nack, nack_exists);
                        if (!s.ok()) {
                            break;
                        }
                        if (!nack_exists) {
                            continue;
                        }
                        s = remove_stream_nack(txn, user_key, group_name, group, id, nack);
                        if (!s.ok()) {
                            break;
                        }
                        acknowledged += 1;
                    }
                    if (s.ok() && acknowledged > 0) {
                        s = write_stream_group(txn, user_key, group_name, group);
                    }
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = acknowledged;
            } else if (op.op == TXN_OP_XPENDING) {
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() < 2) {
                    all_success = false;
                    continue;
                }
                const std::string group_name = args[0];
                const bool summary = args[1] == "SUMMARY";
                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                RedisStreamGroup group;
                bool group_exists = false;
                if (s.ok() && exists) {
                    s = read_stream_group(txn, user_key, group_name, group, group_exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists || !group_exists) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NOGROUP;
                    continue;
                }
                const int64_t now = now_unix_ms();
                RedisStreamId min_id;
                RedisStreamId max_id;
                max_id.ms = std::numeric_limits<uint64_t>::max();
                max_id.seq = std::numeric_limits<uint64_t>::max();
                std::vector<std::string> payload;
                if (summary) {
                    std::vector<std::pair<RedisStreamId, RedisStreamNack>> pel;
                    s = collect_stream_pel(txn, user_key, group_name, min_id, max_id, 0, pel);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    payload.push_back(std::to_string(pel.size()));
                    payload.push_back(pel.empty() ? std::string() : stream_id_format(pel.front().first));
                    payload.push_back(pel.empty() ? std::string() : stream_id_format(pel.back().first));
                    std::map<std::string, int64_t> per_consumer;
                    for (const auto& [id, nack] : pel) {
                        (void)id;
                        per_consumer[nack.consumer] += 1;
                    }
                    payload.push_back(std::to_string(per_consumer.size()));
                    for (const auto& [name, owned] : per_consumer) {
                        payload.push_back(name);
                        payload.push_back(std::to_string(owned));
                    }
                } else {
                    if (args.size() < 7) {
                        all_success = false;
                        continue;
                    }
                    int64_t min_idle = 0;
                    parse_int64(args[2], min_idle);
                    RedisStreamId start;
                    RedisStreamId end;
                    if (!stream_id_parse(args[3], start) || !stream_id_parse(args[4], end)) {
                        all_success = false;
                        continue;
                    }
                    int64_t count = 0;
                    parse_int64(args[5], count);
                    const std::string only_consumer = args[6];
                    std::vector<std::pair<RedisStreamId, RedisStreamNack>> pel;
                    s = collect_stream_pel(txn, user_key, group_name, start, end, 0, pel);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    std::vector<std::string> rows;
                    int64_t shown = 0;
                    for (const auto& [id, nack] : pel) {
                        if (count > 0 && shown >= count) {
                            break;
                        }
                        if (!only_consumer.empty() && nack.consumer != only_consumer) {
                            continue;
                        }
                        const int64_t idle = std::max<int64_t>(0, now - nack.delivery_time_ms);
                        if (min_idle > 0 && idle < min_idle) {
                            continue;
                        }
                        rows.push_back(stream_id_format(id));
                        rows.push_back(nack.consumer);
                        rows.push_back(std::to_string(idle));
                        rows.push_back(std::to_string(nack.delivery_count));
                        shown += 1;
                    }
                    payload.push_back(std::to_string(shown));
                    for (const auto& item : rows) {
                        payload.push_back(item);
                    }
                }
                result.success = true;
                result.int_value = summary ? 1 : 0;
                if (!copy_result_value(result, pack_bytes_list(payload))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_XCLAIM || op.op == TXN_OP_XAUTOCLAIM) {
                const bool autoclaim = op.op == TXN_OP_XAUTOCLAIM;
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args)
                    || args.size() < (autoclaim ? 6u : 10u)) {
                    all_success = false;
                    continue;
                }
                const std::string group_name = args[0];
                const std::string consumer_name = args[1];
                int64_t min_idle = 0;
                parse_int64(args[2], min_idle);

                bool allowed = false;
                mako::Status s = stream_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_stream_if_needed(txn, user_key);
                RedisStreamMeta meta;
                bool exists = false;
                if (s.ok()) {
                    s = read_stream_meta(txn, user_key, meta, exists);
                }
                RedisStreamGroup group;
                bool group_exists = false;
                if (s.ok() && exists) {
                    s = read_stream_group(txn, user_key, group_name, group, group_exists);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!exists || !group_exists) {
                    result.success = true;
                    result.value_present = false;
                    result.int_value = TXN_STREAM_ERR_NOGROUP;
                    continue;
                }

                const int64_t now = now_unix_ms();
                RedisStreamId min_id;
                RedisStreamId max_id;
                max_id.ms = std::numeric_limits<uint64_t>::max();
                max_id.seq = std::numeric_limits<uint64_t>::max();
                RedisStreamConsumer consumer;
                bool created = false;
                bool consumer_loaded = false;
                std::vector<std::string> payload;
                std::vector<std::string> claimed;
                std::vector<std::string> deleted;

                // Claiming one entry: the record moves to this consumer, both
                // pending counts follow it, and the delivery clock is reset.
                auto claim_one = [&](const RedisStreamId& id, RedisStreamNack& nack,
                                     int64_t delivery_time, int64_t retrycount, bool justid) {
                    if (nack.consumer != consumer_name) {
                        RedisStreamConsumer previous;
                        bool previous_exists = false;
                        mako::Status cs = read_stream_consumer(
                            txn, user_key, group_name, nack.consumer, previous, previous_exists);
                        if (!cs.ok()) {
                            return cs;
                        }
                        if (previous_exists) {
                            if (previous.pending > 0) {
                                previous.pending -= 1;
                            }
                            cs = write_stream_consumer(
                                txn, user_key, group_name, nack.consumer, previous);
                            if (!cs.ok()) {
                                return cs;
                            }
                        }
                        nack.consumer = consumer_name;
                        consumer.pending += 1;
                    }
                    nack.delivery_time_ms = delivery_time;
                    if (retrycount >= 0) {
                        nack.delivery_count = static_cast<uint64_t>(retrycount);
                    } else if (!justid) {
                        nack.delivery_count += 1;
                    }
                    return write_stream_nack(txn, user_key, group_name, id, nack);
                };

                auto load_consumer = [&]() {
                    if (consumer_loaded) {
                        return mako::Status::OK();
                    }
                    consumer_loaded = true;
                    return touch_stream_consumer(
                        txn, user_key, group_name, group, consumer_name, true, consumer, created);
                };

                if (!autoclaim) {
                    const bool justid = args[3] == "1";
                    const bool force = args[4] == "1";
                    int64_t delivery_time = now;
                    if (!args[5].empty()) {
                        int64_t idle = 0;
                        parse_int64(args[5], idle);
                        delivery_time = now - idle;
                    } else if (!args[6].empty()) {
                        parse_int64(args[6], delivery_time);
                    }
                    if (delivery_time < 0 || delivery_time > now) {
                        delivery_time = now;
                    }
                    int64_t retrycount = -1;
                    if (!args[7].empty()) {
                        parse_int64(args[7], retrycount);
                    }
                    bool last_id_given = !args[8].empty();
                    RedisStreamId last_id;
                    if (last_id_given && !stream_id_parse(args[8], last_id)) {
                        all_success = false;
                        continue;
                    }
                    for (size_t i = 9; i < args.size(); ++i) {
                        RedisStreamId id;
                        if (!stream_id_parse(args[i], id)) {
                            continue;
                        }
                        std::string fields;
                        bool entry_exists = false;
                        s = read_stream_entry(txn, user_key, id, fields, entry_exists);
                        if (!s.ok()) {
                            break;
                        }
                        RedisStreamNack nack;
                        bool nack_exists = false;
                        s = read_stream_nack(txn, user_key, group_name, id, nack, nack_exists);
                        if (!s.ok()) {
                            break;
                        }
                        if (!entry_exists) {
                            // Redis 7 drops a pending entry whose stream entry
                            // is gone and reports nothing for it.
                            if (nack_exists) {
                                s = remove_stream_nack(txn, user_key, group_name, group, id, nack);
                                if (!s.ok()) {
                                    break;
                                }
                            }
                            continue;
                        }
                        if (!nack_exists) {
                            if (!force) {
                                continue;
                            }
                            nack = RedisStreamNack{};
                            nack.consumer.clear();
                            nack.delivery_time_ms = now;
                            nack.delivery_count = 1;
                            group.pel += 1;
                        } else if (min_idle > 0
                                   && std::max<int64_t>(0, now - nack.delivery_time_ms) < min_idle) {
                            continue;
                        }
                        s = load_consumer();
                        if (!s.ok()) {
                            break;
                        }
                        s = claim_one(id, nack, delivery_time, retrycount, justid);
                        if (!s.ok()) {
                            break;
                        }
                        claimed.push_back(stream_id_format(id));
                        claimed.push_back(justid ? std::string() : fields);
                    }
                    if (s.ok() && last_id_given
                        && stream_id_compare(last_id, group.last_id) > 0) {
                        group.last_id = last_id;
                    }
                } else {
                    RedisStreamId start;
                    if (!stream_id_parse(args[3], start)) {
                        all_success = false;
                        continue;
                    }
                    int64_t count = 0;
                    parse_int64(args[4], count);
                    if (count <= 0) {
                        count = 100;
                    }
                    const bool justid = args[5] == "1";
                    std::vector<std::pair<RedisStreamId, RedisStreamNack>> pel;
                    s = collect_stream_pel(txn, user_key, group_name, start, max_id, 0, pel);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    // Redis walks at most count*10 pending entries and claims or
                    // drops at most count of them; the cursor is the next entry
                    // it did not look at, or 0-0 once the list ran out.
                    int64_t attempts = count * 10;
                    int64_t remaining = count;
                    size_t index = 0;
                    for (; index < pel.size(); ++index) {
                        if (attempts <= 0 || remaining <= 0) {
                            break;
                        }
                        attempts -= 1;
                        auto& [id, nack] = pel[index];
                        if (min_idle > 0
                            && std::max<int64_t>(0, now - nack.delivery_time_ms) < min_idle) {
                            continue;
                        }
                        s = load_consumer();
                        if (!s.ok()) {
                            break;
                        }
                        std::string fields;
                        bool entry_exists = false;
                        s = read_stream_entry(txn, user_key, id, fields, entry_exists);
                        if (!s.ok()) {
                            break;
                        }
                        if (!entry_exists) {
                            s = remove_stream_nack(txn, user_key, group_name, group, id, nack);
                            if (!s.ok()) {
                                break;
                            }
                            deleted.push_back(stream_id_format(id));
                            remaining -= 1;
                            continue;
                        }
                        s = claim_one(id, nack, now, -1, justid);
                        if (!s.ok()) {
                            break;
                        }
                        claimed.push_back(stream_id_format(id));
                        claimed.push_back(justid ? std::string() : fields);
                        remaining -= 1;
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    payload.push_back(index < pel.size()
                        ? stream_id_format(pel[index].first)
                        : stream_id_format(RedisStreamId{}));
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (consumer_loaded) {
                    s = write_stream_consumer(txn, user_key, group_name, consumer_name, consumer);
                }
                if (s.ok()) {
                    s = write_stream_group(txn, user_key, group_name, group);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                payload.push_back(std::to_string(claimed.size() / 2));
                for (const auto& item : claimed) {
                    payload.push_back(item);
                }
                if (autoclaim) {
                    payload.push_back(std::to_string(deleted.size()));
                    for (const auto& item : deleted) {
                        payload.push_back(item);
                    }
                }
                result.success = true;
                result.int_value = 0;
                if (!copy_result_value(result, pack_bytes_list(payload))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_XRESTORE) {
                // RESTORE of a MAKO_STREAM_DUMP payload: replace whatever is at
                // the key with the records the dump carried, which are the
                // whole stream including its groups, consumers and PELs.
                std::vector<std::string> records;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, records) || records.size() % 3 != 0) {
                    all_success = false;
                    continue;
                }
                std::string storage_key = "table_key_" + user_key;
                mako::Status s = delete_raw_if_exists(txn, storage_key);
                if (s.ok()) {
                    batch_exists[storage_key] = false;
                    batch_values.erase(storage_key);
                    s = delete_set(txn, user_key);
                }
                if (s.ok()) {
                    s = delete_list(txn, user_key);
                }
                if (s.ok()) {
                    s = delete_zset(txn, user_key);
                }
                if (s.ok()) {
                    s = delete_hash(txn, user_key);
                }
                if (s.ok()) {
                    s = delete_stream(txn, user_key);
                }
                if (s.ok()) {
                    s = clear_ttl_meta(txn, user_key);
                }
                if (s.ok()) {
                    s = write_stream_records(txn, user_key, records);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
            } else if (op.op == TXN_OP_RESTORE_LIST) {
                std::vector<std::string> values;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, values)) {
                    all_success = false;
                    continue;
                }
                auto delete_logical_key_for_restore = [&](const std::string& logical_key) {
                    std::string storage_key = "table_key_" + logical_key;
                    mako::Status ds = delete_raw_if_exists(txn, storage_key);
                    if (!ds.ok()) {
                        return ds;
                    }
                    batch_exists[storage_key] = false;
                    batch_values.erase(storage_key);
                    ds = delete_set(txn, logical_key);
                    if (ds.ok()) {
                        ds = delete_zset(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = delete_hash(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = delete_stream(txn, logical_key);
                    }
                    if (ds.ok()) {
                        ds = clear_ttl_meta(txn, logical_key);
                    }
                    return ds;
                };
                mako::Status s = delete_logical_key_for_restore(user_key);
                if (s.ok() && !values.empty()) {
                    s = rewrite_list_values(txn, user_key, values);
                }
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
            } else if (op.op == TXN_OP_SORT) {
                std::vector<std::string> options;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, options) || options.size() < 3) {
                    all_success = false;
                    continue;
                }
                const std::string& destination = options[0];
                const bool alpha = options[1] == "1";
                const bool desc = options[2] == "1";

                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::vector<std::string> sorted = staged_lists[user_key];
                if (alpha) {
                    std::sort(sorted.begin(), sorted.end());
                } else {
                    bool numeric_ok = true;
                    std::sort(sorted.begin(), sorted.end(), [&](const std::string& lhs, const std::string& rhs) {
                        char* lhs_end = nullptr;
                        char* rhs_end = nullptr;
                        double lhs_value = std::strtod(lhs.c_str(), &lhs_end);
                        double rhs_value = std::strtod(rhs.c_str(), &rhs_end);
                        if (lhs_end == lhs.c_str() || *lhs_end != '\0'
                            || rhs_end == rhs.c_str() || *rhs_end != '\0') {
                            numeric_ok = false;
                            return lhs < rhs;
                        }
                        if (lhs_value == rhs_value) {
                            return lhs < rhs;
                        }
                        return lhs_value < rhs_value;
                    });
                    if (!numeric_ok) {
                        result.success = false;
                        continue;
                    }
                }
                if (desc) {
                    std::reverse(sorted.begin(), sorted.end());
                }
                // LIMIT offset count (Redis semantics: negative offset -> 0,
                // negative count -> everything after the offset).
                if (options.size() >= 5) {
                    long long limit_offset = std::strtoll(options[3].c_str(), nullptr, 10);
                    const long long limit_count = std::strtoll(options[4].c_str(), nullptr, 10);
                    if (limit_offset < 0) {
                        limit_offset = 0;
                    }
                    if (limit_count >= 0 || limit_offset > 0) {
                        const size_t begin = std::min(static_cast<size_t>(limit_offset), sorted.size());
                        size_t end = sorted.size();
                        if (limit_count >= 0) {
                            end = std::min(sorted.size(), begin + static_cast<size_t>(limit_count));
                        }
                        sorted = std::vector<std::string>(sorted.begin() + begin, sorted.begin() + end);
                    }
                }

                if (!destination.empty()) {
                    auto delete_logical_key_for_sort = [&](const std::string& logical_key) {
                        std::string storage_key = "table_key_" + logical_key;
                        mako::Status ds = delete_raw_if_exists(txn, storage_key);
                        if (!ds.ok()) {
                            return ds;
                        }
                        batch_exists[storage_key] = false;
                        batch_values.erase(storage_key);
                        ds = delete_set(txn, logical_key);
                        if (ds.ok()) {
                            ds = delete_list(txn, logical_key);
                        }
                        if (ds.ok()) {
                            ds = delete_zset(txn, logical_key);
                        }
                        if (ds.ok()) {
                            ds = delete_hash(txn, logical_key);
                        }
                        if (ds.ok()) {
                            ds = delete_stream(txn, logical_key);
                        }
                        if (ds.ok()) {
                            ds = clear_ttl_meta(txn, logical_key);
                        }
                        return ds;
                    };
                    s = delete_logical_key_for_sort(destination);
                    if (s.ok() && !sorted.empty()) {
                        s = rewrite_list_values(txn, destination, sorted);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    result.success = true;
                    result.value_present = true;
                    result.int_value = static_cast<int64_t>(sorted.size());
                } else {
                    result.success = true;
                    result.value_present = true;
                    std::string payload = pack_bytes_list(sorted);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_HSET) {
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.size() % 2 != 0) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                int64_t cardinality = 0;
                s = read_hash_cardinality(txn, user_key, cardinality);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t added = 0;
                for (size_t part_index = 0; part_index < parts.size(); part_index += 2) {
                    const std::string& field = parts[part_index];
                    const std::string& value = parts[part_index + 1];
                    std::string field_key = hash_field_storage_key(user_key, field);
                    std::string current;
                    bool exists = false;
                    s = read_internal_current(txn, field_key, current, exists);
                    if (!s.ok()) {
                        break;
                    }
                    int64_t field_expire_at_ms = 0;
                    bool field_ttl_exists = false;
                    s = read_hash_field_ttl(txn, user_key, field, field_expire_at_ms, field_ttl_exists);
                    if (!s.ok()) {
                        break;
                    }
                    // A field whose time has passed counts as absent, so HSET
                    // reports it as added and HSETNX writes over it. The record
                    // is overwritten in place rather than deleted first, which
                    // saves the delete; delete_raw_if_exists would be safe here
                    // too, since it only records the removal and put_raw
                    // cancels it.
                    const bool live =
                        exists && !(field_ttl_exists && field_expire_at_ms <= now_unix_ms());
                    if ((op.flags & TXN_FLAG_SET_NX) != 0 && live) {
                        continue;
                    }
                    s = put_raw(txn, field_key, value);
                    if (!s.ok()) {
                        break;
                    }
                    batch_exists[field_key] = true;
                    batch_values[field_key] = value;
                    if (field_ttl_exists) {
                        // Redis 7.4 discards a field's expiration whenever its
                        // value is overwritten.
                        s = clear_hash_field_ttl(txn, user_key, field);
                        if (!s.ok()) {
                            break;
                        }
                    }
                    if (!live) {
                        ++added;
                    }
                    // The cardinality counts stored records, and an expired
                    // field still had one, so it only grows for a new record.
                    if (!exists) {
                        ++cardinality;
                    }
                }
                if (s.ok()) {
                    s = write_hash_cardinality(txn, user_key, cardinality);
                }
                result.success = s.ok();
                result.value_present = true;
                result.int_value = added;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HGET) {
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string field(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                bool expired = false;
                s = expire_hash_field_if_needed(txn, user_key, field, expired);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                std::string value;
                bool exists = false;
                s = read_internal_current(txn, hash_field_storage_key(user_key, field), value, exists);
                result.success = s.ok();
                result.value_present = exists;
                if (!s.ok()) {
                    all_success = false;
                } else if (exists && !copy_result_value(result, value)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HMGET) {
                std::vector<std::string> fields;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, fields)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::vector<std::string> payload_items;
                payload_items.reserve(fields.size() * 2);
                for (const auto& field : fields) {
                    bool expired = false;
                    s = expire_hash_field_if_needed(txn, user_key, field, expired);
                    if (!s.ok()) {
                        break;
                    }
                    std::string value;
                    bool exists = false;
                    s = read_internal_current(txn, hash_field_storage_key(user_key, field), value, exists);
                    if (!s.ok()) {
                        break;
                    }
                    payload_items.push_back(exists ? "1" : "0");
                    payload_items.push_back(exists ? value : std::string());
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                std::string payload = pack_bytes_list(payload_items);
                result.success = true;
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HGETALL || op.op == TXN_OP_HKEYS || op.op == TXN_OP_HVALS || op.op == TXN_OP_HSCAN) {
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                // HGETALL/HKEYS/HVALS/HSCAN and HRANDFIELD (which shares this
                // op) must not report a field whose time has passed: one sweep
                // of the TTL prefix drops them before the entries are read.
                s = expire_hash_fields_due(txn, user_key);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                std::map<std::string, std::string> entries;
                s = collect_hash_entries(txn, user_key, entries);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                std::vector<std::string> payload_items;
                if (op.op == TXN_OP_HKEYS) {
                    payload_items.reserve(entries.size());
                    for (const auto& [field, _] : entries) {
                        payload_items.push_back(field);
                    }
                } else if (op.op == TXN_OP_HVALS) {
                    payload_items.reserve(entries.size());
                    for (const auto& [_, value] : entries) {
                        payload_items.push_back(value);
                    }
                } else {
                    payload_items.reserve(entries.size() * 2);
                    for (const auto& [field, value] : entries) {
                        payload_items.push_back(field);
                        payload_items.push_back(value);
                    }
                }
                std::string payload = pack_bytes_list(payload_items);
                result.success = true;
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HDEL) {
                std::vector<std::string> fields;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, fields)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                int64_t cardinality = 0;
                s = read_hash_cardinality(txn, user_key, cardinality);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t removed = 0;
                for (const auto& field : fields) {
                    std::string field_key = hash_field_storage_key(user_key, field);
                    std::string value;
                    bool exists = false;
                    s = read_internal_current(txn, field_key, value, exists);
                    if (!s.ok()) {
                        break;
                    }
                    if (!exists) {
                        continue;
                    }
                    s = delete_raw_if_exists(txn, field_key);
                    if (!s.ok()) {
                        break;
                    }
                    batch_exists[field_key] = false;
                    batch_values.erase(field_key);
                    // Redis deliberately does not skip an already-expired field
                    // here -- HDEL still reports it as deleted -- but the side
                    // key has to go with the field either way.
                    int64_t field_expire_at_ms = 0;
                    bool field_ttl_exists = false;
                    s = read_hash_field_ttl(txn, user_key, field, field_expire_at_ms, field_ttl_exists);
                    if (!s.ok()) {
                        break;
                    }
                    if (field_ttl_exists) {
                        s = clear_hash_field_ttl(txn, user_key, field);
                        if (!s.ok()) {
                            break;
                        }
                    }
                    ++removed;
                    --cardinality;
                }
                if (s.ok()) {
                    s = write_hash_cardinality(txn, user_key, cardinality);
                }
                if (s.ok() && cardinality <= 0) {
                    s = clear_ttl_meta(txn, user_key);
                }
                result.success = s.ok();
                result.value_present = true;
                result.int_value = removed;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HEXISTS || op.op == TXN_OP_HSTRLEN) {
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string field(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                bool expired = false;
                s = expire_hash_field_if_needed(txn, user_key, field, expired);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                std::string value;
                bool exists = false;
                s = read_internal_current(txn, hash_field_storage_key(user_key, field), value, exists);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = op.op == TXN_OP_HEXISTS
                    ? (exists ? 1 : 0)
                    : (exists ? static_cast<int64_t>(value.size()) : 0);
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HLEN) {
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                int64_t cardinality = 0;
                s = read_hash_cardinality_live(txn, user_key, cardinality);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = s.ok() ? cardinality : 0;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HINCRBY || op.op == TXN_OP_HINCRBYFLOAT) {
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.size() != 2) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                const std::string& field = parts[0];
                std::string field_key = hash_field_storage_key(user_key, field);
                std::string current;
                bool exists = false;
                s = read_internal_current(txn, field_key, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t field_expire_at_ms = 0;
                bool field_ttl_exists = false;
                s = read_hash_field_ttl(txn, user_key, field, field_expire_at_ms, field_ttl_exists);
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                // An expired field is discarded and the increment starts from
                // zero again; a live one keeps its expiration, because HINCRBY
                // changes the value and not the time. The stale record is
                // overwritten in place, which saves a delete the executor
                // would otherwise defer to the end of the transaction.
                const bool field_expired =
                    field_ttl_exists && field_expire_at_ms <= now_unix_ms();
                const bool live = exists && !field_expired;
                std::string next_str;
                if (op.op == TXN_OP_HINCRBY) {
                    int64_t base = 0;
                    int64_t delta = 0;
                    int64_t next = 0;
                    if ((live && !parse_int64(current, base)) || !parse_int64(parts[1], delta)) {
                        result.success = false;
                        result.int_value = -1;
                        continue;
                    }
                    if (!add_int64(base, delta, next)) {
                        result.success = false;
                        result.int_value = -2;
                        continue;
                    }
                    next_str = std::to_string(next);
                    result.int_value = next;
                } else {
                    long double base = 0;
                    long double delta = 0;
                    if ((live && !parse_float(current, base)) || !parse_float(parts[1], delta)) {
                        result.success = false;
                        result.int_value = -3;
                        continue;
                    }
                    long double next = base + delta;
                    if (!std::isfinite(next)) {
                        result.success = false;
                        result.int_value = -3;
                        continue;
                    }
                    next_str = format_float(next);
                }
                s = put_raw(txn, field_key, next_str);
                if (s.ok()) {
                    batch_exists[field_key] = true;
                    batch_values[field_key] = next_str;
                    if (field_expired) {
                        s = clear_hash_field_ttl(txn, user_key, field);
                    }
                    // The cardinality counts stored records, and an expired
                    // field still had one, so it only grows for a new record.
                    if (s.ok() && !exists) {
                        int64_t cardinality = 0;
                        s = read_hash_cardinality(txn, user_key, cardinality);
                        if (s.ok()) {
                            s = write_hash_cardinality(txn, user_key, cardinality + 1);
                        }
                    }
                }
                result.success = s.ok();
                result.value_present = true;
                if (!s.ok()) {
                    all_success = false;
                } else if (op.op == TXN_OP_HINCRBYFLOAT && !copy_result_value(result, next_str)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HFIELD_EXPIRE) {
                // HEXPIRE/HPEXPIRE/HEXPIREAT/HPEXPIREAT. Rust has already
                // turned the requested time into absolute Unix ms in
                // op.expire_at_ms and packed [mode, field ...] into the value.
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.empty()) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                const std::string& mode = parts[0];
                const int64_t now_ms = now_unix_ms();
                std::vector<std::string> codes;
                codes.reserve(parts.size() - 1);
                for (size_t index = 1; index < parts.size(); ++index) {
                    const std::string& field = parts[index];
                    bool expired = false;
                    s = expire_hash_field_if_needed(txn, user_key, field, expired);
                    if (!s.ok()) {
                        break;
                    }
                    std::string ignored;
                    bool field_exists = false;
                    s = read_internal_current(
                        txn, hash_field_storage_key(user_key, field), ignored, field_exists);
                    if (!s.ok()) {
                        break;
                    }
                    if (!field_exists) {
                        codes.push_back("-2");
                        continue;
                    }
                    int64_t current_expire_at_ms = 0;
                    bool ttl_exists = false;
                    s = read_hash_field_ttl(txn, user_key, field, current_expire_at_ms, ttl_exists);
                    if (!s.ok()) {
                        break;
                    }
                    bool should_update = true;
                    if (mode == "NX") {
                        should_update = !ttl_exists;
                    } else if (mode == "XX") {
                        should_update = ttl_exists;
                    } else if (mode == "GT") {
                        should_update = ttl_exists && op.expire_at_ms > current_expire_at_ms;
                    } else if (mode == "LT") {
                        should_update = !ttl_exists || op.expire_at_ms < current_expire_at_ms;
                    }
                    if (!should_update) {
                        codes.push_back("0");
                        continue;
                    }
                    if (op.expire_at_ms > now_ms) {
                        s = write_hash_field_ttl(txn, user_key, field, op.expire_at_ms);
                        if (!s.ok()) {
                            break;
                        }
                        codes.push_back("1");
                        continue;
                    }
                    // The time is already past, so Redis deletes the field now
                    // and answers 2 instead of storing an expiration.
                    if (ttl_exists) {
                        s = clear_hash_field_ttl(txn, user_key, field);
                        if (!s.ok()) {
                            break;
                        }
                    }
                    std::string field_key = hash_field_storage_key(user_key, field);
                    s = delete_raw_if_exists(txn, field_key);
                    if (!s.ok()) {
                        break;
                    }
                    batch_exists[field_key] = false;
                    batch_values.erase(field_key);
                    int64_t cardinality = 0;
                    s = read_hash_cardinality(txn, user_key, cardinality);
                    if (!s.ok()) {
                        break;
                    }
                    cardinality -= 1;
                    s = write_hash_cardinality(txn, user_key, cardinality);
                    if (!s.ok()) {
                        break;
                    }
                    if (cardinality <= 0) {
                        s = delete_hash(txn, user_key);
                        if (s.ok()) {
                            s = clear_ttl_meta(txn, user_key);
                        }
                        if (!s.ok()) {
                            break;
                        }
                    }
                    codes.push_back("2");
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                if (!copy_result_value(result, pack_bytes_list(codes))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_HFIELD_TTL || op.op == TXN_OP_HFIELD_PERSIST) {
                // HTTL/HPTTL/HEXPIRETIME/HPEXPIRETIME report the absolute time
                // and let Rust convert it; HPERSIST removes it.
                std::vector<std::string> fields;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, fields)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = hash_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::vector<std::string> codes;
                codes.reserve(fields.size());
                for (const auto& field : fields) {
                    bool expired = false;
                    s = expire_hash_field_if_needed(txn, user_key, field, expired);
                    if (!s.ok()) {
                        break;
                    }
                    std::string ignored;
                    bool field_exists = false;
                    s = read_internal_current(
                        txn, hash_field_storage_key(user_key, field), ignored, field_exists);
                    if (!s.ok()) {
                        break;
                    }
                    if (!field_exists) {
                        codes.push_back("-2");
                        continue;
                    }
                    int64_t expire_at_ms = 0;
                    bool ttl_exists = false;
                    s = read_hash_field_ttl(txn, user_key, field, expire_at_ms, ttl_exists);
                    if (!s.ok()) {
                        break;
                    }
                    if (!ttl_exists) {
                        codes.push_back("-1");
                        continue;
                    }
                    if (op.op == TXN_OP_HFIELD_TTL) {
                        codes.push_back(std::to_string(expire_at_ms));
                        continue;
                    }
                    s = clear_hash_field_ttl(txn, user_key, field);
                    if (!s.ok()) {
                        break;
                    }
                    codes.push_back("1");
                }
                if (!s.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                result.success = true;
                if (!copy_result_value(result, pack_bytes_list(codes))) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SADD) {
                std::vector<std::string> members;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, members)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_set_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                int64_t added = 0;
                auto& staged = staged_sets[user_key];
                for (const auto& member : members) {
                    auto [_, inserted] = staged.insert(member);
                    if (inserted) {
                        ++added;
                    }
                }
                dirty_sets.insert(user_key);
                result.success = true;
                result.value_present = true;
                result.int_value = added;
            } else if (op.op == TXN_OP_SREM) {
                std::vector<std::string> members;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, members)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_set_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                int64_t removed = 0;
                auto& staged = staged_sets[user_key];
                for (const auto& member : members) {
                    removed += static_cast<int64_t>(staged.erase(member));
                }
                dirty_sets.insert(user_key);
                result.success = true;
                result.value_present = true;
                result.int_value = removed;
            } else if (op.op == TXN_OP_SISMEMBER) {
                std::string member(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                bool allowed = false;
                mako::Status s = set_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_set_if_needed(txn, user_key);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                bool exists = false;
                std::string member_key = set_storage_key(user_key, member);
                auto batch_it = batch_exists.find(member_key);
                if (batch_it != batch_exists.end()) {
                    exists = batch_it->second;
                    s = mako::Status::OK();
                } else {
                    s = read_raw_exists(txn, member_key, exists);
                }
                result.success = s.ok();
                result.value_present = true;
                result.int_value = exists ? 1 : 0;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SCARD) {
                bool allowed = false;
                mako::Status s = set_key_allowed(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                s = expire_set_if_needed(txn, user_key);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t cardinality = 0;
                s = read_set_cardinality(txn, user_key, cardinality);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = cardinality;
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SMEMBERS) {
                std::vector<std::string> members;
                mako::Status s = read_set_members(txn, user_key, members);
                result.success = s.ok();
                if (!s.ok()) {
                    continue;
                }
                std::string payload = pack_bytes_list(members);
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SMOVE) {
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.size() != 2) {
                    all_success = false;
                    continue;
                }
                const std::string& destination = parts[0];
                const std::string& member = parts[1];
                bool source_allowed = false;
                mako::Status s = load_set_stage(txn, user_key, result, source_allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!source_allowed) {
                    continue;
                }
                bool destination_allowed = false;
                TxnOpResult destination_result{};
                s = load_set_stage(txn, destination, destination_result, destination_allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!destination_allowed) {
                    result.success = false;
                    continue;
                }
                auto& source_members = staged_sets[user_key];
                result.success = true;
                result.value_present = true;
                auto source_it = source_members.find(member);
                const bool source_exists = source_it != source_members.end();
                result.int_value = source_exists ? 1 : 0;
                if (!source_exists || user_key == destination) {
                    continue;
                }
                source_members.erase(source_it);
                staged_sets[destination].insert(member);
                dirty_sets.insert(user_key);
                dirty_sets.insert(destination);
            } else if (op.op == TXN_OP_SPOP || op.op == TXN_OP_SRANDMEMBER) {
                std::vector<std::string> members;
                mako::Status s = read_set_members(txn, user_key, members);
                if (!s.ok()) {
                    result.success = false;
                    continue;
                }
                const bool count_given = (op.flags & TXN_FLAG_SET_COUNT_GIVEN) != 0;
                const bool allow_duplicates = (op.flags & TXN_FLAG_SET_ALLOW_DUPLICATES) != 0;
                int64_t requested = count_given ? op.expire_at_ms : 1;
                if (requested < 0) {
                    requested = 0;
                }
                std::vector<std::string> selected;
                if (!members.empty() && requested > 0) {
                    thread_local std::mt19937_64 rng([] {
                        std::random_device rd;
                        uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
                        seed ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                        seed ^= static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
                        return seed;
                    }());
                    if (allow_duplicates) {
                        std::uniform_int_distribution<size_t> dist(0, members.size() - 1);
                        selected.reserve(static_cast<size_t>(requested));
                        for (int64_t idx = 0; idx < requested; ++idx) {
                            selected.push_back(members[dist(rng)]);
                        }
                    } else {
                        const size_t take = std::min<size_t>(members.size(), static_cast<size_t>(requested));
                        std::shuffle(members.begin(), members.end(), rng);
                        selected.reserve(take);
                        for (size_t idx = 0; idx < take; ++idx) {
                            selected.push_back(members[idx]);
                        }
                    }
                }
                if (op.op == TXN_OP_SPOP && !selected.empty()) {
                    auto staged_it = staged_sets.find(user_key);
                    if (staged_it != staged_sets.end()) {
                        for (const auto& member : selected) {
                            staged_it->second.erase(member);
                        }
                        dirty_sets.insert(user_key);
                    } else {
                        int64_t cardinality = 0;
                        s = read_set_cardinality(txn, user_key, cardinality);
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                        for (const auto& member : selected) {
                            std::string member_key = set_storage_key(user_key, member);
                            s = delete_raw_if_exists(txn, member_key);
                            if (!s.ok()) {
                                all_success = false;
                                break;
                            }
                            batch_exists[member_key] = false;
                            batch_values.erase(member_key);
                            cardinality = std::max<int64_t>(0, cardinality - 1);
                        }
                        if (!all_success) {
                            continue;
                        }
                        s = write_set_cardinality(txn, user_key, cardinality);
                        if (s.ok() && cardinality == 0) {
                            s = clear_ttl_meta(txn, user_key);
                        }
                        if (!s.ok()) {
                            all_success = false;
                            continue;
                        }
                    }
                }
                result.success = true;
                result.value_present = !selected.empty();
                std::string payload = pack_bytes_list(selected);
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SET_ALGEBRA) {
                std::vector<std::string> set_keys;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, set_keys)) {
                    all_success = false;
                    continue;
                }
                std::vector<std::unordered_set<std::string>> sets;
                sets.reserve(set_keys.size());
                for (const auto& set_key : set_keys) {
                    std::vector<std::string> members;
                    mako::Status s = read_set_members(txn, set_key, members);
                    if (!s.ok()) {
                        result.success = false;
                        break;
                    }
                    sets.emplace_back(members.begin(), members.end());
                }
                if (!result.success && sets.size() != set_keys.size()) {
                    continue;
                }
                std::unordered_set<std::string> result_set;
                if ((op.flags & TXN_FLAG_SET_ALGEBRA_UNION) != 0) {
                    for (const auto& set : sets) {
                        result_set.insert(set.begin(), set.end());
                    }
                } else if (!sets.empty()) {
                    result_set = sets[0];
                    if ((op.flags & TXN_FLAG_SET_ALGEBRA_DIFF) != 0) {
                        for (size_t set_idx = 1; set_idx < sets.size(); ++set_idx) {
                            for (const auto& member : sets[set_idx]) {
                                result_set.erase(member);
                            }
                        }
                    } else {
                        for (size_t set_idx = 1; set_idx < sets.size(); ++set_idx) {
                            for (auto it = result_set.begin(); it != result_set.end();) {
                                if (sets[set_idx].find(*it) == sets[set_idx].end()) {
                                    it = result_set.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                        }
                    }
                }
                std::vector<std::string> items(result_set.begin(), result_set.end());
                std::sort(items.begin(), items.end());
                if ((op.flags & TXN_FLAG_SET_ALGEBRA_STORE) != 0) {
                    bool string_exists = false;
                    mako::Status s = read_string_exists_no_expire(txn, tl_key_buf, string_exists);
                    if (s.ok() && string_exists) {
                        s = delete_raw_if_exists(txn, tl_key_buf);
                    }
                    std::vector<std::string> existing_members;
                    bool existing_set = false;
                    if (s.ok() && !string_exists) {
                        s = read_set_members(txn, user_key, existing_members);
                        existing_set = s.ok();
                        if (!s.ok()) {
                            s = mako::Status::OK();
                        }
                    }
                    if (s.ok() && !existing_set) {
                        s = delete_list(txn, user_key);
                    }
                    if (s.ok() && !existing_set) {
                        s = delete_zset(txn, user_key);
                    }
                    if (s.ok() && !existing_set) {
                        s = delete_hash(txn, user_key);
                    }
                    std::unordered_set<std::string> desired(items.begin(), items.end());
                    std::unordered_set<std::string> existing(existing_members.begin(), existing_members.end());
                    for (const auto& member : existing_members) {
                        if (desired.find(member) != desired.end()) {
                            continue;
                        }
                        std::string member_key = set_storage_key(user_key, member);
                        s = delete_raw_if_exists(txn, member_key);
                        if (!s.ok()) {
                            all_success = false;
                            break;
                        }
                        batch_exists[member_key] = false;
                        batch_values.erase(member_key);
                    }
                    for (const auto& member : items) {
                        if (existing.find(member) != existing.end()) {
                            continue;
                        }
                        std::string member_key = set_storage_key(user_key, member);
                        s = put_raw(txn, member_key, "1");
                        if (!s.ok()) {
                            all_success = false;
                            break;
                        }
                        batch_exists[member_key] = true;
                        batch_values[member_key] = "1";
                    }
                    if (!all_success) {
                        continue;
                    }
                    s = clear_ttl_meta(txn, user_key);
                    if (s.ok()) {
                        s = write_set_cardinality(txn, user_key, static_cast<int64_t>(items.size()));
                    }
                    result.success = s.ok();
                    result.value_present = true;
                    result.int_value = static_cast<int64_t>(items.size());
                    if (!s.ok()) {
                        all_success = false;
                    }
                } else {
                    result.success = true;
                    result.value_present = true;
                    std::string payload = pack_bytes_list(items);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_LPUSH || op.op == TXN_OP_RPUSH) {
                std::vector<std::string> values;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, values)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& staged = staged_lists[user_key];
                if ((op.flags & TXN_FLAG_LIST_PUSH_IF_EXISTS) != 0 && staged.empty()) {
                    result.success = true;
                    result.value_present = true;
                    result.int_value = 0;
                    continue;
                }
                for (const auto& value : values) {
                    if (op.op == TXN_OP_LPUSH) {
                        staged.insert(staged.begin(), value);
                    } else {
                        staged.push_back(value);
                    }
                }
                dirty_lists.insert(user_key);
                result.success = true;
                result.value_present = true;
                result.int_value = static_cast<int64_t>(staged.size());
            } else if (op.op == TXN_OP_LPOP || op.op == TXN_OP_RPOP) {
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& staged = staged_lists[user_key];
                int64_t available = static_cast<int64_t>(staged.size());
                int64_t requested = (op.flags & TXN_FLAG_LIST_COUNT_GIVEN) != 0 ? op.expire_at_ms : 1;
                requested = std::clamp<int64_t>(requested, 0, available);
                std::vector<std::string> selected;
                selected.reserve(static_cast<size_t>(requested));
                for (int64_t n = 0; n < requested; ++n) {
                    if (op.op == TXN_OP_LPOP) {
                        selected.push_back(staged.front());
                        staged.erase(staged.begin());
                    } else {
                        selected.push_back(staged.back());
                        staged.pop_back();
                    }
                }
                if (requested > 0) {
                    dirty_lists.insert(user_key);
                }
                result.success = true;
                result.value_present = !selected.empty() || available > 0;
                if (!result.value_present) {
                    continue;
                }
                std::string payload = pack_bytes_list(selected);
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_BPOP) {
                std::vector<std::string> keys;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, keys)) {
                    all_success = false;
                    continue;
                }
                const bool pop_left = (op.flags & TXN_FLAG_LIST_SOURCE_LEFT) != 0;
                const int64_t requested = std::max<int64_t>(1, op.expire_at_ms);
                result.success = true;
                result.value_present = false;
                for (const auto& candidate_key : keys) {
                    bool allowed = false;
                    mako::Status s = load_list_stage(txn, candidate_key, result, allowed);
                    if (!s.ok()) {
                        all_success = false;
                        break;
                    }
                    if (!allowed) {
                        break;
                    }
                    auto& staged = staged_lists[candidate_key];
                    if (staged.empty()) {
                        continue;
                    }
                    std::vector<std::string> payload_items;
                    payload_items.push_back(candidate_key);
                    const int64_t pop_count =
                        std::min<int64_t>(requested, static_cast<int64_t>(staged.size()));
                    for (int64_t n = 0; n < pop_count; ++n) {
                        if (pop_left) {
                            payload_items.push_back(staged.front());
                            staged.erase(staged.begin());
                        } else {
                            payload_items.push_back(staged.back());
                            staged.pop_back();
                        }
                    }
                    dirty_lists.insert(candidate_key);
                    result.success = true;
                    result.value_present = true;
                    std::string payload = pack_bytes_list(payload_items);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                    break;
                }
            } else if (op.op == TXN_OP_LLEN) {
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = static_cast<int64_t>(staged_lists[user_key].size());
            } else if (op.op == TXN_OP_LINDEX) {
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& values = staged_lists[user_key];
                result.success = true;
                int64_t index = op.expire_at_ms;
                if (index < 0) {
                    index += static_cast<int64_t>(values.size());
                }
                if (index < 0 || index >= static_cast<int64_t>(values.size())) {
                    result.value_present = false;
                    continue;
                }
                if (!copy_result_value(result, values[static_cast<size_t>(index)])) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_LRANGE || op.op == TXN_OP_LTRIM) {
                std::vector<std::string> bounds;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, bounds) || bounds.size() != 2) {
                    all_success = false;
                    continue;
                }
                int64_t start_index = 0;
                int64_t stop_index = 0;
                if (!parse_int64(bounds[0], start_index) || !parse_int64(bounds[1], stop_index)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& values = staged_lists[user_key];
                const int64_t length = static_cast<int64_t>(values.size());
                if (start_index < 0) {
                    start_index += length;
                }
                if (stop_index < 0) {
                    stop_index += length;
                }
                start_index = std::max<int64_t>(0, start_index);
                stop_index = std::min<int64_t>(length - 1, stop_index);
                std::vector<std::string> selected;
                if (length > 0 && start_index <= stop_index && start_index < length) {
                    selected.assign(
                        values.begin() + static_cast<size_t>(start_index),
                        values.begin() + static_cast<size_t>(stop_index + 1));
                }
                if (op.op == TXN_OP_LTRIM) {
                    values = selected;
                    dirty_lists.insert(user_key);
                    result.success = true;
                    result.value_present = true;
                } else {
                    result.success = true;
                    result.value_present = true;
                    std::string payload = pack_bytes_list(selected);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_LSET || op.op == TXN_OP_LREM || op.op == TXN_OP_LINSERT) {
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& values = staged_lists[user_key];
                if (op.op == TXN_OP_LSET) {
                    if (parts.size() != 2) {
                        all_success = false;
                        continue;
                    }
                    int64_t index = 0;
                    if (!parse_int64(parts[0], index)) {
                        all_success = false;
                        continue;
                    }
                    if (index < 0) {
                        index += static_cast<int64_t>(values.size());
                    }
                    if (index < 0 || index >= static_cast<int64_t>(values.size())) {
                        result.success = false;
                        result.int_value = values.empty() ? -1 : -2;
                        continue;
                    }
                    values[static_cast<size_t>(index)] = parts[1];
                    dirty_lists.insert(user_key);
                    result.success = true;
                    result.value_present = true;
                } else if (op.op == TXN_OP_LREM) {
                    if (parts.size() != 2) {
                        all_success = false;
                        continue;
                    }
                    int64_t count = 0;
                    if (!parse_int64(parts[0], count)) {
                        all_success = false;
                        continue;
                    }
                    const std::string& needle = parts[1];
                    int64_t removed = 0;
                    std::vector<std::string> next;
                    if (count >= 0) {
                        for (const auto& value : values) {
                            if (value == needle && (count == 0 || removed < count)) {
                                ++removed;
                                continue;
                            }
                            next.push_back(value);
                        }
                    } else {
                        int64_t remaining = -count;
                        std::vector<bool> remove(values.size(), false);
                        for (size_t idx = values.size(); idx > 0 && removed < remaining; --idx) {
                            if (values[idx - 1] == needle) {
                                remove[idx - 1] = true;
                                ++removed;
                            }
                        }
                        for (size_t idx = 0; idx < values.size(); ++idx) {
                            if (!remove[idx]) {
                                next.push_back(values[idx]);
                            }
                        }
                    }
                    values = std::move(next);
                    if (removed > 0) {
                        dirty_lists.insert(user_key);
                    }
                    result.success = true;
                    result.value_present = true;
                    result.int_value = removed;
                } else {
                    if (parts.size() != 2) {
                        all_success = false;
                        continue;
                    }
                    auto pivot_it = std::find(values.begin(), values.end(), parts[0]);
                    result.value_present = true;
                    if (pivot_it == values.end()) {
                        result.success = true;
                        result.int_value = values.empty() ? 0 : -1;
                        continue;
                    }
                    if ((op.flags & TXN_FLAG_LIST_INSERT_BEFORE) == 0) {
                        ++pivot_it;
                    }
                    values.insert(pivot_it, parts[1]);
                    dirty_lists.insert(user_key);
                    result.success = true;
                    result.int_value = static_cast<int64_t>(values.size());
                }
            } else if (op.op == TXN_OP_LMOVE) {
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.size() != 1) {
                    all_success = false;
                    continue;
                }
                const std::string& destination = parts[0];
                bool source_allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, source_allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!source_allowed) {
                    continue;
                }
                auto& source_values = staged_lists[user_key];
                result.success = true;
                result.value_present = false;
                if (source_values.empty()) {
                    continue;
                }
                bool dest_allowed = false;
                s = load_list_stage(txn, destination, result, dest_allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!dest_allowed) {
                    continue;
                }
                std::string moved;
                if ((op.flags & TXN_FLAG_LIST_SOURCE_LEFT) != 0) {
                    moved = source_values.front();
                    source_values.erase(source_values.begin());
                } else {
                    moved = source_values.back();
                    source_values.pop_back();
                }
                if (destination == user_key) {
                    if ((op.flags & TXN_FLAG_LIST_DEST_LEFT) != 0) {
                        source_values.insert(source_values.begin(), moved);
                    } else {
                        source_values.push_back(moved);
                    }
                } else {
                    auto& dest_values = staged_lists[destination];
                    if ((op.flags & TXN_FLAG_LIST_DEST_LEFT) != 0) {
                        dest_values.insert(dest_values.begin(), moved);
                    } else {
                        dest_values.push_back(moved);
                    }
                }
                dirty_lists.insert(user_key);
                dirty_lists.insert(destination);
                result.success = true;
                if (!copy_result_value(result, moved)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_LPOS) {
                bool allowed = false;
                mako::Status s = load_list_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& values = staged_lists[user_key];
                result.success = true;
                result.value_present = false;
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.size() != 4) {
                    all_success = false;
                    continue;
                }
                const std::string& needle = parts[0];
                int64_t rank = 1;
                int64_t count = -1;
                int64_t maxlen = 0;
                if (!parse_int64(parts[1], rank) || !parse_int64(parts[2], count) || !parse_int64(parts[3], maxlen)) {
                    all_success = false;
                    continue;
                }
                const bool reverse = rank < 0;
                int64_t remaining_rank = std::llabs(rank);
                int64_t inspected = 0;
                std::vector<std::string> positions;
                auto visit_match = [&](size_t idx) {
                    if (values[idx] != needle) {
                        return false;
                    }
                    --remaining_rank;
                    if (remaining_rank > 0) {
                        return false;
                    }
                    if (count < 0) {
                        result.value_present = true;
                        result.int_value = static_cast<int64_t>(idx);
                        return true;
                    }
                    positions.push_back(std::to_string(idx));
                    return count > 0 && static_cast<int64_t>(positions.size()) >= count;
                };
                if (reverse) {
                    for (size_t offset = 0; offset < values.size(); ++offset) {
                        if (maxlen > 0 && inspected >= maxlen) {
                            break;
                        }
                        size_t idx = values.size() - 1 - offset;
                        ++inspected;
                        if (visit_match(idx)) {
                            break;
                        }
                    }
                } else {
                    for (size_t idx = 0; idx < values.size(); ++idx) {
                        if (maxlen > 0 && inspected >= maxlen) {
                            break;
                        }
                        ++inspected;
                        if (visit_match(idx)) {
                            break;
                        }
                    }
                }
                if (count >= 0) {
                    result.value_present = true;
                    std::string payload = pack_bytes_list(positions);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_ZADD) {
                std::vector<std::string> parts;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, parts) || parts.size() % 2 != 0) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& values = staged_zsets[user_key];
                const bool nx = (op.flags & TXN_FLAG_ZADD_NX) != 0;
                const bool xx = (op.flags & TXN_FLAG_ZADD_XX) != 0;
                const bool ch = (op.flags & TXN_FLAG_ZADD_CH) != 0;
                const bool incr = (op.flags & TXN_FLAG_ZADD_INCR) != 0;
                const bool gt = (op.flags & TXN_FLAG_ZADD_GT) != 0;
                const bool lt = (op.flags & TXN_FLAG_ZADD_LT) != 0;
                int64_t added = 0;
                int64_t changed = 0;
                std::optional<double> increment_result;
                bool score_nan_error = false;
                for (size_t part_idx = 0; part_idx < parts.size(); part_idx += 2) {
                    double score = 0.0;
                    if (!parse_zset_score_value(parts[part_idx], score)) {
                        all_success = false;
                        break;
                    }
                    const std::string& member = parts[part_idx + 1];
                    auto current_it = values.find(member);
                    const bool exists = current_it != values.end();
                    if (incr) {
                        if (!exists && xx) {
                            result.success = true;
                            result.value_present = false;
                            continue;
                        }
                        if (exists && nx) {
                            result.success = true;
                            result.value_present = false;
                            continue;
                        }
                        double next_score = (exists ? current_it->second : 0.0) + score;
                        if (std::isnan(next_score)) {
                            result.success = false;
                            result.int_value = -3;
                            score_nan_error = true;
                            break;
                        }
                        bool should_write = true;
                        if (gt && exists && next_score <= current_it->second) {
                            should_write = false;
                        }
                        if (lt && exists && next_score >= current_it->second) {
                            should_write = false;
                        }
                        if (!should_write) {
                            result.success = true;
                            result.value_present = false;
                            continue;
                        }
                        values[member] = next_score;
                        increment_result = next_score;
                        dirty_zsets.insert(user_key);
                        if (!exists) {
                            ++added;
                            ++changed;
                        } else if (next_score != current_it->second) {
                            ++changed;
                        }
                        continue;
                    }
                    bool should_write = true;
                    if (nx && exists) {
                        should_write = false;
                    }
                    if (xx && !exists) {
                        should_write = false;
                    }
                    if (gt && exists && score <= current_it->second) {
                        should_write = false;
                    }
                    if (lt && exists && score >= current_it->second) {
                        should_write = false;
                    }
                    if (!should_write) {
                        continue;
                    }
                    if (!exists) {
                        ++added;
                        ++changed;
                    } else if (score != current_it->second) {
                        ++changed;
                    }
                    values[member] = score;
                    dirty_zsets.insert(user_key);
                }
                if (score_nan_error) {
                    continue;
                }
                if (!all_success) {
                    continue;
                }
                result.success = true;
                result.value_present = true;
                if (incr) {
                    if (increment_result.has_value()) {
                        std::string score_text = format_zset_score(*increment_result);
                        if (!copy_result_value(result, score_text)) {
                            all_success = false;
                        }
                    } else {
                        result.value_present = false;
                    }
                } else {
                    result.int_value = ch ? changed : added;
                }
            } else if (op.op == TXN_OP_ZSCORE) {
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string member(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                auto& values = staged_zsets[user_key];
                auto value_it = values.find(member);
                result.success = true;
                if (value_it == values.end()) {
                    result.value_present = false;
                    continue;
                }
                std::string score_text = format_zset_score(value_it->second);
                if (!copy_result_value(result, score_text)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_ZREM) {
                std::vector<std::string> members;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, members)) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto& values = staged_zsets[user_key];
                int64_t removed = 0;
                for (const auto& member : members) {
                    removed += values.erase(member) > 0 ? 1 : 0;
                }
                if (removed > 0) {
                    dirty_zsets.insert(user_key);
                }
                result.success = true;
                result.value_present = true;
                result.int_value = removed;
            } else if (op.op == TXN_OP_ZCARD) {
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                result.success = true;
                result.value_present = true;
                result.int_value = static_cast<int64_t>(staged_zsets[user_key].size());
            } else if (op.op == TXN_OP_ZRANK) {
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::string member(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                auto items = zset_ordered_items(staged_zsets[user_key]);
                if ((op.flags & TXN_FLAG_Z_REV) != 0) {
                    std::reverse(items.begin(), items.end());
                }
                result.success = true;
                result.value_present = false;
                for (size_t idx = 0; idx < items.size(); ++idx) {
                    if (items[idx].first == member) {
                        result.value_present = true;
                        result.int_value = static_cast<int64_t>(idx);
                        if ((op.flags & TXN_FLAG_Z_WITHSCORES) != 0) {
                            std::string score = format_zset_score(items[idx].second);
                            if (!copy_result_value(result, score)) {
                                all_success = false;
                            }
                        }
                        break;
                    }
                }
            } else if (op.op == TXN_OP_ZRANGE || op.op == TXN_OP_ZRANGEBYLEX || op.op == TXN_OP_ZCOUNT || op.op == TXN_OP_ZLEXCOUNT) {
                std::vector<std::string> bounds;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, bounds) || bounds.size() < 2) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::vector<std::pair<std::string, double>> selected;
                bool range_ok = true;
                if (op.op == TXN_OP_ZLEXCOUNT || op.op == TXN_OP_ZRANGEBYLEX) {
                    range_ok = select_zset_lex_range(
                        staged_zsets[user_key],
                        bounds,
                        (op.flags & TXN_FLAG_Z_REV) != 0,
                        selected);
                } else if (op.op == TXN_OP_ZCOUNT || (op.flags & TXN_FLAG_Z_BYSCORE) != 0) {
                    range_ok = select_zset_score_range(
                        staged_zsets[user_key],
                        bounds,
                        (op.flags & TXN_FLAG_Z_REV) != 0,
                        selected);
                } else {
                    range_ok = select_zset_rank_range(
                        staged_zsets[user_key],
                        bounds,
                        (op.flags & TXN_FLAG_Z_REV) != 0,
                        selected);
                }
                if (!range_ok) {
                    all_success = false;
                    continue;
                }
                result.success = true;
                result.value_present = true;
                if (op.op == TXN_OP_ZCOUNT || op.op == TXN_OP_ZLEXCOUNT) {
                    result.int_value = static_cast<int64_t>(selected.size());
                } else {
                    std::vector<std::string> payload_items;
                    const bool with_scores = (op.flags & TXN_FLAG_Z_WITHSCORES) != 0;
                    for (const auto& [member, score] : selected) {
                        payload_items.push_back(member);
                        if (with_scores) {
                            payload_items.push_back(format_zset_score(score));
                        }
                    }
                    std::string payload = pack_bytes_list(payload_items);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_ZREMRANGEBYSCORE || op.op == TXN_OP_ZREMRANGEBYRANK || op.op == TXN_OP_ZREMRANGEBYLEX) {
                std::vector<std::string> bounds;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, bounds) || bounds.size() < 2) {
                    all_success = false;
                    continue;
                }
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::vector<std::pair<std::string, double>> selected;
                bool range_ok = false;
                if (op.op == TXN_OP_ZREMRANGEBYSCORE) {
                    range_ok = select_zset_score_range(staged_zsets[user_key], bounds, false, selected);
                } else if (op.op == TXN_OP_ZREMRANGEBYRANK) {
                    range_ok = select_zset_rank_range(staged_zsets[user_key], bounds, false, selected);
                } else {
                    range_ok = select_zset_lex_range(staged_zsets[user_key], bounds, false, selected);
                }
                if (!range_ok) {
                    all_success = false;
                    continue;
                }
                auto& values = staged_zsets[user_key];
                int64_t removed = 0;
                for (const auto& [member, score] : selected) {
                    removed += values.erase(member) > 0 ? 1 : 0;
                }
                if (removed > 0) {
                    dirty_zsets.insert(user_key);
                }
                result.success = true;
                result.value_present = true;
                result.int_value = removed;
            } else if (op.op == TXN_OP_ZRANGESTORE) {
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.size() < 3) {
                    all_success = false;
                    continue;
                }
                const std::string destination = user_key;
                const std::string source = args[0];
                std::vector<std::string> bounds(args.begin() + 1, args.end());
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, source, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                TxnOpResult dest_allowed_result{};
                s = zset_key_allowed(txn, destination, dest_allowed_result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    result.success = false;
                    continue;
                }
                std::vector<std::pair<std::string, double>> selected;
                bool range_ok = false;
                if (op.expire_at_ms == 2) {
                    range_ok = select_zset_lex_range(staged_zsets[source], bounds, (op.flags & TXN_FLAG_Z_REV) != 0, selected);
                } else if (op.expire_at_ms == 1) {
                    range_ok = select_zset_score_range(staged_zsets[source], bounds, (op.flags & TXN_FLAG_Z_REV) != 0, selected);
                } else {
                    range_ok = select_zset_rank_range(staged_zsets[source], bounds, (op.flags & TXN_FLAG_Z_REV) != 0, selected);
                }
                if (!range_ok) {
                    all_success = false;
                    continue;
                }
                auto& dest_values = staged_zsets[destination];
                dest_values.clear();
                for (const auto& [member, score] : selected) {
                    dest_values[member] = score;
                }
                staged_zsets_loaded.insert(destination);
                dirty_zsets.insert(destination);
                result.success = true;
                result.value_present = true;
                result.int_value = static_cast<int64_t>(dest_values.size());
            } else if (op.op == TXN_OP_ZSET_ALGEBRA) {
                std::vector<std::string> args;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, args) || args.empty()) {
                    all_success = false;
                    continue;
                }
                int64_t numkeys_i64 = 0;
                if (!parse_int64(args[0], numkeys_i64) || numkeys_i64 <= 0) {
                    all_success = false;
                    continue;
                }
                const size_t numkeys = static_cast<size_t>(numkeys_i64);
                if (args.size() < 1 + numkeys * 2) {
                    all_success = false;
                    continue;
                }
                std::vector<std::string> sources(args.begin() + 1, args.begin() + 1 + numkeys);
                std::vector<double> weights;
                weights.reserve(numkeys);
                for (size_t i = 0; i < numkeys; ++i) {
                    double weight = 1.0;
                    if (!parse_zset_score_value(args[1 + numkeys + i], weight)) {
                        all_success = false;
                        break;
                    }
                    weights.push_back(weight);
                }
                if (!all_success) {
                    continue;
                }
                const bool is_union = (op.flags & TXN_FLAG_SET_ALGEBRA_UNION) != 0;
                const bool is_diff = (op.flags & TXN_FLAG_SET_ALGEBRA_DIFF) != 0;
                const bool is_store = (op.flags & TXN_FLAG_SET_ALGEBRA_STORE) != 0;
                const bool cardinality_only = (op.flags & TXN_FLAG_SCAN_COUNT_ONLY) != 0;
                std::vector<std::map<std::string, double>> source_values;
                source_values.reserve(numkeys);
                for (const auto& source_key : sources) {
                    TxnOpResult source_result{};
                    bool allowed = false;
                    mako::Status s = load_zset_stage(txn, source_key, source_result, allowed);
                    if (!s.ok()) {
                        all_success = false;
                        break;
                    }
                    if (allowed) {
                        source_values.push_back(staged_zsets[source_key]);
                        continue;
                    }
                    int64_t set_cardinality = 0;
                    s = read_set_cardinality(txn, source_key, set_cardinality);
                    if (!s.ok()) {
                        all_success = false;
                        break;
                    }
                    if (set_cardinality > 0) {
                        std::vector<std::string> members;
                        s = collect_set_members(txn, source_key, members);
                        if (!s.ok()) {
                            all_success = false;
                            break;
                        }
                        std::map<std::string, double> as_zset;
                        for (const auto& member : members) {
                            as_zset[member] = 1.0;
                        }
                        source_values.push_back(std::move(as_zset));
                    } else {
                        source_values.emplace_back();
                    }
                }
                if (!all_success) {
                    continue;
                }
                std::map<std::string, double> out;
                if (is_diff) {
                    if (!source_values.empty()) {
                        out = source_values[0];
                        for (size_t i = 1; i < source_values.size(); ++i) {
                            for (const auto& [member, score] : source_values[i]) {
                                out.erase(member);
                            }
                        }
                    }
                } else if (is_union) {
                    for (size_t i = 0; i < source_values.size(); ++i) {
                        for (const auto& [member, score] : source_values[i]) {
                            double weighted = score * weights[i];
                            if (std::isnan(weighted)) {
                                weighted = 0.0;
                            }
                            auto it = out.find(member);
                            if (it == out.end()) {
                                out[member] = weighted;
                            } else {
                                it->second = combine_zset_aggregate_score(it->second, weighted, op.expire_at_ms);
                            }
                        }
                    }
                } else {
                    if (!source_values.empty()) {
                        for (const auto& [member, score] : source_values[0]) {
                            double combined = score * weights[0];
                            if (std::isnan(combined)) {
                                combined = 0.0;
                            }
                            bool present_in_all = true;
                            for (size_t i = 1; i < source_values.size(); ++i) {
                                auto it = source_values[i].find(member);
                                if (it == source_values[i].end()) {
                                    present_in_all = false;
                                    break;
                                }
                                double weighted = it->second * weights[i];
                                if (std::isnan(weighted)) {
                                    weighted = 0.0;
                                }
                                combined = combine_zset_aggregate_score(combined, weighted, op.expire_at_ms);
                            }
                            if (present_in_all) {
                                out[member] = combined;
                            }
                        }
                    }
                }
                if (cardinality_only) {
                    result.success = true;
                    result.value_present = true;
                    int64_t count = static_cast<int64_t>(out.size());
                    if (op.expire_at_ms > 0 && op.expire_at_ms < count) {
                        count = op.expire_at_ms;
                    }
                    result.int_value = count;
                } else if (is_store) {
                    const std::string destination = user_key;
                    bool allowed = false;
                    TxnOpResult dest_allowed_result{};
                    mako::Status s = zset_key_allowed(txn, destination, dest_allowed_result, allowed);
                    if (!s.ok()) {
                        all_success = false;
                        continue;
                    }
                    if (!allowed) {
                        result.success = false;
                        continue;
                    }
                    staged_zsets[destination] = out;
                    staged_zsets_loaded.insert(destination);
                    dirty_zsets.insert(destination);
                    result.success = true;
                    result.value_present = true;
                    result.int_value = static_cast<int64_t>(out.size());
                } else {
                    std::vector<std::string> payload_items;
                    const bool with_scores = (op.flags & TXN_FLAG_Z_WITHSCORES) != 0;
                    for (const auto& [member, score] : zset_ordered_items(out)) {
                        payload_items.push_back(member);
                        if (with_scores) {
                            payload_items.push_back(format_zset_score(score));
                        }
                    }
                    result.success = true;
                    result.value_present = true;
                    std::string payload = pack_bytes_list(payload_items);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else if (op.op == TXN_OP_ZPOPMIN) {
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto items = zset_ordered_items(staged_zsets[user_key]);
                if ((op.flags & TXN_FLAG_Z_REV) != 0) {
                    std::reverse(items.begin(), items.end());
                }
                int64_t requested = (op.flags & TXN_FLAG_Z_COUNT_GIVEN) != 0 ? op.expire_at_ms : 1;
                requested = std::clamp<int64_t>(requested, 0, static_cast<int64_t>(items.size()));
                std::vector<std::string> payload_items;
                auto& values = staged_zsets[user_key];
                for (int64_t idx = 0; idx < requested; ++idx) {
                    const auto& [member, score] = items[static_cast<size_t>(idx)];
                    payload_items.push_back(member);
                    payload_items.push_back(format_zset_score(score));
                    values.erase(member);
                }
                if (requested > 0) {
                    dirty_zsets.insert(user_key);
                }
                result.success = true;
                result.value_present = true;
                std::string payload = pack_bytes_list(payload_items);
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_ZMPOP) {
                std::vector<std::string> keys;
                if (!unpack_bytes_list(op.val_ptr, op.val_len, keys)) {
                    all_success = false;
                    continue;
                }
                const bool reverse = (op.flags & TXN_FLAG_Z_REV) != 0;
                const int64_t requested_count = std::max<int64_t>(1, op.expire_at_ms);
                result.success = true;
                result.value_present = false;
                for (const auto& candidate_key : keys) {
                    TxnOpResult candidate_result{};
                    bool allowed = false;
                    mako::Status s = load_zset_stage(txn, candidate_key, candidate_result, allowed);
                    if (!s.ok()) {
                        all_success = false;
                        break;
                    }
                    if (!allowed) {
                        result.success = false;
                        result.value_present = false;
                        break;
                    }
                    auto items = zset_ordered_items(staged_zsets[candidate_key]);
                    if (items.empty()) {
                        continue;
                    }
                    if (reverse) {
                        std::reverse(items.begin(), items.end());
                    }
                    const int64_t count = std::clamp<int64_t>(
                        requested_count,
                        0,
                        static_cast<int64_t>(items.size()));
                    std::vector<std::string> payload_items;
                    payload_items.push_back(candidate_key);
                    auto& values = staged_zsets[candidate_key];
                    for (int64_t idx = 0; idx < count; ++idx) {
                        const auto& [member, score] = items[static_cast<size_t>(idx)];
                        payload_items.push_back(member);
                        payload_items.push_back(format_zset_score(score));
                        values.erase(member);
                    }
                    if (count > 0) {
                        dirty_zsets.insert(candidate_key);
                    }
                    std::string payload = pack_bytes_list(payload_items);
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                    result.success = true;
                    result.value_present = true;
                    break;
                }
                if (!all_success) {
                    continue;
                }
            } else if (op.op == TXN_OP_ZRANDMEMBER) {
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                auto items = zset_ordered_items(staged_zsets[user_key]);
                const bool with_scores = (op.flags & TXN_FLAG_Z_WITHSCORES) != 0;
                const bool count_given = (op.flags & TXN_FLAG_Z_COUNT_GIVEN) != 0;
                result.success = true;
                if (items.empty()) {
                    result.value_present = false;
                    continue;
                }
                int64_t requested = op.expire_at_ms;
                std::vector<std::string> payload_items;
                const uint64_t offset = g_mako_random_counter.fetch_add(1, std::memory_order_relaxed);
                if (!count_given) {
                    payload_items.push_back(items[static_cast<size_t>(offset % items.size())].first);
                } else if (requested == 0) {
                    // Empty array response.
                } else {
                    const bool allow_duplicates = requested < 0;
                    if (requested < 0) {
                        requested = -requested;
                    }
                    const int64_t limit = allow_duplicates
                        ? requested
                        : std::min<int64_t>(requested, static_cast<int64_t>(items.size()));
                    const size_t start = static_cast<size_t>(offset % items.size());
                    for (int64_t idx = 0; idx < limit; ++idx) {
                        const auto& item = items[(start + static_cast<size_t>(idx)) % items.size()];
                        payload_items.push_back(item.first);
                        if (with_scores) {
                            payload_items.push_back(format_zset_score(item.second));
                        }
                    }
                }
                result.value_present = true;
                std::string payload = pack_bytes_list(payload_items);
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_ZSCAN) {
                bool allowed = false;
                mako::Status s = load_zset_stage(txn, user_key, result, allowed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                if (!allowed) {
                    continue;
                }
                std::vector<std::string> payload_items;
                for (const auto& [member, score] : zset_ordered_items(staged_zsets[user_key])) {
                    payload_items.push_back(member);
                    payload_items.push_back(format_zset_score(score));
                }
                result.success = true;
                result.value_present = true;
                std::string payload = pack_bytes_list(payload_items);
                if (!copy_result_value(result, payload)) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_FLUSHDB) {
                // -1 for FLUSHALL, otherwise the one logical database FLUSHDB
                // was issued on; see redis_flush_db_filter.
                const int flush_db_filter = redis_flush_db_filter(op.val_ptr, op.val_len);
                std::vector<std::string> keys_to_delete;
                class FlushScanCallback : public oi_scan_callback {
                public:
                    FlushScanCallback(std::vector<std::string>& keys, int db_filter)
                        : keys_(keys), db_filter_(db_filter) {}

                    bool invoke(const char* keyp, size_t keylen, const std::string&) override {
                        if (db_filter_ >= 0
                            && redis_storage_key_db_index(keyp, keylen) != db_filter_) {
                            return true;
                        }
                        keys_.emplace_back(keyp, keylen);
                        return true;
                    }

                private:
                    std::vector<std::string>& keys_;
                    int db_filter_;
                };

                FlushScanCallback callback(keys_to_delete, flush_db_filter);
                tx_scan(g_table, txn, std::string(), nullptr, callback, tl_arena);
                mako::Status s = mako::Status::OK();
                for (const auto& storage_key : keys_to_delete) {
                    s = delete_raw_if_exists(txn, storage_key);
                    if (!s.ok() && !s.IsNotFound()) {
                        break;
                    }
                }
                if (s.ok() || s.IsNotFound()) {
                    // The scan only sees what is in storage, and a key this
                    // transaction created is still only in the write buffer,
                    // so drop the buffer as well: FLUSHDB empties the keyspace,
                    // including whatever this transaction was about to add.
                    // A database-scoped flush drops only the staged entries
                    // that belong to that database, so a MULTI that writes to
                    // two databases and flushes one keeps the other's writes.
                    auto drop_staged_user_keys = [&](auto& container) {
                        for (auto it = container.begin(); it != container.end();) {
                            const std::string& staged_key = redis_staged_entry_key(*it);
                            if (flush_db_filter < 0
                                || redis_user_key_db_index(staged_key.data(), staged_key.size())
                                    == flush_db_filter) {
                                it = container.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    };
                    for (auto it = pending_writes.begin(); it != pending_writes.end();) {
                        if (flush_db_filter < 0
                            || redis_storage_key_db_index(it->first.data(), it->first.size())
                                == flush_db_filter) {
                            it = pending_writes.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    drop_staged_user_keys(batch_ttls);
                    drop_staged_user_keys(staged_sets);
                    drop_staged_user_keys(staged_sets_loaded);
                    drop_staged_user_keys(dirty_sets);
                    drop_staged_user_keys(staged_lists);
                    drop_staged_user_keys(staged_lists_loaded);
                    drop_staged_user_keys(dirty_lists);
                    drop_staged_user_keys(staged_zsets);
                    drop_staged_user_keys(staged_zsets_loaded);
                    drop_staged_user_keys(dirty_zsets);
                    result.success = true;
                    result.value_present = true;
                } else {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SCAN) {
                const bool count_only = (op.flags & TXN_FLAG_SCAN_COUNT_ONLY) != 0;
                const size_t limit = count_only ? std::numeric_limits<size_t>::max()
                    : static_cast<size_t>(std::clamp<int64_t>(op.expire_at_ms, 1, 1000000));
                std::string cursor(reinterpret_cast<const char*>(op.key_ptr), op.key_len);
                std::string user_prefix;
                if (op.val_ptr != nullptr && op.val_len > 0) {
                    user_prefix.assign(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                }
                // A prefix that starts with the 0x02 marker names one logical
                // database, so its keys are what the scan is for; any other
                // prefix walks database 0 and skips them.
                const bool db_scoped =
                    !user_prefix.empty()
                    && static_cast<unsigned char>(user_prefix[0]) == kRedisDbMarker;

                const std::string storage_prefix = "table_key_" + user_prefix;
                std::string scan_start;
                if (!cursor.empty()) {
                    scan_start = "table_key_" + cursor + '\0';
                } else {
                    scan_start = storage_prefix;
                }
                std::optional<std::string> scan_end = storage_prefix_upper(storage_prefix);
                const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;
                std::vector<std::string> keys;
                std::vector<std::string> expired_user_keys;
                std::string last_seen_before_current;
                std::string next_cursor;
                bool has_more = false;
                int64_t visible_count = 0;

                class RedisScanCallback : public oi_scan_callback {
                public:
                    RedisScanCallback(
                        bool count_only,
                        bool db_scoped,
                        size_t limit,
                        std::vector<std::string>& keys,
                        std::vector<std::string>& expired_user_keys,
                        std::string& last_seen_before_current,
                        std::string& next_cursor,
                        bool& has_more,
                        int64_t& visible_count,
                        const std::function<bool(const std::string&)>& is_expired)
                        : count_only_(count_only),
                          db_scoped_(db_scoped),
                          limit_(limit),
                          keys_(keys),
                          expired_user_keys_(expired_user_keys),
                          last_seen_before_current_(last_seen_before_current),
                          next_cursor_(next_cursor),
                          has_more_(has_more),
                          visible_count_(visible_count),
                          is_expired_(is_expired) {}

                    bool invoke(const char* keyp, size_t keylen, const std::string&) override {
                        std::string storage_key(keyp, keylen);
                        constexpr std::string_view kStoragePrefix = "table_key_";
                        if (storage_key.rfind(kStoragePrefix, 0) != 0) {
                            return true;
                        }

                        std::string user_key = storage_key.substr(kStoragePrefix.size());
                        if (!user_key.empty() && static_cast<unsigned char>(user_key[0]) == 0x01) {
                            return true;
                        }
                        // Logical databases: a scan whose prefix does not name
                        // a database walks database 0, and database 0 never
                        // sees the 0x02 namespace the other databases live in.
                        if (!db_scoped_ && !user_key.empty()
                            && static_cast<unsigned char>(user_key[0]) == kRedisDbMarker) {
                            return true;
                        }
                        if (!count_only_ && keys_.size() >= limit_) {
                            has_more_ = true;
                            next_cursor_ = last_seen_before_current_;
                            return false;
                        }

                        const bool expired = is_expired_(user_key);
                        if (expired) {
                            expired_user_keys_.push_back(std::move(user_key));
                            return true;
                        }

                        if (count_only_) {
                            ++visible_count_;
                        } else {
                            keys_.push_back(user_key);
                        }
                        last_seen_before_current_ = user_key;
                        return true;
                    }

                private:
                    bool count_only_;
                    bool db_scoped_;
                    size_t limit_;
                    std::vector<std::string>& keys_;
                    std::vector<std::string>& expired_user_keys_;
                    std::string& last_seen_before_current_;
                    std::string& next_cursor_;
                    bool& has_more_;
                    int64_t& visible_count_;
                    const std::function<bool(const std::string&)>& is_expired_;
                };

                std::function<bool(const std::string&)> is_expired =
                    [&](const std::string& scanned_user_key) {
                        int64_t expire_at_ms = 0;
                        bool ttl_exists = false;
                        mako::Status ttl_status = read_ttl_meta(txn, scanned_user_key, expire_at_ms, ttl_exists);
                        if (!ttl_status.ok() || !ttl_exists) {
                            return false;
                        }
                        return expire_at_ms <= now_unix_ms();
                    };

                RedisScanCallback callback(
                    count_only,
                    db_scoped,
                    limit,
                    keys,
                    expired_user_keys,
                    last_seen_before_current,
                    next_cursor,
                    has_more,
                    visible_count,
                    is_expired);
                tx_scan(g_table, txn, scan_start, scan_end_ptr, callback, tl_arena);

                for (const auto& expired_user_key : expired_user_keys) {
                    std::string expired_storage_key = "table_key_" + expired_user_key;
                    mako::Status s = delete_raw_if_exists(txn, expired_storage_key);
                    if (s.ok()) {
                        s = clear_ttl_meta(txn, expired_user_key);
                    }
                    if (!s.ok()) {
                        all_success = false;
                        break;
                    }
                    batch_exists[expired_storage_key] = false;
                    batch_values.erase(expired_storage_key);
                }
                if (!all_success) {
                    continue;
                }

                result.success = true;
                result.value_present = true;
                if (count_only) {
                    result.int_value = visible_count;
                } else {
                    std::string payload;
                    append_u64_le(payload, has_more ? next_cursor.size() : 0);
                    if (has_more) {
                        payload.append(next_cursor);
                    }
                    append_u64_le(payload, keys.size());
                    for (const auto& key : keys) {
                        append_u64_le(payload, key.size());
                        payload.append(key);
                    }
                    if (!copy_result_value(result, payload)) {
                        all_success = false;
                    }
                }
            } else {
                // Unknown operation
                all_success = false;
            }
        }

        if (!finish) {
            return true;
        }

        // An interactive session has no TxnRequest left to walk at commit time,
        // so the Redis-visible names whose string value changed are read off
        // the write and delete buffers, which is exactly the string namespace
        // the cache holds.
        if (session.interactive && redis_cache_enabled()) {
            constexpr std::string_view kStoragePrefix = "table_key_";
            for (const auto& [storage_key, raw_value] : pending_writes) {
                if (storage_key.rfind(kStoragePrefix, 0) == 0) {
                    session.cache_invalidations.push_back(
                        storage_key.substr(kStoragePrefix.size()));
                }
            }
            for (const auto& storage_key : pending_deletes) {
                if (storage_key.rfind(kStoragePrefix, 0) == 0) {
                    session.cache_invalidations.push_back(
                        storage_key.substr(kStoragePrefix.size()));
                }
            }
        }

        if (all_success) {
            for (const auto& set_key : dirty_sets) {
                auto values_it = staged_sets.find(set_key);
                const std::unordered_set<std::string> empty_values;
                const auto& values = values_it == staged_sets.end() ? empty_values : values_it->second;
                mako::Status s = rewrite_set_values(txn, set_key, values);
                if (s.ok() && values.empty()) {
                    s = clear_ttl_meta(txn, set_key);
                }
                if (!s.ok()) {
                    all_success = false;
                    break;
                }
            }
        }
        if (all_success) {
            for (const auto& list_key : dirty_lists) {
                auto values_it = staged_lists.find(list_key);
                const std::vector<std::string> empty_values;
                const auto& values = values_it == staged_lists.end() ? empty_values : values_it->second;
                mako::Status s = rewrite_list_values(txn, list_key, values);
                if (s.ok() && values.empty()) {
                    s = clear_ttl_meta(txn, list_key);
                }
                if (!s.ok()) {
                    all_success = false;
                    break;
                }
            }
        }
        if (all_success) {
            for (const auto& zset_key : dirty_zsets) {
                auto values_it = staged_zsets.find(zset_key);
                const std::map<std::string, double> empty_values;
                const auto& values = values_it == staged_zsets.end() ? empty_values : values_it->second;
                mako::Status s = rewrite_zset_values(txn, zset_key, values);
                if (s.ok() && values.empty()) {
                    s = clear_ttl_meta(txn, zset_key);
                }
                if (!s.ok()) {
                    all_success = false;
                    break;
                }
            }
        }

        // Every op and every staged collection has had its say, so the buffer
        // now holds the final value of each key this transaction writes. One
        // tx_put per key, here and nowhere else, is what keeps a second write
        // of a key the transaction created from being dropped by install()
        // (see pending_writes).
        if (all_success) {
            mako::Status s = flush_pending_writes(txn);
            if (!s.ok()) {
                all_success = false;
            }
        }

        // What is still marked for removal is what was not written again.
        // Removing it here, and only here, is what keeps a delete and a later
        // write of the same key from colliding inside one STO transaction (see
        // pending_deletes). The two sets are disjoint, so the order of the two
        // flushes does not matter; writes go first so that a removal never
        // races a record this transaction has just created.
        if (all_success) {
            mako::Status s = flush_pending_deletes(txn);
            if (!s.ok()) {
                all_success = false;
            }
        }

        // Commit or rollback based on success
        if (all_success && commit) {
            g_mako_db->Commit(txn);
            if (session.has_write) {
                wait_for_redis_replication();
            }
            for (const auto& user_key : session.cache_invalidations) {
                redis_cache_invalidate(
                    reinterpret_cast<const uint8_t*>(user_key.data()), user_key.size());
            }
            response->transaction_success = true;
            g_mako_txn_commits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_mako_db->Rollback(txn);
            response->transaction_success = false;
            g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        }
        session.active = false;

    } catch (abstract_db::abstract_abort_exception& ex) {
        g_mako_db->Rollback(txn);
        session.active = false;
        session.threw = true;
        all_success = false;
        response->transaction_success = false;
        g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        // Mark all results as failed on abort
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    } catch (...) {
        g_mako_db->Rollback(txn);
        session.active = false;
        session.threw = true;
        all_success = false;
        response->transaction_success = false;
        g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    }

    return true;
}

// ===== Interactive transaction sessions =====
//
// cpp_txn_begin opens one, cpp_txn_execute runs an op list inside it as many
// times as the caller likes, and cpp_txn_commit / cpp_txn_abort end it. The
// point is read-your-writes across calls: everything the op loop buffers in the
// session (pending_writes, pending_deletes, the staged collections and the
// exists/value caches) is what the next call reads, so a Lua script's
// redis.call('get', k) after a redis.call('set', k, v) sees v without either
// reaching storage.

static RedisTxnSession* session_from_handle(void* handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    RedisTxnSession* session = static_cast<RedisTxnSession*>(handle);
    if (session->magic != RedisTxnSession::kSessionMagic) {
        std::cerr << "[cpp] interactive transaction: bad session handle" << std::endl;
        return nullptr;
    }
    return session;
}

static void destroy_session(RedisTxnSession* session) {
    session->magic = 0;
    delete session;
}

void* txn_begin(const uint8_t* const* keys, const size_t* key_lens, size_t num_keys) {
    ensure_thread_info();
    if (g_mako_db == nullptr) {
        return nullptr;
    }

    std::unique_ptr<RedisTxnSession> session(new RedisTxnSession());
    session->interactive = true;

    if (!g_redis_single_worker_mode) {
        session->keyspace_shared_lock = std::shared_lock<std::shared_mutex>(g_redis_keyspace_mutex);
        std::vector<size_t> stripes;
        stripes.reserve(num_keys);
        for (size_t i = 0; keys != nullptr && key_lens != nullptr && i < num_keys; ++i) {
            stripes.push_back(redis_lock_hash(keys[i], key_lens[i]) % kRedisTxnLockStripes);
        }
        // Stripe order, as the batch path takes them, so the two cannot deadlock
        // against each other.
        std::sort(stripes.begin(), stripes.end());
        stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());
        session->key_locks.reserve(stripes.size());
        for (size_t stripe : stripes) {
            session->key_locks.emplace_back(g_redis_txn_key_mutexes[stripe]);
        }
    }

    begin_session_txn(*session);
    return session.release();
}

bool txn_execute(void* handle, const TxnRequest* request, TxnResponse* response) {
    RedisTxnSession* session = session_from_handle(handle);
    if (session == nullptr || !session->active) {
        return false;
    }
    if (!makocon_ffi::allocate_response(request, response)) {
        return false;
    }

    // Lazy, per-call: a session that only reads is legal on a follower, and the
    // first write in one is not.
    if (redis_request_has_write(request)) {
        if (!redis_can_write_here()) {
            response->transaction_success = false;
            for (size_t i = 0; i < response->num_results; ++i) {
                response->results[i].success = false;
            }
            session->all_success = false;
            return false;
        }
        session->has_write = true;
    }

    const bool ok = execute_ops(*session, request, response);
    response->transaction_success = ok && session->active && session->all_success;
    return response->transaction_success;
}

bool txn_commit(void* handle) {
    RedisTxnSession* session = session_from_handle(handle);
    if (session == nullptr) {
        return false;
    }
    const bool committed = finish_session(*session, true);
    destroy_session(session);
    return committed;
}

void txn_abort(void* handle) {
    RedisTxnSession* session = session_from_handle(handle);
    if (session == nullptr) {
        return;
    }
    finish_session(*session, false);
    destroy_session(session);
}

// Free transaction response resources
void free_transaction_response(TxnResponse* response) {
    makocon_ffi::free_transaction_response(response);
}

// FFI exports - called by Rust
extern "C" {
    // Called by Rust when each worker thread starts
    void cpp_worker_thread_init(size_t thread_id) {
        ensure_thread_info();
        tl_redis_worker_id = thread_id;
        std::cout << "[cpp] Worker thread " << thread_id << " initialized" << std::endl;
    }

    // Cleanup thread-local state
    void cpp_cleanup_thread_info() {
        cleanup_thread_info();
    }

    // Specialized allocation-light path for plain GET and unconditional SET.
    bool cpp_execute_fast_mako_string(
        uint32_t op,
        const uint8_t* key_ptr,
        size_t key_len,
        const uint8_t* val_ptr,
        size_t val_len,
        FastMakoStringResult* result) {
        return execute_fast_mako_string(
            op, key_ptr, key_len, val_ptr, val_len, result);
    }

    // Generic command/fallback and MULTI/EXEC entry point.
    bool cpp_execute_transaction(const TxnRequest* request, TxnResponse* response) {
        return execute_transaction(request, response);
    }

    // Interactive transaction sessions (Lua scripting).
    void* cpp_txn_begin(const uint8_t* const* keys, const size_t* key_lens, size_t num_keys) {
        return txn_begin(keys, key_lens, num_keys);
    }

    bool cpp_txn_execute(void* session, const TxnRequest* request, TxnResponse* response) {
        return txn_execute(session, request, response);
    }

    bool cpp_txn_commit(void* session) {
        return txn_commit(session);
    }

    void cpp_txn_abort(void* session) {
        txn_abort(session);
    }

    // Free transaction response resources
    void cpp_free_transaction_response(TxnResponse* response) {
        free_transaction_response(response);
    }

    bool cpp_get_metrics(MakoMetrics* metrics) {
        const bool ok = makocon_ffi::populate_metrics(
            metrics, g_mako_start_time, g_mako_txn_commits, g_mako_txn_aborts,
            g_mako_txn_retries);
        if (ok) {
            append_fast_metrics(metrics);
            metrics->cache_enabled = redis_cache_enabled() ? 1 : 0;
            metrics->cache_capacity_bytes = g_redis_cache_capacity_bytes;
            metrics->cache_hits = g_redis_cache_hits.load(std::memory_order_relaxed);
            metrics->cache_misses = g_redis_cache_misses.load(std::memory_order_relaxed);
            metrics->cache_inserts = g_redis_cache_inserts.load(std::memory_order_relaxed);
            metrics->cache_evictions = g_redis_cache_evictions.load(std::memory_order_relaxed);
            metrics->cache_invalidations = g_redis_cache_invalidations.load(std::memory_order_relaxed);
            redis_cache_usage(metrics->cache_entries, metrics->cache_bytes);
        }
        return ok;
    }

    void cpp_record_txn_retry(void) {
        makocon_ffi::record_txn_retry(g_mako_txn_retries);
    }
}

int main() {
    std::cout << "=== makoCon: Redis-compatible server with mako::DB ===" << std::endl;
    setenv("MAKO_REDIS_SERVER", "1", 1);

    // Configuration parameters. Defaults remain one local shard with no replication.
    int nshards = 1;
    int shard_index = 0;
    int nthreads = 32;
    int bench_value_size = 8;
    int cache_mb = 0;
    if (!parse_env_int("MAKO_REDIS_THREADS", 32, 1, 32, nthreads) ||
        !parse_env_int("MAKO_NUM_SHARDS", 1, 1, 10, nshards) ||
        !parse_env_int("MAKO_SHARD_INDEX", 0, 0, nshards - 1, shard_index) ||
        !parse_env_int("MAKO_REDIS_BENCH_VALUE_SIZE", 8, 0, 1048576, bench_value_size) ||
        !parse_env_int("MAKO_REDIS_CACHE_MB", 0, 0, 65536, cache_mb)) {
        return 1;
    }
    g_redis_single_worker_mode = nthreads == 1;
    g_redis_bench_skip_ttl = env_enabled("MAKO_REDIS_BENCH_SKIP_TTL");
    g_redis_bench_no_db = env_enabled("MAKO_REDIS_BENCH_NO_DB");
    g_redis_bench_skip_locks = env_enabled("MAKO_REDIS_BENCH_SKIP_LOCKS");
    g_redis_bench_skip_mutex = env_enabled("MAKO_REDIS_BENCH_SKIP_MUTEX");
    g_redis_bench_value_size = static_cast<size_t>(bench_value_size);
    g_redis_cache_capacity_bytes = static_cast<size_t>(cache_mb) * 1024 * 1024;
    if (redis_cache_enabled()) {
        std::cout << "Redis string cache enabled: capacity=" << cache_mb
                  << " MiB; coherence=Redis-mediated writes only; TTL values bypass cache"
                  << std::endl;
    }
    if (g_redis_bench_skip_ttl || g_redis_bench_no_db
        || g_redis_bench_skip_locks || g_redis_bench_skip_mutex) {
        std::cerr << "WARNING: benchmark-only Redis semantics ablation enabled:"
                  << " skip_ttl=" << g_redis_bench_skip_ttl
                  << " no_db=" << g_redis_bench_no_db
                  << " skip_locks=" << g_redis_bench_skip_locks
                  << " skip_mutex=" << g_redis_bench_skip_mutex << std::endl;
    }
    std::vector<int> local_shards;
    if (!parse_local_shards(getenv("MAKO_LOCAL_SHARDS"), nshards, local_shards)) {
        return 1;
    }

    const char* mako_paxos_proc_name = getenv("MAKO_PAXOS_PROC_NAME");
    std::string paxos_proc_name =
            mako_paxos_proc_name ? mako_paxos_proc_name : "localhost";  // Leader by default
    bool replication_enabled = env_enabled("MAKO_REPLICATION_ENABLED");
    bool is_leader = paxos_proc_name == "localhost";
    g_redis_replication_enabled = replication_enabled;
    if (replication_enabled) {
        // Redis replies are per-command durability promises. A larger Mako
        // batch could leave acknowledged commands only in process memory.
        setenv("MAKO_BATCH_SIZE", "1", 1);
    }

    // Build config path (same pattern as simpleTransactionRep.cc)
    std::string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards" + std::to_string(nshards)
            + "-warehouses" + std::to_string(nthreads) + ".yml";
    if (const char* mako_config = getenv("MAKO_SHARD_CONFIG")) {
        if (mako_config[0] != '\0') {
            config_path = mako_config;
        }
    }
    // @unsafe { std::ifstream probes the filesystem for fixture configuration validation. }
    std::ifstream config_probe(config_path);
    if (!config_probe.good()) {
        std::cerr << "Shard config not found: " << config_path << std::endl;
        return 1;
    }

    // Create transport configuration
    auto transport_config = new transport::Configuration(config_path);
    if (!local_shards.empty()) {
        transport_config->local_shard_indices = local_shards;
        transport_config->multi_shard_mode = local_shards.size() > 1;
    }

    // Configure mako::Options (following simpleTransactionRep.cc pattern)
    mako::Options options;
    options.num_shards = nshards;
    options.shard_index = shard_index;
    options.num_threads = nthreads;
    options.paxos_proc_name = paxos_proc_name;
    options.replication.enabled = replication_enabled;
    options.replication.is_leader = is_leader;
    options.transport_config = transport_config;
    if (replication_enabled) {
        if (const char* replication_config = getenv("MAKO_REPLICATION_CONFIG")) {
            if (replication_config[0] != '\0') {
                options.paxos_config_files.push_back(replication_config);
            }
        }
        if (const char* occ_config = getenv("MAKO_OCC_CONFIG")) {
            if (occ_config[0] != '\0') {
                options.paxos_config_files.push_back(occ_config);
            }
        }
    }
    std::cout << "makoCon config: shards=" << nshards
              << " shard_index=" << shard_index
              << " threads=" << nthreads
              << " local_shards=";
    if (local_shards.empty()) {
        std::cout << "<default>";
    } else {
        for (size_t i = 0; i < local_shards.size(); ++i) {
            if (i != 0) {
                std::cout << ",";
            }
            std::cout << local_shards[i];
        }
    }
    std::cout << " replication=" << (replication_enabled ? "enabled" : "disabled")
              << " paxos_proc=" << paxos_proc_name << std::endl;

    // Open the database using mako::DB interface
    // mako::DB::Open() internally configures BenchmarkConfig
    mako::Status status = mako::DB::Open(options, "/tmp/mako_redis", &g_mako_db);
    if (!status.ok()) {
        std::cerr << "Failed to open database: " << status.ToString() << std::endl;
        return 1;
    }
    std::cout << "Database opened successfully" << std::endl;

    // Open the table for key-value operations
    g_table = g_mako_db->GetDB()->open_sharded_index("customer_0");
    if (!g_table) {
        std::cerr << "Failed to open table" << std::endl;
        delete g_mako_db;
        return 1;
    }
    std::cout << "Table 'customer_0' opened" << std::endl;

    // Initialize Rust server (spawns N workers sharing one round-robin listener)
    // Each worker thread will call cpp_worker_thread_init() to initialize
    // its thread-local state via mako_db_->InitThread()
    if (!rust_init(nthreads)) {
        std::cerr << "Failed to initialize Rust server" << std::endl;
        delete g_mako_db;
        return 1;
    }
    std::cout << "Rust server initialized with " << nthreads << " worker threads" << std::endl;

    const char* mako_host = getenv("MAKO_HOST");
    const char* mako_port = getenv("MAKO_PORT");
    std::cout << "\n=== Server running on "
              << (mako_host ? mako_host : "127.0.0.1")
              << ":"
              << (mako_port ? mako_port : "6380")
              << " ===" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    // Main thread just waits
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup (unreachable in practice)
    delete g_mako_db;
    return 0;
}
