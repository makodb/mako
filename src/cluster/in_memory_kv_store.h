module;
#include <string>
#include <rusty/array.hpp>   // c529cd3d: BTreeMap::len() free-fn decl
#include <rusty/option.hpp>         // KvStore::get returns rusty::Option<std::string>
export module cluster:in_memory_kv_store;
import btree_port.btree.map;   // c529cd3d: btree_port is now a C++20 module (retired the .hpp header)
import :kv_store;

namespace btree_port { using btree::map::BTreeMap; }  // compat: flat name the DSL/GEN expect

namespace janus {

/**
 * InMemoryKvStore — a btree_port::BTreeMap-backed KvStore for unit tests.
 * This is the test double of the KvStore port; it lets test_config_manager
 * drive ConfigManager / ClusterConfig / ConfigWatcher with no storage
 * engine, no cluster, no masstree config — a genuinely standalone
 * binary.
 */
// @safe - BTreeMap behind the port; no I/O.
export class InMemoryKvStore : public KvStore {
public:
    // Force noexcept: the DSL-generated KvStore base has a noexcept(false)
    // destructor, which would otherwise propagate to any class holding an
    // InMemoryKvStore by value (e.g. a gtest fixture) and clash with a
    // noexcept(true) base like ::testing::Test::~Test. Narrowing a
    // noexcept(false) base dtor is legal and this dtor never throws.
    ~InMemoryKvStore() noexcept override {}

    rusty::Option<std::string> get(const std::string& key) override {
        auto found = store_.get(key);
        if (found.is_none()) return rusty::None;
        return rusty::Some(std::string(found.unwrap()));
    }

    void put(const std::string& key, const std::string& value) override {
        store_.insert(key, value);  // BTreeMap::insert overwrites
    }

    void remove(const std::string& key) override { store_.remove(key); }

    // ---- Test helpers (not part of the KvStore port) ----
    size_t size() const { return store_.len(); }
    bool contains(const std::string& key) const {
        return store_.contains_key(key);
    }
    void clear() { store_.clear(); }

private:
    // Default member initializer so InMemoryKvStore stays default-constructible:
    // the c529cd3d btree_port module BTreeMap has no default ctor, and gtest
    // fixtures hold an InMemoryKvStore by value (`InMemoryKvStore kv_;`).
    btree_port::BTreeMap<std::string, std::string> store_ =
        btree_port::BTreeMap<std::string, std::string>::new_();
};

}  // namespace janus
