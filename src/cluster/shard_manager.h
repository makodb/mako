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
#include <rusty/vec.hpp>      // committed range->owner override list
#include <rusty/option.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like (generated bodies)

namespace janus {

#if RUSTYCPP_RUST
// A committed range->shard override: once a migration commits, keys in [lo, hi)
// route to `owner`, overriding the hash default.
pub struct RangeOwner {
    lo: std::string,
    hi: std::string,
    owner: u32,
}
impl RangeOwner {
    fn make(lo: std::string, hi: std::string, owner: u32) -> RangeOwner {
        RangeOwner { lo: lo, hi: hi, owner: owner }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard_manager.1 version=1 rust_sha256=4134593613ecca7947fe7ca556354aee9238782fbc815a8d3c659e3665335dda*/
struct RangeOwner;

struct RangeOwner {
    std::string lo;
    std::string hi;
    uint32_t owner;

    static RangeOwner make(std::string lo, std::string hi, uint32_t owner);
};


inline RangeOwner RangeOwner::make(std::string lo, std::string hi, uint32_t owner) {
    return RangeOwner{.lo = std::move(lo), .hi = std::move(hi), .owner = std::move(owner)};
}
/*RUSTYCPP:GEN-END id=shard_manager.1*/

#if RUSTYCPP_RUST
pub struct ShardManager {
    cm: *mut ConfigManager,     // borrowed: authoritative config (over a KvStore)
    cfg: *mut ClusterConfig,    // borrowed: routing cache, reloaded from cm
    shards: btree_port::BTreeMap<u32, Shard>,   // stub data shards by id
    // ---- online range-migration state ----
    migrated: rusty::Vec<RangeOwner>,           // committed [lo,hi)->owner overrides
    mig_active: bool,                           // a migration is in flight
    mig_source: u32,                            // the migrating range's current owner
    mig_dest: u32,                              // where the range is moving to
    mig_lo: std::string,                        // range [lo, hi)
    mig_hi: std::string,
    mig_locked: bool,                           // false = Copying phase, true = Locked (2PC freeze)
    mig_staged: btree_port::BTreeMap<std::string, std::string>,  // writes-during-copy delta
}
impl ShardManager {
    fn new(cm: *mut ConfigManager, cfg: *mut ClusterConfig) -> ShardManager {
        ShardManager {
            cm: cm,
            cfg: cfg,
            shards: btree_port::BTreeMap::<u32, Shard>::new_(),
            migrated: rusty::Vec::<RangeOwner>::new_(),
            mig_active: false,
            mig_source: 0,
            mig_dest: 0,
            mig_lo: std::string(""),
            mig_hi: std::string(""),
            mig_locked: false,
            mig_staged: btree_port::BTreeMap::<std::string, std::string>::new_(),
        }
    }
    // Pull the authoritative config into the routing cache.
    fn reload(&mut self) {
        unsafe { (*(*self).cfg).load_from_config_manager((*self).cm) };
    }
    // Route a key to its owning shard. Precedence: (1) an in-flight migration
    // keeps its range on the source until COMMIT; (2) a committed range
    // override points at the destination; (3) the hash-mod default (+ dead
    // shard follow) otherwise.
    fn route(&self, key: &std::string) -> u32 {
        let mut i: usize = 0;
        while i < (*self).migrated.size() {
            if (*key) >= (*self).migrated[i].lo && (*key) < (*self).migrated[i].hi {
                return (*self).migrated[i].owner;
            }
            i = i + 1;
        }
        unsafe { (*(*self).cfg).get_shard_for_key_default(key) }
    }
    // Set (or update) the owner of range [lo, hi) in the override table.
    fn set_range_owner(&mut self, lo: &std::string, hi: &std::string, owner: u32) {
        let mut i: usize = 0;
        while i < (*self).migrated.size() {
            if (*self).migrated[i].lo == (*lo) && (*self).migrated[i].hi == (*hi) {
                (*self).migrated[i].owner = owner;
                return;
            }
            i = i + 1;
        }
        (*self).migrated.push(RangeOwner::make((*lo), (*hi), owner));
    }
    // True while a key's range is frozen by an in-flight LOCK (2PC prepare).
    fn frozen(&self, key: &std::string) -> bool {
        (*self).mig_active
            && (*self).mig_locked
            && (*key) >= (*self).mig_lo && (*key) < (*self).mig_hi
    }
    // True while a key's range is being background-copied (Copying phase).
    fn copying(&self, key: &std::string) -> bool {
        (*self).mig_active
            && !(*self).mig_locked
            && (*key) >= (*self).mig_lo && (*key) < (*self).mig_hi
    }
    // Client write: rejected while the range is frozen; during a background
    // copy the write also lands in the delta so FINAL SYNC carries it across.
    fn put(&mut self, key: &std::string, value: &std::string) {
        if self.frozen(key) { return; }
        if self.copying(key) {
            (*self).mig_staged.insert(key, value);
        }
        let sid: u32 = self.route(key);
        if (*self).shards.contains_key(sid) {
            (*self).shards.get_mut(sid).unwrap().get().put(key, value);
        }
    }
    // Client read: None while the range is frozen; else serve from the owner.
    fn get(&mut self, key: &std::string) -> rusty::Option<std::string> {
        if self.frozen(key) { return rusty::None; }
        let sid: u32 = self.route(key);
        if !(*self).shards.contains_key(sid) {
            return rusty::None;
        }
        (*self).shards.get(sid).unwrap().get().get(key)
    }
    // Test seam: write straight to a shard, bypassing routing (used to place a
    // range's initial data on the source before a migration begins).
    fn put_direct(&mut self, shard_id: u32, key: &std::string, value: &std::string) {
        if (*self).shards.contains_key(shard_id) {
            (*self).shards.get_mut(shard_id).unwrap().get().put(key, value);
        }
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
    // ---- online migration protocol (docs/mako-book.md) ------------------
    // PREPARE: record the intent for range [lo, hi) source->dest. The range
    // keeps routing to the source (Copying) until COMMIT.
    fn begin_migration(&mut self, source: u32, dest: u32, lo: &std::string, hi: &std::string) -> bool {
        if (*self).mig_active { return false; }
        (*self).mig_active = true;
        (*self).mig_source = source;
        (*self).mig_dest = dest;
        (*self).mig_lo = (*lo);
        (*self).mig_hi = (*hi);
        (*self).mig_locked = false;
        (*self).mig_staged.clear();
        // Pin the range to the source for the migration's duration (and if we
        // abort); COMMIT flips this override to the destination.
        self.set_range_owner(lo, hi, source);
        true
    }
    // BACKGROUND COPY: snapshot the source's [lo, hi) into the destination,
    // leaving the source intact (it keeps serving). Idempotent / re-runnable.
    fn background_copy(&mut self) {
        if !(*self).mig_active { return; }
        let src_id: u32 = (*self).mig_source;
        let dst_id: u32 = (*self).mig_dest;
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        let removed: rusty::Option<Shard> = (*self).shards.remove(src_id);
        if removed.is_some() {
            let mut src: Shard = removed.unwrap();
            if (*self).shards.contains_key(dst_id) {
                (*self).shards.get_mut(dst_id).unwrap().get().copy_range_from(&mut src, &lo, &hi);
            }
            (*self).shards.insert(src_id, src);
        }
    }
    // LOCK: 2PC prepare -- freeze the range (the source stops serving it).
    fn lock_range(&mut self) {
        if (*self).mig_active {
            (*self).mig_locked = true;
        }
    }
    // FINAL SYNC: apply the captured delta (writes during copy) to the dest.
    fn final_sync(&mut self) {
        if !(*self).mig_active { return; }
        let dst_id: u32 = (*self).mig_dest;
        if (*self).shards.contains_key(dst_id) {
            for kv in (*self).mig_staged {
                (*self).shards.get_mut(dst_id).unwrap().get().put(kv.first, kv.second);
            }
        }
    }
    // COMMIT: source sheds the range, record the routing override so the range
    // now points at the destination, clear migration state. The destination
    // serves the range from here on.
    fn commit_migration(&mut self) -> bool {
        if !(*self).mig_active { return false; }
        let src_id: u32 = (*self).mig_source;
        let dst_id: u32 = (*self).mig_dest;
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        if (*self).shards.contains_key(src_id) {
            (*self).shards.get_mut(src_id).unwrap().get().drop_range(&lo, &hi);
        }
        self.set_range_owner(&lo, &hi, dst_id);
        (*self).mig_active = false;
        (*self).mig_staged.clear();
        true
    }
    // ABORT (before COMMIT): the dest discards its partial copy, the source
    // resumes serving (it never lost data), migration state clears.
    fn abort_migration(&mut self) {
        if !(*self).mig_active { return; }
        let dst_id: u32 = (*self).mig_dest;
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        if (*self).shards.contains_key(dst_id) {
            (*self).shards.get_mut(dst_id).unwrap().get().drop_range(&lo, &hi);
        }
        (*self).mig_active = false;
        (*self).mig_staged.clear();
    }
    fn is_migrating(&self) -> bool { (*self).mig_active }
    fn migration_locked(&self) -> bool {
        (*self).mig_active && (*self).mig_locked
    }
    // Keys shard `id` holds in [lo, hi) (test observability).
    fn range_key_count(&self, id: u32, lo: &std::string, hi: &std::string) -> usize {
        if !(*self).shards.contains_key(id) {
            return 0;
        }
        (*self).shards.get(id).unwrap().get().range_count(lo, hi)
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
/*RUSTYCPP:GEN-BEGIN id=shard_manager.2 version=1 rust_sha256=f9540be7f88201b586c613a620fea9e7f4f24b586096f882751571e6647fb20d*/
struct ShardManager;

struct ShardManager {
    ConfigManager* cm;
    ClusterConfig* cfg;
    btree_port::BTreeMap<uint32_t, Shard> shards;
    rusty::Vec<RangeOwner> migrated;
    bool mig_active;
    uint32_t mig_source;
    uint32_t mig_dest;
    std::string mig_lo;
    std::string mig_hi;
    bool mig_locked;
    btree_port::BTreeMap<std::string, std::string> mig_staged;

    static ShardManager new_(ConfigManager* cm, ClusterConfig* cfg);
    void reload();
    uint32_t route(const std::string& key) const;
    void set_range_owner(const std::string& lo, const std::string& hi, uint32_t owner);
    bool frozen(const std::string& key) const;
    bool copying(const std::string& key) const;
    void put(const std::string& key, const std::string& value);
    rusty::Option<std::string> get(const std::string& key);
    void put_direct(uint32_t shard_id, const std::string& key, const std::string& value);
    bool add_shard(uint32_t id, const std::vector<std::string>& replicas);
    bool kill_shard(uint32_t dead, uint32_t taker);
    bool remove_shard(uint32_t id);
    bool begin_migration(uint32_t source, uint32_t dest, const std::string& lo, const std::string& hi);
    void background_copy();
    void lock_range();
    void final_sync();
    bool commit_migration();
    void abort_migration();
    bool is_migrating() const;
    bool migration_locked() const;
    size_t range_key_count(uint32_t id, const std::string& lo, const std::string& hi) const;
    uint32_t shard_count() const;
    uint64_t epoch();
    bool is_shard_alive(uint32_t id) const;
    size_t shard_key_count(uint32_t id) const;
};


inline ShardManager ShardManager::new_(ConfigManager* cm, ClusterConfig* cfg) {
    return ShardManager{.cm = cm, .cfg = cfg, .shards = btree_port::BTreeMap<uint32_t, Shard>::new_(), .migrated = rusty::Vec<RangeOwner>::new_(), .mig_active = false, .mig_source = static_cast<uint32_t>(0), .mig_dest = static_cast<uint32_t>(0), .mig_lo = std::string(""), .mig_hi = std::string(""), .mig_locked = false, .mig_staged = btree_port::BTreeMap<std::string, std::string>::new_()};
}

inline void ShardManager::reload() {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).load_from_config_manager(((*this)).cm);
    }
}

inline uint32_t ShardManager::route(const std::string& key) const {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).migrated.size()) {
        if (((key) >= rusty::detail::deref_if_pointer_like(((*this)).migrated[i].lo)) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).migrated[i].hi))) {
            return ((*this)).migrated[i].owner;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).get_shard_for_key_default(key);
    }
}

inline void ShardManager::set_range_owner(const std::string& lo, const std::string& hi, uint32_t owner) {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).migrated.size()) {
        if ((rusty::detail::deref_if_pointer_like(((*this)).migrated[i].lo) == (lo)) && (rusty::detail::deref_if_pointer_like(((*this)).migrated[i].hi) == (hi))) {
            ((*this)).migrated[i].owner = std::move(owner);
            return;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    ((*this)).migrated.push(RangeOwner::make((lo), (hi), std::move(owner)));
}

inline bool ShardManager::frozen(const std::string& key) const {
    return ((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}

inline bool ShardManager::copying(const std::string& key) const {
    return ((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && !((*this)).mig_locked) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}

inline void ShardManager::put(const std::string& key, const std::string& value) {
    if (this->frozen(key)) {
        return;
    }
    if (this->copying(key)) {
        ((*this)).mig_staged.insert(key, std::move(value));
    }
    const uint32_t sid = this->route(key);
    if (((*this)).shards.contains_key(std::move(sid))) {
        ((*this)).shards.get_mut(std::move(sid)).unwrap().get().put(key, value);
    }
}

inline rusty::Option<std::string> ShardManager::get(const std::string& key) {
    if (this->frozen(key)) {
        return rusty::None;
    }
    const uint32_t sid = this->route(key);
    if (!((*this)).shards.contains_key(std::move(sid))) {
        return rusty::None;
    }
    return ((*this)).shards.get(sid).unwrap().get().get(key);
}

inline void ShardManager::put_direct(uint32_t shard_id, const std::string& key, const std::string& value) {
    if (((*this)).shards.contains_key(std::move(shard_id))) {
        ((*this)).shards.get_mut(std::move(shard_id)).unwrap().get().put(key, value);
    }
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

inline bool ShardManager::begin_migration(uint32_t source, uint32_t dest, const std::string& lo, const std::string& hi) {
    if (((*this)).mig_active) {
        return false;
    }
    ((*this)).mig_active = true;
    ((*this)).mig_source = std::move(source);
    ((*this)).mig_dest = std::move(dest);
    ((*this)).mig_lo = (lo);
    ((*this)).mig_hi = (hi);
    ((*this)).mig_locked = false;
    ((*this)).mig_staged.clear();
    this->set_range_owner(lo, hi, std::move(source));
    return true;
}

inline void ShardManager::background_copy() {
    if (!((*this)).mig_active) {
        return;
    }
    const uint32_t src_id = ((*this)).mig_source;
    const uint32_t dst_id = ((*this)).mig_dest;
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    rusty::Option<Shard> removed = ((*this)).shards.remove(std::move(src_id));
    if (removed.is_some()) {
        Shard src = removed.unwrap();
        if (((*this)).shards.contains_key(std::move(dst_id))) {
            ((*this)).shards.get_mut(std::move(dst_id)).unwrap().get().copy_range_from(&src, lo, hi);
        }
        ((*this)).shards.insert(std::move(src_id), std::move(src));
    }
}

inline void ShardManager::lock_range() {
    if (((*this)).mig_active) {
        ((*this)).mig_locked = true;
    }
}

inline void ShardManager::final_sync() {
    if (!((*this)).mig_active) {
        return;
    }
    const uint32_t dst_id = ((*this)).mig_dest;
    if (((*this)).shards.contains_key(std::move(dst_id))) {
        for (auto&& kv : rusty::for_in(((*this)).mig_staged)) {
            ((*this)).shards.get_mut(std::move(dst_id)).unwrap().get().put(kv.first, kv.second);
        }
    }
}

inline bool ShardManager::commit_migration() {
    if (!((*this)).mig_active) {
        return false;
    }
    const uint32_t src_id = ((*this)).mig_source;
    uint32_t dst_id = ((*this)).mig_dest;
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    if (((*this)).shards.contains_key(std::move(src_id))) {
        ((*this)).shards.get_mut(std::move(src_id)).unwrap().get().drop_range(lo, hi);
    }
    this->set_range_owner(lo, hi, std::move(dst_id));
    ((*this)).mig_active = false;
    ((*this)).mig_staged.clear();
    return true;
}

inline void ShardManager::abort_migration() {
    if (!((*this)).mig_active) {
        return;
    }
    const uint32_t dst_id = ((*this)).mig_dest;
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    if (((*this)).shards.contains_key(std::move(dst_id))) {
        ((*this)).shards.get_mut(std::move(dst_id)).unwrap().get().drop_range(lo, hi);
    }
    ((*this)).mig_active = false;
    ((*this)).mig_staged.clear();
}

inline bool ShardManager::is_migrating() const {
    return ((*this)).mig_active;
}

inline bool ShardManager::migration_locked() const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked);
}

inline size_t ShardManager::range_key_count(uint32_t id, const std::string& lo, const std::string& hi) const {
    if (!((*this)).shards.contains_key(std::move(id))) {
        return static_cast<size_t>(0);
    }
    return ((*this)).shards.get(id).unwrap().get().range_count(lo, hi);
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
/*RUSTYCPP:GEN-END id=shard_manager.2*/

}  // namespace janus
