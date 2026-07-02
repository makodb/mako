#pragma once

/**
 * mako/kv_backends.hh - The namespace-switchable KV surface
 * (docs/mako-nontxn-api-plan.md, Phase 5).
 *
 * Three namespaces export the SAME surface, so consumers pick a
 * backend by switching one alias and nothing else:
 *
 *     namespace kv = kv_silo;          // or kv_masstree / kv_mako
 *     kv::thread_init();
 *     kv::Table* t = kv::open("mytable");
 *     t->put(lcdf::Str("k"), "value");
 *
 * Surface per namespace:
 *     Table* open(const std::string& name);   // find-or-create
 *     void   thread_init();                   // once per thread
 *     class Table {
 *         bool get(lcdf::Str key, std::string& value,
 *                  size_t max_bytes_read = npos);   // found?
 *         bool put(lcdf::Str key, const std::string& value);    // new?
 *         bool insert(lcdf::Str key, const std::string& value); // inserted?
 *         bool remove(lcdf::Str key);                           // existed?
 *         void scan (const std::string& start, const std::string* end,
 *                    abstract_ordered_index::scan_callback& cb,
 *                    str_arena* arena = nullptr);   // [start, end)
 *         void rscan(const std::string& start, const std::string* end,
 *                    abstract_ordered_index::scan_callback& cb,
 *                    str_arena* arena = nullptr);   // descending
 *     };
 *
 * Values are RAW BYTES on every backend: the silo/mako shims apply
 * mako::Encode() on writes and rely on the L3 ops' stripping on
 * reads, so the EXTRA_BITS convention never leaks to callers (the
 * masstree backend has no such convention to begin with).
 *
 * Backends:
 *   kv_masstree — L1: a value-owning adapter over concurrent_btree
 *                 (plain Masstree, no transactions). Value frees on
 *                 overwrite/remove are RCU-deferred, so concurrent
 *                 readers are safe.
 *   kv_silo     — L3: mbta_ordered_index's non-txn ops (each op an
 *                 internal one-op OCC txn).
 *   kv_mako     — L4: mbta_sharded_ordered_index's non-txn routing
 *                 (single local shard when opened via this header;
 *                 in a sharded deployment remote keys travel the
 *                 self-contained non-txn RPCs).
 */

// Order matters: MassTrans.hh (via mbta_wrapper.hh) does
// `#define RCU 1`, which would mangle imstring.h's `template <bool
// RCU>` — so the plain-masstree headers must come first.
#include "masstree_btree.h"  // concurrent_btree (plain Masstree, L1)
#include "varkey.h"
#include "benchmarks/mbta_wrapper.hh"
#include "benchmarks/mbta_sharded_ordered_index.hh"
#include "lib/common.h"

// ============================================================================
// Shared per-process / per-thread bring-up
// ============================================================================
namespace mako_kv_detail {

// Idempotent Silo/Masstree thread binding — the same minimal contract
// tests/test_silo_nontxn_api.cc proved: TThread id + mode 0, MassTrans
// static_init once, per-thread masstree threadinfo. All three backends
// share one storage runtime, so one init covers them.
// @unsafe - mutates Sto/masstree thread-local state
inline void ensure_thread_init() {
    static std::atomic<int> tid_counter{0};
    thread_local bool done = false;
    if (done) return;
    done = true;

    static std::once_flag static_once;
    std::call_once(static_once, [] {
        mbta_ordered_index::mbta_type::static_init();
    });

    TThread::set_id(tid_counter.fetch_add(1));
    TThread::set_mode(0);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    mbta_ordered_index::mbta_type::thread_init();
}

// Find-or-create registry shared by the open() implementations.
template <typename T>
inline T* registry_open(const std::string& name,
                        const std::function<T*(const std::string&)>& make) {
    static std::mutex mu;
    static std::map<std::string, T*> tables;
    std::lock_guard<std::mutex> lock(mu);
    auto it = tables.find(name);
    if (it != tables.end()) return it->second;
    T* t = make(name);
    tables[name] = t;
    return t;
}

}  // namespace mako_kv_detail

// ============================================================================
// kv_masstree — plain Masstree (L1) with owned values
// ============================================================================
namespace kv_masstree {

class Table {
public:
    explicit Table(std::string name) : name_(std::move(name)) {}

    // Value buffers live in the RCU arena as [u32 len][bytes], so an
    // overwrite/remove can defer the free past any concurrent reader's
    // pinned epoch.

    // @unsafe - copies out of an RCU-protected buffer
    bool get(lcdf::Str key, std::string& value,
             size_t max_bytes_read = std::string::npos) {
        scoped_rcu_region guard;  // pins the value buffer while we copy
        concurrent_btree::value_type v{};
        if (!tree_.search(to_key(key), v)) return false;
        uint32_t len;
        memcpy(&len, v, sizeof(len));
        size_t n = std::min<size_t>(len, max_bytes_read);
        value.assign(reinterpret_cast<const char*>(v) + sizeof(len), n);
        return true;
    }

    // @unsafe - RCU arena allocation + deferred free of the old value
    bool put(lcdf::Str key, const std::string& value) {
        scoped_rcu_region guard;
        concurrent_btree::value_type old = nullptr;
        bool inserted = tree_.insert(to_key(key), make_val(value), &old);
        if (!inserted && old != nullptr) rcu_free_val(old);
        return inserted;
    }

    // @unsafe - RCU arena allocation
    bool insert(lcdf::Str key, const std::string& value) {
        scoped_rcu_region guard;
        concurrent_btree::value_type v = make_val(value);
        if (tree_.insert_if_absent(to_key(key), v)) return true;
        // Never published — safe to free immediately.
        rcu::s_instance.dealloc(v, sizeof(uint32_t) + value.size());
        return false;
    }

    // @unsafe - deferred free of the removed value
    bool remove(lcdf::Str key) {
        scoped_rcu_region guard;
        concurrent_btree::value_type old = nullptr;
        if (!tree_.remove(to_key(key), &old)) return false;
        if (old != nullptr) rcu_free_val(old);
        return true;
    }

    // [start, end); ascending — same contract as the L3 non-txn scan.
    // @unsafe - iterates RCU-protected buffers
    void scan(const std::string& start_key, const std::string* end_key,
              abstract_ordered_index::scan_callback& callback,
              str_arena* = nullptr) {
        scoped_rcu_region guard;
        Collector c(callback);
        varkey lower = to_key(lcdf::Str(start_key));
        if (end_key != nullptr) {
            varkey upper = to_key(lcdf::Str(*end_key));
            tree_.search_range(lower, &upper, c);
        } else {
            tree_.search_range(lower, nullptr, c);
        }
    }

    // Descending from start_key down to end_key.
    // @unsafe - iterates RCU-protected buffers
    void rscan(const std::string& start_key, const std::string* end_key,
               abstract_ordered_index::scan_callback& callback,
               str_arena* = nullptr) {
        scoped_rcu_region guard;
        Collector c(callback);
        varkey upper = to_key(lcdf::Str(start_key));
        if (end_key != nullptr) {
            varkey lower = to_key(lcdf::Str(*end_key));
            tree_.rsearch_range(upper, &lower, c);
        } else {
            tree_.rsearch_range(upper, nullptr, c);
        }
    }

    const std::string& name() const { return name_; }

private:
    // Bridges mbtree's functor protocol to the shared scan_callback.
    struct Collector {
        explicit Collector(abstract_ordered_index::scan_callback& cb)
            : cb_(cb) {}
        // @unsafe - decodes the RCU-protected value buffer
        bool operator()(const lcdf::Str& k, concurrent_btree::value_type v) {
            uint32_t len;
            memcpy(&len, v, sizeof(len));
            buf_.assign(reinterpret_cast<const char*>(v) + sizeof(len), len);
            return cb_.invoke(k.data(), k.length(), buf_);
        }
        abstract_ordered_index::scan_callback& cb_;
        std::string buf_;
    };

    static varkey to_key(lcdf::Str s) {
        return varkey(reinterpret_cast<const uint8_t*>(s.data()), s.length());
    }

    // @unsafe - RCU arena allocation
    static concurrent_btree::value_type make_val(const std::string& value) {
        uint32_t len = static_cast<uint32_t>(value.size());
        auto* p = static_cast<uint8_t*>(
            rcu::s_instance.alloc(sizeof(len) + value.size()));
        memcpy(p, &len, sizeof(len));
        memcpy(p + sizeof(len), value.data(), value.size());
        return p;
    }

    // @unsafe - schedules a deferred free (readers may still hold v)
    static void rcu_free_val(concurrent_btree::value_type v) {
        uint32_t len;
        memcpy(&len, v, sizeof(len));
        rcu::s_instance.dealloc_rcu(v, sizeof(len) + len);
    }

    std::string name_;
    concurrent_btree tree_;
};

inline void thread_init() { mako_kv_detail::ensure_thread_init(); }

inline Table* open(const std::string& name) {
    return mako_kv_detail::registry_open<Table>(
        name, [](const std::string& n) { return new Table(n); });
}

}  // namespace kv_masstree

// ============================================================================
// kv_silo — L3 non-txn ops (one-op OCC transactions)
// ============================================================================
namespace kv_silo {

class Table {
public:
    Table(std::string name, mbta_ordered_index* tbl)
        : name_(std::move(name)), tbl_(tbl) {}

    // @unsafe - L3 non-txn op (internal one-op OCC txn)
    bool get(lcdf::Str key, std::string& value,
             size_t max_bytes_read = std::string::npos) {
        return tbl_->get(key, value, max_bytes_read);
    }

    // @unsafe - L3 non-txn op; Encode applied so callers pass raw bytes
    bool put(lcdf::Str key, const std::string& value) {
        return tbl_->put(key, mako::Encode(value));
    }

    // @unsafe - L3 non-txn op; Encode applied so callers pass raw bytes
    bool insert(lcdf::Str key, const std::string& value) {
        return tbl_->insert(key, mako::Encode(value));
    }

    // @unsafe - direct raw write through the MassTrans cursor
    bool remove(lcdf::Str key) { return tbl_->remove(key); }

    // @unsafe - L3 non-txn scan (values arrive stripped)
    void scan(const std::string& start_key, const std::string* end_key,
              abstract_ordered_index::scan_callback& callback,
              str_arena* arena = nullptr) {
        tbl_->scan(start_key, end_key, callback, arena);
    }

    // @unsafe - L3 non-txn rscan (values arrive stripped)
    void rscan(const std::string& start_key, const std::string* end_key,
               abstract_ordered_index::scan_callback& callback,
               str_arena* arena = nullptr) {
        tbl_->rscan(start_key, end_key, callback, arena);
    }

    const std::string& name() const { return name_; }
    mbta_ordered_index* underlying() { return tbl_; }

private:
    std::string name_;
    mbta_ordered_index* tbl_;
};

inline void thread_init() { mako_kv_detail::ensure_thread_init(); }

inline Table* open(const std::string& name) {
    return mako_kv_detail::registry_open<Table>(
        name, [](const std::string& n) {
            static std::atomic<long> table_id{7000};
            return new Table(
                n, new mbta_ordered_index(n, table_id.fetch_add(1),
                                          /*db=*/nullptr));
        });
}

}  // namespace kv_silo

// ============================================================================
// kv_mako — L4 sharded routing over the same non-txn ops
// ============================================================================
namespace kv_mako {

class Table {
public:
    Table(std::string name, mbta_sharded_ordered_index* tbl)
        : name_(std::move(name)), tbl_(tbl) {}

    // @unsafe - routed L3 non-txn op
    bool get(lcdf::Str key, std::string& value,
             size_t max_bytes_read = std::string::npos) {
        return tbl_->get(key, value, max_bytes_read);
    }

    // @unsafe - routed L3 non-txn op; Encode applied internally
    bool put(lcdf::Str key, const std::string& value) {
        return tbl_->put(key, mako::Encode(value));
    }

    // @unsafe - routed L3 non-txn op; Encode applied internally
    bool insert(lcdf::Str key, const std::string& value) {
        return tbl_->insert(key, mako::Encode(value));
    }

    // @unsafe - routed direct raw write
    bool remove(lcdf::Str key) { return tbl_->remove(key); }

    // @unsafe - local-shard scan (cross-shard scan unsupported, as at L3/L4)
    void scan(const std::string& start_key, const std::string* end_key,
              abstract_ordered_index::scan_callback& callback,
              str_arena* arena = nullptr) {
        tbl_->scan(start_key, end_key, callback, arena);
    }

    // @unsafe - local-shard rscan
    void rscan(const std::string& start_key, const std::string* end_key,
               abstract_ordered_index::scan_callback& callback,
               str_arena* arena = nullptr) {
        tbl_->rscan(start_key, end_key, callback, arena);
    }

    const std::string& name() const { return name_; }
    mbta_sharded_ordered_index* underlying() { return tbl_; }

private:
    std::string name_;
    mbta_sharded_ordered_index* tbl_;
};

inline void thread_init() { mako_kv_detail::ensure_thread_init(); }

inline Table* open(const std::string& name) {
    return mako_kv_detail::registry_open<Table>(
        name, [](const std::string& n) {
            static std::atomic<long> table_id{8000};
            auto* local = new mbta_ordered_index(n, table_id.fetch_add(1),
                                                 /*db=*/nullptr);
            auto* sharded = new mbta_sharded_ordered_index(
                n, std::vector<abstract_ordered_index*>{local});
            return new Table(n, sharded);
        });
}

}  // namespace kv_mako
