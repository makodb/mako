//
// makoCon.cc - Redis-compatible server using Mako database (mako::DB interface)
//
// This file implements a simple Redis-compatible key-value server using:
// - mako::DB interface for database operations
// - Rust library for Redis protocol handling (shared listener, thread-per-core)
// - Nonblocking connection I/O with synchronous Mako transactions
// - Transaction support via MULTI/EXEC
//
// All operations (single GET/SET or batched MULTI/EXEC) go through execute_transaction()
// which wraps them in a single database transaction.
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
#include "db.hh"
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
static std::atomic<uint64_t> g_mako_random_counter{1};
// Redis-facing correctness is claimed through makoCon. Conflicting write
// transactions are serialized by Redis key stripe instead of using one global
// executor lock, so unrelated keys can still make progress in parallel.
static std::shared_mutex g_redis_keyspace_mutex;
static constexpr size_t kRedisTxnLockStripes = 256;
static std::array<std::mutex, kRedisTxnLockStripes> g_redis_txn_key_mutexes;
static bool g_redis_single_worker_mode = false;
static bool g_redis_replication_enabled = false;

// Thread-local state for transaction handling
thread_local str_arena* tl_arena = nullptr;
thread_local std::string tl_txn_buf;
thread_local std::string tl_key_buf;
thread_local std::string tl_val_buf;
thread_local bool tl_initialized = false;

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

static uint64_t redis_lock_hash(const uint8_t* data, size_t len) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
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
        case TXN_OP_GETBIT:
        case TXN_OP_GETRANGE:
        case TXN_OP_DUMP:
        case TXN_OP_ZRANGEBYLEX:
        case TXN_OP_ZLEXCOUNT:
        case TXN_OP_ZRANDMEMBER:
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
        case TXN_OP_SMOVE:
        case TXN_OP_LMOVE:
        case TXN_OP_SORT:
        case TXN_OP_ZRANGESTORE:
        case TXN_OP_SET_ALGEBRA:
        case TXN_OP_ZSET_ALGEBRA:
        case TXN_OP_BPOP:
        case TXN_OP_ZMPOP:
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
    tl_val_buf.clear();
    tl_initialized = false;
}

static bool execute_flushdb_chunked(size_t chunk_size = 1024) {
    ensure_thread_info();
    if (g_mako_db == nullptr || g_table == nullptr) {
        return false;
    }

    class FlushChunkScanCallback : public abstract_ordered_index::scan_callback {
    public:
        FlushChunkScanCallback(std::vector<std::string>& keys, size_t limit)
            : keys_(keys), limit_(limit) {}

        bool invoke(const char* keyp, size_t keylen, const std::string&) override {
            keys_.emplace_back(keyp, keylen);
            return keys_.size() < limit_;
        }

    private:
        std::vector<std::string>& keys_;
        size_t limit_;
    };

    for (;;) {
        if (tl_arena) {
            tl_arena->reset();
        }

        std::vector<std::string> keys_to_delete;
        keys_to_delete.reserve(chunk_size);
        void* txn = g_mako_db->BeginTransaction();

        try {
            FlushChunkScanCallback callback(keys_to_delete, chunk_size);
            g_table->scan(txn, std::string(), nullptr, callback, tl_arena);

            if (keys_to_delete.empty()) {
                g_mako_db->Rollback(txn);
                return true;
            }

            for (const auto& storage_key : keys_to_delete) {
                mako::Status s = g_table->Delete(txn, storage_key);
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

// Execute a batch of operations as a single database transaction
// This is the ONLY entry point for all database operations (single or batched)
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

    std::unique_lock<std::shared_mutex> redis_keyspace_exclusive_lock;
    std::shared_lock<std::shared_mutex> redis_keyspace_shared_lock;
    std::unique_lock<std::mutex> redis_single_key_lock;
    std::vector<std::unique_lock<std::mutex>> redis_key_locks;
    if (g_redis_single_worker_mode) {
        // One Rust protocol worker already serializes all FFI executor calls.
    } else if (redis_request_has_flushdb(request)) {
        redis_keyspace_exclusive_lock = std::unique_lock<std::shared_mutex>(g_redis_keyspace_mutex);
    } else {
        redis_keyspace_shared_lock = std::shared_lock<std::shared_mutex>(g_redis_keyspace_mutex);
        if (request != nullptr
            && request->ops != nullptr
            && request->num_ops == 1
            && !redis_op_is_read_only(request->ops[0])
            && redis_op_uses_only_primary_lock_key(request->ops[0])) {
            const TxnOperation& op = request->ops[0];
            const size_t stripe = redis_lock_hash(op.key_ptr, op.key_len) % kRedisTxnLockStripes;
            redis_single_key_lock = std::unique_lock<std::mutex>(g_redis_txn_key_mutexes[stripe]);
        } else if (has_write) {
            const std::vector<size_t> stripes = redis_request_lock_stripes(request);
            redis_key_locks.reserve(stripes.size());
            for (size_t stripe : stripes) {
                redis_key_locks.emplace_back(g_redis_txn_key_mutexes[stripe]);
            }
        }
    }

    if (request->num_ops == 1 && request->ops != nullptr && request->ops[0].op == TXN_OP_FLUSHDB) {
        const bool ok = execute_flushdb_chunked();
        response->transaction_success = ok;
        response->results[0].success = ok;
        response->results[0].value_present = ok;
        if (ok) {
            g_mako_txn_commits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    // Reset arena for this transaction
    if (tl_arena) {
        tl_arena->reset();
    }

    // StringWrapper stores a pointer to the string, so encoded values must
    // outlive Commit(). deque keeps references stable as we append values.
    std::deque<std::string> owned_encoded_vals;

    // Begin a single database transaction for all operations
    // NOTE: mbta_wrapper::new_txn() always returns NULL - it uses thread-local TThread::txn state
    // The actual transaction is started via Sto::start_transaction() internally
    // DO NOT check for NULL - that's expected behavior!
    void* txn = g_mako_db->BeginTransaction();

    bool all_success = true;
    std::unordered_map<std::string, bool> batch_exists;
    std::unordered_map<std::string, std::string> batch_values;
    std::unordered_map<std::string, int64_t> batch_ttls;
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

    auto read_raw = [&](void* txn, const std::string& key, std::string& value, bool& exists) {
        value.clear();
        mako::Status s = g_table->Get(txn, key, value);
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

    auto delete_raw_if_exists = [&](void* txn, const std::string& key) {
        if (!key.empty() && static_cast<unsigned char>(key[0]) == 0x01) {
            mako::Status s = g_table->Delete(txn, key);
            return s.IsNotFound() ? mako::Status::OK() : s;
        }
        auto batch_it = batch_exists.find(key);
        if (batch_it != batch_exists.end()) {
            if (!batch_it->second) {
                return mako::Status::OK();
            }
            return g_table->Delete(txn, key);
        }
        bool exists = false;
        mako::Status s = read_raw_exists(txn, key, exists);
        if (!s.ok() || !exists) {
            return s;
        }
        return g_table->Delete(txn, key);
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

    auto put_raw = [&](void* txn, const std::string& key, const std::string& raw_value) {
        owned_encoded_vals.push_back(mako::Encode(raw_value));
        return g_table->Put(txn, key, owned_encoded_vals.back());
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

    if (request->num_ops == 1 && request->ops != nullptr
        && request->ops[0].op == TXN_OP_SCAN
        && (request->ops[0].flags & TXN_FLAG_SCAN_COUNT_ONLY) != 0
        && request->ops[0].key_len == 0
        && request->ops[0].val_len == 0) {
        g_mako_db->Rollback(txn);

        constexpr size_t kDbSizeChunkSize = 1024;
        int64_t visible_count = 0;
        bool ok = true;
        std::string scan_start = "table_key_";
        const std::string storage_prefix = "table_key_";
        std::optional<std::string> scan_end = storage_prefix_upper(storage_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class DbSizeChunkScanCallback : public abstract_ordered_index::scan_callback {
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
                if (!user_key.empty() && static_cast<unsigned char>(user_key[0]) == 0x01) {
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
                g_table->scan(scan_txn, scan_start, scan_end_ptr, callback, tl_arena);

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
                    g_mako_db->Rollback(scan_txn);
                    break;
                }

                if (expired_user_keys.empty()) {
                    g_mako_db->Rollback(scan_txn);
                } else {
                    g_mako_db->Commit(scan_txn);
                    wait_for_redis_replication();
                }

                if (callback.seen() < kDbSizeChunkSize || last_storage_key.empty()) {
                    break;
                }
                scan_start = last_storage_key;
                scan_start.push_back('\0');
            } catch (abstract_db::abstract_abort_exception&) {
                g_mako_db->Rollback(scan_txn);
                ok = false;
                break;
            } catch (...) {
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

        class ZSetMemberScanCallback : public abstract_ordered_index::scan_callback {
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
        g_table->scan(txn, member_prefix, scan_end_ptr, callback, tl_arena);

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

    auto collect_set_members = [&](void* txn, const std::string& set_key, std::vector<std::string>& members) {
        members.clear();
        const std::string user_prefix = make_set_member_prefix(set_key);
        const std::string storage_prefix = user_prefix;
        std::optional<std::string> scan_end = storage_prefix_upper(storage_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class SetScanCallback : public abstract_ordered_index::scan_callback {
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
        g_table->scan(txn, storage_prefix, scan_end_ptr, callback, tl_arena);
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

    auto collect_hash_entries = [&](void* txn, const std::string& hash_key, std::map<std::string, std::string>& entries) {
        entries.clear();
        const std::string field_prefix = make_hash_field_prefix(hash_key);
        std::optional<std::string> scan_end = storage_prefix_upper(field_prefix);
        const std::string* scan_end_ptr = scan_end ? &*scan_end : nullptr;

        class HashScanCallback : public abstract_ordered_index::scan_callback {
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
        g_table->scan(txn, field_prefix, scan_end_ptr, callback, tl_arena);
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
        return write_hash_cardinality(txn, hash_key, 0);
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
        s = read_hash_cardinality(txn, user_key, hash_count);
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
        exists = zset_count > 0;
        return s;
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
        allowed = !string_exists && list_length == 0 && zset_count == 0 && hash_count == 0;
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
        allowed = !string_exists && set_cardinality == 0 && zset_count == 0 && hash_count == 0;
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
        allowed = !string_exists && set_cardinality == 0 && list_length == 0 && hash_count == 0;
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
        allowed = !string_exists && set_cardinality == 0 && list_length == 0 && zset_count == 0;
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
        allowed = set_cardinality == 0 && list_length == 0 && zset_count == 0 && hash_count == 0;
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
                // @unsafe { g_table->Delete calls non-borrow-checked Masstree code }
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
                if (s.ok() && string_exists) {
                    s = g_table->Delete(txn, tl_key_buf);
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
                std::string current;
                bool exists = false;
                mako::Status s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                int64_t base = 0;
                int64_t delta = 0;
                std::string delta_str(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                if ((exists && !parse_int64(current, base)) || !parse_int64(delta_str, delta)) {
                    all_success = false;
                    continue;
                }
                int64_t next = 0;
                if (!add_int64(base, delta, next)) {
                    all_success = false;
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
                std::string current;
                bool exists = false;
                mako::Status s = read_current(txn, user_key, tl_key_buf, current, exists);
                if (!s.ok()) {
                    all_success = false;
                    continue;
                }
                long double base = 0;
                long double delta = 0;
                std::string delta_str(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                if ((exists && !parse_float(current, base)) || !parse_float(delta_str, delta)) {
                    all_success = false;
                    continue;
                }
                long double next = base + delta;
                if (!std::isfinite(next)) {
                    all_success = false;
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
                s = read_hash_cardinality(txn, user_key, hash_count);
                result.success = s.ok();
                result.value_present = true;
                result.int_value = string_exists ? 1
                    : (set_count > 0 ? 2
                    : (list_length > 0 ? 3
                    : (zset_count > 0 ? 4
                    : (hash_count > 0 ? 5 : 0))));
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
                if (!string_exists && set_cardinality == 0 && list_length == 0
                    && zset_count == 0 && hash_count == 0) {
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
                    if (!ds.ok()) {
                        result.success = false;
                        all_success = false;
                        continue;
                    }
                    if (destination_string_exists || destination_set_cardinality > 0
                        || destination_list_length > 0 || destination_zset_count > 0
                        || destination_hash_count > 0) {
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
                } else {
                    s = collect_hash_entries(txn, user_key, hash_entries);
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
                    for (const auto& [member, score] : zset_values) {
                        std::string member_key = zset_member_storage_key(destination, member);
                        std::string score_payload = std::to_string(score);
                        s = put_raw(txn, member_key, score_payload);
                        if (!s.ok()) {
                            break;
                        }
                        batch_exists[member_key] = true;
                        batch_values[member_key] = score_payload;
                        std::string score_key = zset_score_storage_key(destination, score, member);
                        s = put_raw(txn, score_key, "1");
                        if (!s.ok()) {
                            break;
                        }
                        batch_exists[score_key] = true;
                        batch_values[score_key] = "1";
                    }
                    if (s.ok()) {
                        s = write_zset_cardinality(txn, destination, static_cast<int64_t>(zset_values.size()));
                    }
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
            } else if (op.op == TXN_OP_COPY) {
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
                if (!string_exists && set_cardinality == 0 && list_length == 0
                    && zset_count == 0 && hash_count == 0) {
                    result.success = true;
                    result.value_present = true;
                    result.int_value = 0;
                    continue;
                }
                const bool replace = op.expire_at_ms == 1;
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
                if (!ds.ok()) {
                    result.success = false;
                    all_success = false;
                    continue;
                }
                const bool destination_exists = destination_string_exists
                    || destination_set_cardinality > 0 || destination_list_length > 0
                    || destination_zset_count > 0 || destination_hash_count > 0;
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
                } else {
                    s = collect_hash_entries(txn, user_key, hash_entries);
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
                    if (s.ok()) {
                        s = write_hash_cardinality(txn, destination, static_cast<int64_t>(hash_entries.size()));
                    }
                }
                if (s.ok() && source_ttl_exists) {
                    s = write_ttl_meta(txn, destination, source_expire_at_ms);
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
                    s = read_hash_cardinality(txn, user_key, hash_count);
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
                    if ((op.flags & TXN_FLAG_SET_NX) != 0 && exists) {
                        continue;
                    }
                    s = put_raw(txn, field_key, value);
                    if (!s.ok()) {
                        break;
                    }
                    batch_exists[field_key] = true;
                    batch_values[field_key] = value;
                    if (!exists) {
                        ++added;
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
                s = read_hash_cardinality(txn, user_key, cardinality);
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
                std::string next_str;
                if (op.op == TXN_OP_HINCRBY) {
                    int64_t base = 0;
                    int64_t delta = 0;
                    int64_t next = 0;
                    if ((exists && !parse_int64(current, base)) || !parse_int64(parts[1], delta)) {
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
                    if ((exists && !parse_float(current, base)) || !parse_float(parts[1], delta)) {
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
                    if (!exists) {
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
                        s = g_table->Delete(txn, tl_key_buf);
                    }
                    if (s.ok() && string_exists) {
                        batch_exists[tl_key_buf] = false;
                        batch_values.erase(tl_key_buf);
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
                std::vector<std::string> keys_to_delete;
                class FlushScanCallback : public abstract_ordered_index::scan_callback {
                public:
                    explicit FlushScanCallback(std::vector<std::string>& keys)
                        : keys_(keys) {}

                    bool invoke(const char* keyp, size_t keylen, const std::string&) override {
                        keys_.emplace_back(keyp, keylen);
                        return true;
                    }

                private:
                    std::vector<std::string>& keys_;
                };

                FlushScanCallback callback(keys_to_delete);
                g_table->scan(txn, std::string(), nullptr, callback, tl_arena);
                mako::Status s = mako::Status::OK();
                for (const auto& storage_key : keys_to_delete) {
                    s = g_table->Delete(txn, storage_key);
                    if (!s.ok() && !s.IsNotFound()) {
                        break;
                    }
                    batch_exists[storage_key] = false;
                    batch_values.erase(storage_key);
                }
                if (s.ok() || s.IsNotFound()) {
                    batch_ttls.clear();
                    staged_sets.clear();
                    staged_sets_loaded.clear();
                    dirty_sets.clear();
                    staged_lists.clear();
                    staged_lists_loaded.clear();
                    dirty_lists.clear();
                    staged_zsets.clear();
                    staged_zsets_loaded.clear();
                    dirty_zsets.clear();
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

                class RedisScanCallback : public abstract_ordered_index::scan_callback {
                public:
                    RedisScanCallback(
                        bool count_only,
                        size_t limit,
                        std::vector<std::string>& keys,
                        std::vector<std::string>& expired_user_keys,
                        std::string& last_seen_before_current,
                        std::string& next_cursor,
                        bool& has_more,
                        int64_t& visible_count,
                        const std::function<bool(const std::string&)>& is_expired)
                        : count_only_(count_only),
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
                    limit,
                    keys,
                    expired_user_keys,
                    last_seen_before_current,
                    next_cursor,
                    has_more,
                    visible_count,
                    is_expired);
                g_table->scan(txn, scan_start, scan_end_ptr, callback, tl_arena);

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

        // Commit or rollback based on success
        if (all_success) {
            g_mako_db->Commit(txn);
            if (has_write) {
                wait_for_redis_replication();
            }
            response->transaction_success = true;
            g_mako_txn_commits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_mako_db->Rollback(txn);
            response->transaction_success = false;
            g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        }

    } catch (abstract_db::abstract_abort_exception& ex) {
        g_mako_db->Rollback(txn);
        response->transaction_success = false;
        g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        // Mark all results as failed on abort
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    } catch (...) {
        g_mako_db->Rollback(txn);
        response->transaction_success = false;
        g_mako_txn_aborts.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    }

    return true;
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
        std::cout << "[cpp] Worker thread " << thread_id << " initialized" << std::endl;
    }

    // Cleanup thread-local state
    void cpp_cleanup_thread_info() {
        cleanup_thread_info();
    }

    // Execute a batch of operations as a single database transaction
    // This is used for both single operations (GET/SET) and batched MULTI/EXEC
    bool cpp_execute_transaction(const TxnRequest* request, TxnResponse* response) {
        return execute_transaction(request, response);
    }

    // Free transaction response resources
    void cpp_free_transaction_response(TxnResponse* response) {
        free_transaction_response(response);
    }

    bool cpp_get_metrics(MakoMetrics* metrics) {
        return makocon_ffi::populate_metrics(
            metrics, g_mako_start_time, g_mako_txn_commits, g_mako_txn_aborts,
            g_mako_txn_retries);
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
    if (!parse_env_int("MAKO_REDIS_THREADS", 32, 1, 32, nthreads) ||
        !parse_env_int("MAKO_NUM_SHARDS", 1, 1, 10, nshards) ||
        !parse_env_int("MAKO_SHARD_INDEX", 0, 0, nshards - 1, shard_index)) {
        return 1;
    }
    g_redis_single_worker_mode = nthreads == 1;
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
