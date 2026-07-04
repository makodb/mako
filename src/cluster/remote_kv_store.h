#pragma once

#include "kv_store.h"

#include <functional>
#include <string>

namespace janus {

/**
 * RemoteKvStore — the KvStore a non-shard-0 node uses to read cluster
 * config. Shard 0's leader owns the __mako_config__ table; every other
 * node discovers topology by reading those keys *from shard 0*. This
 * adapter turns a KvStore get() into whatever remote read the caller
 * injects — in production a ReadConfigKey RPC to shard 0's leader; in
 * tests a closure over an in-memory store standing in for shard 0.
 *
 * Transport-agnostic on purpose: it depends only on the injected
 * ReadFn, so it stays in cluster/ (no RPC-layer dependency) and is
 * unit-testable standalone.
 *
 * Read-only: config mutation is shard-0-only. A read-only consumer that
 * tries to Put/Delete is a bug, so the writes are no-ops. Because
 * ConfigManager and ConfigWatcher only call get() when loading config,
 * wrapping a ConfigManager around a RemoteKvStore makes
 * ClusterConfig::LoadFromConfigManager work transparently against shard 0.
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the
 * `#if RUSTYCPP_RUST` block is the source of truth, the GEN block is the
 * generated C++. Regenerate with scripts/regen_storage_dsl.sh. The read
 * function type is now the namespace-scope RemoteKvStoreReadFn (was the
 * nested RemoteKvStore::ReadFn) so the DSL struct can name it as a field
 * type; invoking a std::function is the one thing the DSL can't spell, so
 * it goes through the rkv_invoke kernel.
 */

// read_fn(key, out_value) -> found. In production this issues a
// ReadConfigKey RPC to shard 0's leader and fills *out on hit.
using RemoteKvStoreReadFn =
    std::function<bool(const std::string& key, std::string* out_value)>;

// @unsafe - invokes an injected std::function (a call through an
// erased callable is not borrow-checkable).
inline bool rkv_invoke(const RemoteKvStoreReadFn* fn,
                       const std::string& key, std::string* out) {
    if (fn == nullptr || !*fn || out == nullptr) return false;
    return (*fn)(key, out);
}

#if RUSTYCPP_RUST
pub struct RemoteKvStore {
    read_fn: RemoteKvStoreReadFn,
}
#[cpp_inherit]
impl KvStore for RemoteKvStore {
    // Delegates to the injected reader.
    fn get(&mut self, key: &std::string, out: *mut std::string) -> bool {
        unsafe { rkv_invoke(&self.read_fn, key, out) }
    }
    // Read-only consumer: writes are not permitted (no-op).
    fn put(&mut self, key: &std::string, value: &std::string) {}
    fn remove(&mut self, key: &std::string) {}
}
#endif
/*RUSTYCPP:GEN-BEGIN id=remote_kv_store.1 version=1 rust_sha256=3bcf00df6e9c66d5d9969d3f554cfb8ab6b2eb7e5544f2f9de734945dc075ddc*/
struct RemoteKvStore;

struct RemoteKvStore : public KvStore {
    RemoteKvStoreReadFn read_fn;
    RemoteKvStore(RemoteKvStoreReadFn read_fn_init) : KvStore(), read_fn(std::move(read_fn_init)) {}
    RemoteKvStore(RemoteKvStore&& other) noexcept : KvStore(), read_fn(std::move(other.read_fn)) {}


    bool get(const std::string& key, std::string* out);
    void put(const std::string& key, const std::string& value);
    void remove(const std::string& key);
};


inline bool RemoteKvStore::get(const std::string& key, std::string* out) {
    // @unsafe
    {
        return rkv_invoke(&this->read_fn, key, out);
    }
}

inline void RemoteKvStore::put(const std::string& key, const std::string& value) {
}

inline void RemoteKvStore::remove(const std::string& key) {
}
/*RUSTYCPP:GEN-END id=remote_kv_store.1*/

}  // namespace janus
