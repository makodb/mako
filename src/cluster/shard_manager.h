#pragma once

// ShardManager — the control plane under test. It drives the REAL cluster
// reconfiguration path (janus::ConfigManager writes + janus::ClusterConfig
// routing) against a map of stub Shards, so the add/kill/remove lifecycle can
// be exercised end-to-end without the storage engine or RPC. Each verb calls
// the authoritative ConfigManager verb, applies the matching effect to the
// stub shards (kill_shard migrates the dead shard's data into the taker), and
// reloads the routing cache; route/put/get then go through
// ClusterConfig::get_shard_for_key (hash-mod default + dead->taker follow).
//
// ConfigManager and ClusterConfig are borrowed (the caller owns them,
// ConfigManager over an InMemoryKvStore) — the same *mut dependency-injection
// seam ConfigManager itself uses for its KvStore. Swap the stub Shard for a
// masstree-backed shard to run this same manager against real storage.
//
// Authored in the inline-Rust DSL (docs/storage-interface.md): the
// `#if RUSTYCPP_RUST` block is the source of truth; regenerate with
// scripts/regen_storage_dsl.sh.

#include "shard.h"
#include "config_manager.h"
#include "cluster_config.h"

#include <string>
#include <vector>
#include <btree_port/btreemap.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like (generated bodies)

namespace janus {

#if RUSTYCPP_RUST
pub struct ShardManager {
    cm: *mut ConfigManager,     // borrowed: authoritative config (over a KvStore)
    cfg: *mut ClusterConfig,    // borrowed: routing cache, reloaded from cm
    shards: btree_port::BTreeMap<u32, Shard>,   // stub data shards by id
}
impl ShardManager {
    fn new(cm: *mut ConfigManager, cfg: *mut ClusterConfig) -> ShardManager {
        ShardManager {
            cm: cm,
            cfg: cfg,
            shards: btree_port::BTreeMap::<u32, Shard>::new_(),
        }
    }
    // Pull the authoritative config into the routing cache.
    fn reload(&mut self) {
        unsafe { (*(*self).cfg).load_from_config_manager((*self).cm) };
    }
    // Route a key to a live shard: hash-mod default, then dead->taker chase.
    fn route(&self, key: &std::string) -> u32 {
        unsafe { (*(*self).cfg).get_shard_for_key_default(key) }
    }
    // Client write: route then store on the owning shard.
    fn put(&mut self, key: &std::string, value: &std::string) {
        let sid: u32 = self.route(key);
        if (*self).shards.contains_key(sid) {
            (*self).shards.get_mut(sid).unwrap().get().put(key, value);
        }
    }
    // Client read: route then read from the owning shard.
    fn get(&mut self, key: &std::string) -> rusty::Option<std::string> {
        let sid: u32 = self.route(key);
        if !(*self).shards.contains_key(sid) {
            return rusty::None;
        }
        (*self).shards.get(sid).unwrap().get().get(key)
    }
    // Add a shard: config verb + fresh stub + reload.
    fn add_shard(&mut self, id: u32, replicas: &std::vector<std::string>) -> bool {
        let ok: bool = unsafe { (*(*self).cm).add_shard(id, replicas) };
        if ok {
            (*self).shards.insert(id, Shard::new(id));
            self.reload();
        }
        ok
    }
    // Kill `dead`, handing its data AND its routing over to `taker`. The
    // ConfigManager marks dead + records the replacement pointer; we migrate
    // the stub data into the taker and mark the dead stub, then reload so
    // routing chases dead->taker.
    fn kill_shard(&mut self, dead: u32, taker: u32) -> bool {
        let ok: bool = unsafe { (*(*self).cm).kill_shard(dead, taker) };
        if ok {
            let removed: rusty::Option<Shard> = (*self).shards.remove(dead);
            if removed.is_some() {
                let mut dead_shard: Shard = removed.unwrap();
                dead_shard.mark_dead();
                if (*self).shards.contains_key(taker) {
                    (*self).shards.get_mut(taker).unwrap().get().absorb(&mut dead_shard);
                }
                (*self).shards.insert(dead, dead_shard);
            }
            self.reload();
        }
        ok
    }
    // Remove a shard entirely (config verb + drop the stub + reload). Unlike
    // kill, this does not migrate data — callers use it for clean teardown.
    fn remove_shard(&mut self, id: u32) -> bool {
        let ok: bool = unsafe { (*(*self).cm).remove_shard(id) };
        if ok {
            (*self).shards.remove(id);
            self.reload();
        }
        ok
    }
    // ---- observers (for tests) ------------------------------------------
    fn shard_count(&self) -> u32 {
        unsafe { (*(*self).cfg).get_shard_count() }
    }
    fn epoch(&mut self) -> u64 {
        unsafe { (*(*self).cm).get_epoch() }
    }
    fn is_shard_alive(&self, id: u32) -> bool {
        if !(*self).shards.contains_key(id) {
            return false;
        }
        (*self).shards.get(id).unwrap().get().is_alive()
    }
    fn shard_key_count(&self, id: u32) -> usize {
        if !(*self).shards.contains_key(id) {
            return 0;
        }
        (*self).shards.get(id).unwrap().get().key_count()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard_manager.1 version=1 rust_sha256=37ffbca69ba7c9b945ae3245b2e3515be2b8c549e475181e71fea0de32341781*/
struct ShardManager;

struct ShardManager {
    ConfigManager* cm;
    ClusterConfig* cfg;
    btree_port::BTreeMap<uint32_t, Shard> shards;

    static ShardManager new_(ConfigManager* cm, ClusterConfig* cfg);
    void reload();
    uint32_t route(const std::string& key) const;
    void put(const std::string& key, const std::string& value);
    rusty::Option<std::string> get(const std::string& key);
    bool add_shard(uint32_t id, const std::vector<std::string>& replicas);
    bool kill_shard(uint32_t dead, uint32_t taker);
    bool remove_shard(uint32_t id);
    uint32_t shard_count() const;
    uint64_t epoch();
    bool is_shard_alive(uint32_t id) const;
    size_t shard_key_count(uint32_t id) const;
};


inline ShardManager ShardManager::new_(ConfigManager* cm, ClusterConfig* cfg) {
    return ShardManager{.cm = cm, .cfg = cfg, .shards = btree_port::BTreeMap<uint32_t, Shard>::new_()};
}

inline void ShardManager::reload() {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).load_from_config_manager(((*this)).cm);
    }
}

inline uint32_t ShardManager::route(const std::string& key) const {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).get_shard_for_key_default(key);
    }
}

inline void ShardManager::put(const std::string& key, const std::string& value) {
    const uint32_t sid = this->route(key);
    if (((*this)).shards.contains_key(std::move(sid))) {
        ((*this)).shards.get_mut(std::move(sid)).unwrap().get().put(key, value);
    }
}

inline rusty::Option<std::string> ShardManager::get(const std::string& key) {
    const uint32_t sid = this->route(key);
    if (!((*this)).shards.contains_key(std::move(sid))) {
        return rusty::None;
    }
    return ((*this)).shards.get(sid).unwrap().get().get(key);
}

inline bool ShardManager::add_shard(uint32_t id, const std::vector<std::string>& replicas) {
    bool ok = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).add_shard(std::move(id), replicas);
    if (ok) {
        ((*this)).shards.insert(std::move(id), Shard::new_(std::move(id)));
        this->reload();
    }
    return std::move(ok);
}

inline bool ShardManager::kill_shard(uint32_t dead, uint32_t taker) {
    bool ok = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).kill_shard(std::move(dead), std::move(taker));
    if (ok) {
        rusty::Option<Shard> removed = ((*this)).shards.remove(std::move(dead));
        if (removed.is_some()) {
            Shard dead_shard = removed.unwrap();
            dead_shard.mark_dead();
            if (((*this)).shards.contains_key(std::move(taker))) {
                ((*this)).shards.get_mut(std::move(taker)).unwrap().get().absorb(&dead_shard);
            }
            ((*this)).shards.insert(std::move(dead), std::move(dead_shard));
        }
        this->reload();
    }
    return std::move(ok);
}

inline bool ShardManager::remove_shard(uint32_t id) {
    bool ok = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).remove_shard(std::move(id));
    if (ok) {
        ((*this)).shards.remove(std::move(id));
        this->reload();
    }
    return std::move(ok);
}

inline uint32_t ShardManager::shard_count() const {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).get_shard_count();
    }
}

inline uint64_t ShardManager::epoch() {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cm))).get_epoch();
    }
}

inline bool ShardManager::is_shard_alive(uint32_t id) const {
    if (!((*this)).shards.contains_key(std::move(id))) {
        return false;
    }
    return ((*this)).shards.get(id).unwrap().get().is_alive();
}

inline size_t ShardManager::shard_key_count(uint32_t id) const {
    if (!((*this)).shards.contains_key(std::move(id))) {
        return static_cast<size_t>(0);
    }
    return ((*this)).shards.get(id).unwrap().get().key_count();
}
/*RUSTYCPP:GEN-END id=shard_manager.1*/

}  // namespace janus
