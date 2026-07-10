module;

// ShardDataCatalog — the per-shard registry of named migratable tables behind
// the ShardDataService: every RPC names its table; the service resolves it
// here, creating the table on first use, so ONE service per shard covers all
// of that shard's migratable tables. A port beside ShardData: the production
// catalog (engine-backed standalone indexes, table-registry registration,
// engine thread-registration gates) lives mako-side in
// src/mako/shard_data_plane.cc; SingleTableCatalog below is the test/harness
// adapter preserving the single-index service shape.

#include <string>

export module cluster:shard_data_catalog;
import :shard_data;

export namespace janus {

// @unsafe - returned ShardData is borrowed (catalog-owned, process-lifetime).
class ShardDataCatalog {
public:
    virtual ~ShardDataCatalog() = default;
    // Resolve `table` to its data plane, creating it on first use.
    // Returns nullptr only if the table cannot be created.
    virtual ShardData* get_or_create(const std::string& table) = 0;
};

// Test/adapter catalog: every name resolves to the one wrapped table.
// @unsafe - non-owning.
class SingleTableCatalog : public ShardDataCatalog {
public:
    explicit SingleTableCatalog(ShardData* shard) : shard_(shard) {}
    ShardData* get_or_create(const std::string&) override { return shard_; }
private:
    ShardData* shard_;
};

}  // namespace janus
