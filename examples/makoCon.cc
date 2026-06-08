//
// makoCon.cc - Redis-compatible server using Mako database (mako::DB interface)
//
// This file implements a simple Redis-compatible key-value server using:
// - mako::DB interface for database operations
// - Rust library for Redis protocol handling (SO_REUSEPORT thread-per-core)
// - 100% synchronous blocking I/O
// - Transaction support via MULTI/EXEC
//
// All operations (single GET/SET or batched MULTI/EXEC) go through execute_transaction()
// which wraps them in a single database transaction.
//

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include <algorithm>
#include <mako.hh>
#include "db.hh"
#include <examples/common.h>
#include "lib/transaction_ffi.h"
#include "silo_runtime.h"

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

// Thread-local state for transaction handling
thread_local str_arena* tl_arena = nullptr;
thread_local std::string tl_txn_buf;
thread_local std::string tl_key_buf;
thread_local std::string tl_val_buf;
thread_local bool tl_initialized = false;

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

// Execute a batch of operations as a single database transaction
// This is the ONLY entry point for all database operations (single or batched)
bool execute_transaction(const TxnRequest* request, TxnResponse* response) {
    ensure_thread_info();

    if (!request || !response || request->num_ops == 0) {
        if (response) {
            response->transaction_success = false;
            response->num_results = 0;
            response->results = nullptr;
        }
        return false;
    }

    // Allocate results array
    response->num_results = request->num_ops;
    response->results = static_cast<TxnOpResult*>(
        std::calloc(request->num_ops, sizeof(TxnOpResult)));
    if (!response->results) {
        response->transaction_success = false;
        response->num_results = 0;
        return false;
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

    auto parse_int64 = [](const std::string& input, int64_t& out) {
        if (input.empty()) {
            return false;
        }
        size_t pos = 0;
        try {
            long long parsed = std::stoll(input, &pos, 10);
            if (pos != input.size()) {
                return false;
            }
            out = static_cast<int64_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
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

    auto delete_raw_if_exists = [&](void* txn, const std::string& key) {
        bool exists = false;
        mako::Status s = g_table->Exists(txn, key, &exists);
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
        return g_table->Exists(txn, storage_key, &exists);
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
        if (input.empty()) {
            return false;
        }
        size_t pos = 0;
        try {
            long double parsed = std::stold(input, &pos);
            if (pos != input.size() || !std::isfinite(parsed)) {
                return false;
            }
            out = parsed;
            return true;
        } catch (...) {
            return false;
        }
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
                bool existed = false;
                mako::Status s = read_current(txn, user_key, tl_key_buf, old_value, existed);
                if (!s.ok()) {
                    all_success = false;
                    continue;
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
                            std::string group_value;
                            bool group_exists = false;
                            mako::Status group_status = read_current(
                                txn, group_user_key, group_key, group_value, group_exists);
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
                if ((op.flags & TXN_FLAG_SET_NX) != 0 && existed) {
                    write_allowed = false;
                }
                if ((op.flags & TXN_FLAG_SET_XX) != 0 && !existed) {
                    write_allowed = false;
                }

                result.success = true;
                if ((op.flags & TXN_FLAG_SET_RETURN_OLD) != 0 && existed) {
                    if (!copy_result_value(result, old_value)) {
                        all_success = false;
                        continue;
                    }
                }

                if (write_allowed) {
                    std::string raw_val(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
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
                auto batch_it = batch_exists.find(tl_key_buf);
                if (batch_it != batch_exists.end()) {
                    exists = batch_it->second;
                } else {
                    mako::Status expire_status = expire_if_needed(txn, user_key, tl_key_buf);
                    if (!expire_status.ok()) {
                        all_success = false;
                        continue;
                    }
                    mako::Status exists_status = g_table->Exists(txn, tl_key_buf, &exists);
                    if (!exists_status.ok()) {
                        all_success = false;
                        continue;
                    }
                }
                mako::Status s = exists ? g_table->Delete(txn, tl_key_buf) : mako::Status::OK();
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
                mako::Status s = read_user_exists(txn, user_key, tl_key_buf, exists);
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
                mako::Status s = read_user_exists(txn, user_key, tl_key_buf, exists);
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
                    s = delete_raw_if_exists(txn, tl_key_buf);
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
                mako::Status s = read_user_exists(txn, user_key, tl_key_buf, exists);
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
                    s = delete_raw_if_exists(txn, tl_key_buf);
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
                mako::Status s = read_user_exists(txn, user_key, tl_key_buf, exists);
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
                std::string scan_start = cursor.empty() ? storage_prefix : "table_key_" + cursor + '\0';
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

        // Commit or rollback based on success
        if (all_success) {
            g_mako_db->Commit(txn);
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
    if (response && response->results) {
        for (size_t i = 0; i < response->num_results; i++) {
            if (response->results[i].data_ptr) {
                std::free(response->results[i].data_ptr);
            }
        }
        std::free(response->results);
        response->results = nullptr;
        response->num_results = 0;
    }
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
        if (!metrics) {
            return false;
        }
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_mako_start_time);
        metrics->txn_commits = g_mako_txn_commits.load(std::memory_order_relaxed);
        metrics->txn_aborts = g_mako_txn_aborts.load(std::memory_order_relaxed);
        metrics->txn_retries = g_mako_txn_retries.load(std::memory_order_relaxed);
        metrics->uptime_seconds = static_cast<uint64_t>(uptime.count());
        return true;
    }

    void cpp_record_txn_retry(void) {
        g_mako_txn_retries.fetch_add(1, std::memory_order_relaxed);
    }
}

int main() {
    std::cout << "=== makoCon: Redis-compatible server with mako::DB ===" << std::endl;

    // Configuration parameters (single shard, no replication)
    int nshards = 1;
    int shard_index = 0;
    int nthreads = 32;
    if (const char* mako_redis_threads = getenv("MAKO_REDIS_THREADS")) {
        try {
            nthreads = std::stoi(mako_redis_threads);
        } catch (...) {
            std::cerr << "Invalid MAKO_REDIS_THREADS value: " << mako_redis_threads << std::endl;
            return 1;
        }
        if (nthreads < 1 || nthreads > 32) {
            std::cerr << "MAKO_REDIS_THREADS must be between 1 and 32" << std::endl;
            return 1;
        }
    }
    const char* mako_paxos_proc_name = getenv("MAKO_PAXOS_PROC_NAME");
    std::string paxos_proc_name =
            mako_paxos_proc_name ? mako_paxos_proc_name : "localhost";  // Leader by default

    // Build config path (same pattern as simpleTransactionRep.cc)
    std::string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards" + std::to_string(nshards)
            + "-warehouses" + std::to_string(nthreads) + ".yml";

    // Create transport configuration
    auto transport_config = new transport::Configuration(config_path);

    // Configure mako::Options (following simpleTransactionRep.cc pattern)
    mako::Options options;
    options.num_shards = nshards;
    options.shard_index = shard_index;
    options.num_threads = nthreads;
    options.paxos_proc_name = paxos_proc_name;
    options.replication.enabled = false;  // No replication for simplicity
    options.replication.is_leader = true;
    options.transport_config = transport_config;

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

    // Initialize Rust server (spawns N worker threads with SO_REUSEPORT)
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
