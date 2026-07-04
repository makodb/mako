#pragma once

#include "kv_store.h"

#include <map>
#include <string>

namespace janus {

/**
 * InMemoryKvStore — a std::map-backed KvStore for unit tests. This is
 * the test double of the KvStore port; it lets test_config_manager
 * drive ConfigManager / ClusterConfig / ConfigWatcher with no storage
 * engine, no cluster, no masstree config — a genuinely standalone
 * binary.
 */
// @safe - std::map behind the port; no I/O.
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
        auto it = store_.find(key);
        if (it == store_.end()) return false;
        *out = it->second;
        return true;
    }

    void put(const std::string& key, const std::string& value) override {
        store_[key] = value;
    }

    void remove(const std::string& key) override { store_.erase(key); }

    // ---- Test helpers (not part of the KvStore port) ----
    size_t size() const { return store_.size(); }
    bool contains(const std::string& key) const {
        return store_.find(key) != store_.end();
    }
    void clear() { store_.clear(); }

private:
    std::map<std::string, std::string> store_;
};

}  // namespace janus
