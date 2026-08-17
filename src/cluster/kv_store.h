module;
#include <string>
#include <rusty/option.hpp>   // get() returns rusty::Option<std::string>
export module cluster:kv_store;

namespace janus {

/**
 * KvStore — the minimal key-value port the cluster metadata component
 * depends on. Deliberately tiny (three methods, string keys and raw
 * byte values, stdlib only) so `cluster` compiles and unit-tests with
 * NO dependency on the storage engine.
 *
 * This is a decoupling seam, not a parallel substrate. In production
 * the port is bound to Mako's unified FullOrderedIndex — the
 * __mako_config__ system table on shard 0 — via the OrderedIndexKvStore
 * adapter (src/mako/ordered_index_kv_store.h), which lives on the mako
 * side so cluster never pulls storage headers. Tests bind an in-memory
 * fake (in_memory_kv_store.h). Either way the metadata really lives in
 * the unified store; cluster just talks to it through this narrow port.
 *
 * Authored as an inline-Rust DSL trait (docs/storage-interface.md): the
 * `#if RUSTYCPP_RUST` block below is the source of truth; the committed
 * `/*RUSTYCPP:GEN-BEGIN ...*​/` block is the generated C++ the compiler
 * sees. Regenerate with scripts/regen_storage_dsl.sh.
 */
#if RUSTYCPP_RUST
pub trait KvStore {
    // Point read: the value on hit, None on miss (no out-pointer).
    fn get(&mut self, key: &std::string) -> rusty::Option<std::string>;
    // Point write: blind overwrite (raw bytes).
    fn put(&mut self, key: &std::string, value: &std::string);
    // Point delete: no-op if the key is absent.
    fn remove(&mut self, key: &std::string);
}
#endif
export {
/*RUSTYCPP:GEN-BEGIN id=kv_store.1 version=1 rust_sha256=fcaaf367b94719c3db074bc9cef6ef83ad25c9a1fc53c9d2bcce23e6cad1f2d0*/
class KvStore {
public:
    virtual ~KvStore() noexcept(false) {}
    virtual rusty::Option<std::string> get(const std::string& key) = 0;
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual void remove(const std::string& key) = 0;
    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;
    KvStore(KvStore&&) = delete;
    KvStore& operator=(KvStore&&) = delete;
protected:
    KvStore() = default;
};

template <class U> class KvStoreAdapter;
template <class U> class KvStoreAdapterRef;
template <class U> class KvStoreAdapterRefMut;
/*RUSTYCPP:GEN-END id=kv_store.1*/
}

}  // namespace janus
