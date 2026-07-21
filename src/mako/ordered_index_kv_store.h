#pragma once

// OrderedIndexKvStore — the adapter that binds the cluster KvStore port
// onto Mako's unified FullOrderedIndex (abstract_ordered_index). It
// lives on the mako side, NOT in src/cluster/, so the cluster metadata
// component stays free of storage-engine headers. Production wiring
// (shard 0 bootstrap) constructs the __mako_config__ mbta index, wraps
// it in an OrderedIndexKvStore, and hands that to ConfigManager.
//
// ConfigManager uses only the non-txn OrderedIndex point ops, and on
// that surface values are raw bytes in both directions (no
// mako::Encode) — see abstract_ordered_index.h. So the adapter is a
// straight std::string <-> lcdf::Str shim.

import cluster;   // config/sharding metadata module (was #include "cluster/...")
#include "storage/abstract_ordered_index.h"

#include <string>
#include <rusty/option.hpp>   // KvStore::get returns rusty::Option<std::string>

namespace janus {

// @safe - thin shim from the KvStore port to a FullOrderedIndex.
class OrderedIndexKvStore : public KvStore {
public:
    // Non-owning: the index outlives this adapter (owned by the shard).
    explicit OrderedIndexKvStore(::FullOrderedIndex* index) : index_(index) {}

    // @unsafe - FullOrderedIndex point read
    rusty::Option<std::string> get(const std::string& key) override {
        if (index_ == nullptr) return rusty::None;
        std::string out;
        if (index_->get(lcdf::Str(key.data(), key.size()), out,
                        std::string::npos)) {
            return rusty::Some(std::move(out));
        }
        return rusty::None;
    }

    // @unsafe - FullOrderedIndex point put
    void put(const std::string& key, const std::string& value) override {
        if (index_ != nullptr) {
            index_->put(lcdf::Str(key.data(), key.size()), value);
        }
    }

    // @unsafe - FullOrderedIndex point remove
    void remove(const std::string& key) override {
        if (index_ != nullptr) {
            index_->remove(lcdf::Str(key.data(), key.size()));
        }
    }

private:
    ::FullOrderedIndex* index_;
};

}  // namespace janus
