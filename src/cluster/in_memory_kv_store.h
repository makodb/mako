#pragma once

#include "kv_store.h"

#include <string>
#include <btree_port/btreemap.hpp>  // native-API ordered map (replaces std::map)

namespace janus {

/**
 * InMemoryKvStore — a btree_port::BTreeMap-backed KvStore for unit tests.
 * This is the test double of the KvStore port; it lets test_config_manager
 * drive ConfigManager / ClusterConfig / ConfigWatcher with no storage
 * engine, no cluster, no masstree config — a genuinely standalone
 * binary.
 */
// @safe - BTreeMap behind the port; no I/O.
class InMemoryKvStore : public KvStore {
public:
    // Force noexcept: the DSL-generated KvStore base has a noexcept(false)
    // destructor, which would otherwise propagate to any class holding an
    // InMemoryKvStore by value (e.g. a gtest fixture) and clash with a
    // noexcept(true) base like ::testing::Test::~Test. Narrowing a
    // noexcept(false) base dtor is legal and this dtor never throws.
    ~InMemoryKvStore() noexcept override {}

    bool get(const std::string& key, std::string* out) override {
        if (out == nullptr) return false;
        auto found = store_.get(key);
        if (found.is_none()) return false;
        *out = found.unwrap().get();
        return true;
    }

    void put(const std::string& key, const std::string& value) override {
        store_.insert(key, value);  // BTreeMap::insert overwrites
    }

    void remove(const std::string& key) override { store_.remove(key); }

    // ---- Test helpers (not part of the KvStore port) ----
    size_t size() const { return store_.size(); }
    bool contains(const std::string& key) const {
        return store_.contains_key(key);
    }
    void clear() { store_.clear(); }

private:
    btree_port::BTreeMap<std::string, std::string> store_;
};

}  // namespace janus
