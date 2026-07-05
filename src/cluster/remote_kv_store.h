#pragma once

#include "kv_store.h"

#include <functional>
#include <string>
#include <rusty/function.hpp>   // rusty::Function — DSL-invocable callable
#include <rusty/option.hpp>     // get() / read_fn return rusty::Option<std::string>

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
 * function type is the namespace-scope RemoteKvStoreReadFn (a
 * rusty::Function) so the DSL struct can name it as a field type AND invoke
 * it directly — ((*self).read_fn)(key, out). No erased-callable kernel.
 */

// read_fn(key) -> Some(value) on hit, None on miss. In production this issues
// a ReadConfigKey RPC to shard 0's leader. rusty::Function (move-only) instead
// of std::function so the DSL can invoke it directly — its operator() means
// `self.read_fn(key)` is plain-safe DSL, no erased-callable kernel. Returns an
// Option (not a bool + out-pointer) so the whole read path is pointer-free.
using RemoteKvStoreReadFn =
    rusty::Function<rusty::Option<std::string>(const std::string& key)>;

#if RUSTYCPP_RUST
pub struct RemoteKvStore {
    read_fn: RemoteKvStoreReadFn,
}
#[cpp_inherit]
impl KvStore for RemoteKvStore {
    // Delegates to the injected reader (called directly — rusty::Function).
    fn get(&mut self, key: &std::string) -> rusty::Option<std::string> {
        ((*self).read_fn)(key)
    }
    // Read-only consumer: writes are not permitted (no-op).
    fn put(&mut self, key: &std::string, value: &std::string) {}
    fn remove(&mut self, key: &std::string) {}
}
#endif
/*RUSTYCPP:GEN-BEGIN id=remote_kv_store.1 version=1 rust_sha256=fccf08dd94fb20623f53a392fa432035dfc309953b1dd94cef9b7d35f249a619*/
struct RemoteKvStore;

struct RemoteKvStore : public KvStore {
    RemoteKvStoreReadFn read_fn;
    RemoteKvStore(RemoteKvStoreReadFn read_fn_init) : KvStore(), read_fn(std::move(read_fn_init)) {}
    RemoteKvStore(RemoteKvStore&& other) noexcept : KvStore(), read_fn(std::move(other.read_fn)) {}


    rusty::Option<std::string> get(const std::string& key);
    void put(const std::string& key, const std::string& value);
    void remove(const std::string& key);
};


inline rusty::Option<std::string> RemoteKvStore::get(const std::string& key) {
    return (((*this)).read_fn)(key);
}

inline void RemoteKvStore::put(const std::string& key, const std::string& value) {
}

inline void RemoteKvStore::remove(const std::string& key) {
}
/*RUSTYCPP:GEN-END id=remote_kv_store.1*/

}  // namespace janus
