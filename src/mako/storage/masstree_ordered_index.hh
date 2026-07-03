#pragma once

/**
 * masstree_ordered_index - plain Masstree (L1) behind the shared
 * abstract_ordered_index interface.
 *
 * The third table backend beside mbta_ordered_index (Silo) and
 * mbta_sharded_ordered_index (Mako routing): callers hold an
 * abstract_ordered_index* and pick the backend at construction —
 * there is no separate API surface for masstree.
 *
 * Only the NON-TRANSACTIONAL ops are supported (masstree has no
 * transaction runtime); the transactional/2PC virtuals abort loudly
 * via NDB_UNIMPLEMENTED. Values are raw bytes in both directions, per
 * the non-txn contract in abstract_ordered_index.h — this backend has
 * no storage encoding at all.
 *
 * Value ownership: masstree stores raw pointers, so this class owns
 * the value allocations. Buffers live in the RCU arena as
 * [u32 len][bytes]; overwrite/remove frees are RCU-deferred and every
 * op pins a scoped_rcu_region, so concurrent readers are safe.
 *
 * Thread contract: same per-thread bring-up as the rest of the engine
 * (masstree threadinfo + RCU registration — scoped_db_thread_ctx or
 * an mbta-style thread_init covers it).
 *
 * INCLUDE ORDER: this header must be included BEFORE any header that
 * pulls in sto/MassTrans.hh (e.g. mbta_wrapper.hh) —
 * MassTrans does `#define RCU 1`, which mangles imstring.h's
 * `template <bool RCU>` parameter that varkey.h drags in.
 */

#include "mako/masstree_btree.h"  // concurrent_btree
#include "mako/varkey.h"
#include "abstract_ordered_index.h"

#include <stdint.h>
#include <string.h>
#include <string>
#include <map>
#include <algorithm>

// OrderedIndex ONLY — masstree has no transaction runtime, and after
// the trait split (docs/ordered-index-trait-plan.md) that is a type
// fact rather than a set of aborting stubs: this class simply does
// not implement TxnOrderedIndex or ShardParticipant.
class masstree_ordered_index : public OrderedIndex {
public:
    masstree_ordered_index(std::string name, int table_id)
        : name_(std::move(name)), table_id_(table_id) {}

    // ========================================================================
    // Non-transactional API (the supported surface)
    // ========================================================================

    // @unsafe - copies out of an RCU-protected buffer
    bool get(lcdf::Str key, std::string &value,
             size_t max_bytes_read = std::string::npos) override {
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
    bool put(lcdf::Str key, const std::string &value) override {
        scoped_rcu_region guard;
        concurrent_btree::value_type old = nullptr;
        bool inserted = tree_.insert(to_key(key), make_val(value), &old);
        if (!inserted && old != nullptr) rcu_free_val(old);
        return inserted;
    }

    // @unsafe - RCU arena allocation
    bool insert(lcdf::Str key, const std::string &value) override {
        scoped_rcu_region guard;
        concurrent_btree::value_type v = make_val(value);
        if (tree_.insert_if_absent(to_key(key), v)) return true;
        // Never published — safe to free immediately.
        rcu::s_instance.dealloc(v, sizeof(uint32_t) + value.size());
        return false;
    }

    // @unsafe - deferred free of the removed value
    bool remove(lcdf::Str key) override {
        scoped_rcu_region guard;
        concurrent_btree::value_type old = nullptr;
        if (!tree_.remove(to_key(key), &old)) return false;
        if (old != nullptr) rcu_free_val(old);
        return true;
    }

    // @unsafe - iterates RCU-protected buffers
    void scan(const std::string &start_key, const std::string *end_key,
              oi_scan_callback &callback, str_arena * = nullptr) override {
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

    // @unsafe - iterates RCU-protected buffers
    void rscan(const std::string &start_key, const std::string *end_key,
               oi_scan_callback &callback, str_arena * = nullptr) override {
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

    // ========================================================================
    // Bookkeeping
    // ========================================================================

    // @safe - masstree maintains the count internally
    size_t size() const override { return tree_.size(); }

    // @unsafe - NOT THREAD SAFE (mbtree::clear contract); leaks owned
    // values deliberately — reclaiming them would need a full scan and
    // clear() is a test/teardown affordance, not a hot path.
    std::map<std::string, uint64_t> clear() {
        tree_.clear();
        return {};
    }

    int get_table_id() override { return table_id_; }
    bool get_is_remote() override { return false; }

    const std::string &name() const { return name_; }

private:
    // Bridges mbtree's functor protocol to the shared scan_callback.
    struct Collector {
        explicit Collector(oi_scan_callback &cb) : cb_(cb) {}
        // @unsafe - decodes the RCU-protected value buffer
        bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
            uint32_t len;
            memcpy(&len, v, sizeof(len));
            buf_.assign(reinterpret_cast<const char*>(v) + sizeof(len), len);
            return cb_.invoke(k.data(), k.length(), buf_);
        }
        oi_scan_callback &cb_;
        std::string buf_;
    };

    static varkey to_key(lcdf::Str s) {
        return varkey(reinterpret_cast<const uint8_t*>(s.data()), s.length());
    }

    // @unsafe - RCU arena allocation
    static concurrent_btree::value_type make_val(const std::string &value) {
        uint32_t len = static_cast<uint32_t>(value.size());
        auto *p = static_cast<uint8_t*>(
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
    int table_id_;
    concurrent_btree tree_;
};
