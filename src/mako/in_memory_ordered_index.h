#pragma once

// An in-memory FullOrderedIndex (= abstract_ordered_index) backed by a
// std::map<string,string>. A TEST DOUBLE of the real unified storage
// interface, used to exercise code that binds to a FullOrderedIndex
// (e.g. the OrderedIndexKvStore adapter) without linking the mako
// storage engine, a running cluster, or per-thread Sto bring-up.
//
// Only the non-txn OrderedIndex point ops (get/put/insert/remove) plus
// the cheap bookkeeping (size/clear/get_table_id/get_is_remote) are
// meaningful here. The transactional (tx_*) and 2PC-participant
// (shard_*) methods, and scan/rscan, are stubbed — callers of the
// non-txn point surface never reach them, and a standalone test has no
// Sto runtime to drive them.

#include "storage/abstract_ordered_index.h"

#include <map>
#include <string>

namespace janus {

// @safe - std::map behind the non-txn point-op surface; no I/O.
class InMemoryOrderedIndex : public FullOrderedIndex {
public:
    InMemoryOrderedIndex() = default;
    // Force noexcept(true): the base's noexcept(false) dtor (a DSL-codegen
    // quirk) would otherwise propagate through any class that holds an
    // InMemoryOrderedIndex by value and clash with, e.g., gtest's
    // noexcept(true) ~Test(). Narrowing a noexcept(false) base dtor is
    // legal, and none of these dtors actually throw.
    ~InMemoryOrderedIndex() noexcept override {}

    // ---- OrderedIndex: the surface ConfigManager actually uses ----

    // @safe
    bool get(lcdf::Str key, std::string& value, size_t max_bytes_read) override {
        auto it = store_.find(std::string(key.data(),
                                          static_cast<size_t>(key.length())));
        if (it == store_.end()) return false;
        value = it->second;
        if (max_bytes_read != std::string::npos && value.size() > max_bytes_read) {
            value.resize(max_bytes_read);
        }
        return true;
    }

    // @safe - returns "newly inserted" (OrderedIndex::put contract).
    bool put(lcdf::Str key, const std::string& value) override {
        auto res = store_.insert_or_assign(
            std::string(key.data(), static_cast<size_t>(key.length())), value);
        return res.second;
    }

    // @safe - put-if-absent; returns "inserted".
    bool insert(lcdf::Str key, const std::string& value) override {
        auto res = store_.emplace(
            std::string(key.data(), static_cast<size_t>(key.length())), value);
        return res.second;
    }

    // @safe - returns "existed".
    bool remove(lcdf::Str key) override {
        return store_.erase(
                   std::string(key.data(),
                               static_cast<size_t>(key.length()))) > 0;
    }

    // @safe
    size_t size() const override { return store_.size(); }

    // @safe
    oi_stats_map clear() override {
        store_.clear();
        return {};
    }

    // @safe - fixed synthetic id; config never routes by table id here.
    int32_t get_table_id() override { return kConfigTableId; }

    // @safe - an in-memory fake is always local.
    bool get_is_remote() override { return false; }

    // ---- Unused by ConfigManager: scan + txn + 2PC participant ----
    //
    // These are stubbed deliberately. ConfigManager does only point
    // get/put/remove; a standalone config test has no Sto runtime, so
    // driving the transactional / shard-participant paths is neither
    // possible nor meaningful. If a future config feature needs scans,
    // implement scan/rscan over the ordered map here.

    // @unsafe - not exercised
    void scan(const std::string&, const std::string*, oi_scan_callback&,
              str_arena*) override {}
    // @unsafe - not exercised
    void rscan(const std::string&, const std::string*, oi_scan_callback&,
               str_arena*) override {}

    // @unsafe - not exercised
    bool tx_get(c_void*, lcdf::Str, std::string&, size_t) override { return false; }
    // @unsafe - not exercised
    void tx_put(c_void*, lcdf::Str, const std::string&) override {}
    // @unsafe - not exercised
    void tx_insert(c_void*, lcdf::Str, const std::string&) override {}
    // @unsafe - not exercised
    void tx_remove(c_void*, lcdf::Str) override {}
    // @unsafe - not exercised
    void tx_scan(c_void*, const std::string&, const std::string*,
                 oi_scan_callback&, str_arena*) override {}
    // @unsafe - not exercised
    void tx_rscan(c_void*, const std::string&, const std::string*,
                  oi_scan_callback&, str_arena*) override {}
    // @unsafe - not exercised
    void tx_scan_remote_one(c_void*, const std::string&, const std::string&,
                            std::string&) override {}

    // @unsafe - not exercised
    bool shard_get(lcdf::Str, std::string&, size_t) override { return false; }
    // @unsafe - not exercised
    const c_char* shard_put(lcdf::Str, const std::string&) override {
        return nullptr;
    }
    // @unsafe - not exercised
    bool shard_scan(const std::string&, const std::string*, oi_scan_callback&,
                    str_arena*) override {
        return false;
    }

private:
    static constexpr int32_t kConfigTableId = 0;
    std::map<std::string, std::string> store_;
};

}  // namespace janus
