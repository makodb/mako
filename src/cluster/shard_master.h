module;

// ShardMaster — the long-lived control plane that lives on the master shard
// (shard 0) and drives EVERY range migration in the cluster: stub tests and the
// real workload go through the SAME coordinator. It owns the authoritative
// config (janus::ConfigManager over a KvStore) and the routing cache
// (janus::ClusterConfig), and holds a registry of each shard's data plane as a
// borrowed ShardData* — a LOCAL OrderedIndexShardData (masstree/mbta), an
// in-memory InMemoryShardData (tests), or a RemoteShardData RPC proxy (a shard
// in another process). One coordinator, any transport.
//
// A migration is the 2PC of docs/mako-book.md §3 run over two registered
// participants: background_copy (source stays live) -> lock (freeze the range on
// the source) -> final_sync (replay the writes-during-copy delta, then the
// checksum-equality gate = the prepare vote) -> commit (source sheds the range;
// the cutover is PUBLISHED on the real config plane via ConfigManager, which
// bumps the version, so ClusterConfig reroutes the range to the destination on
// its next reload). A per-attempt generation fences stale votes. This folds in
// what was the separate ShardMigrator (real engine) + stub ShardManager: there
// is now one master, and routing overrides live in ConfigManager, not a private
// table.
//
// ConfigManager and ClusterConfig are borrowed (the caller owns them) — the same
// *mut dependency-injection seam ConfigManager itself uses for its KvStore.
//
// Authored in the inline-Rust DSL (docs/storage-interface.md): the
// `#if RUSTYCPP_RUST` block is the source of truth; regenerate with
// scripts/regen_storage_dsl.sh.

#include <string>
#include <vector>
#include <btree_port/btreemap.hpp>
#include <rusty/vec.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like (generated bodies)

export module cluster:shard_master;
import :shard_data;        // ShardData — the participant data-plane port
import :config_manager;    // ConfigManager — authoritative config (the routing plane)
import :cluster_config;    // ClusterConfig — routing cache, reloaded from cm

export namespace janus {

#if RUSTYCPP_RUST
pub struct ShardMaster {
    cm: *mut ConfigManager,     // borrowed: authoritative config (over a KvStore)
    cfg: *mut ClusterConfig,    // borrowed: routing cache, reloaded from cm
    shards: btree_port::BTreeMap<u32, *mut ShardData>,   // each shard's data plane (borrowed)
    // ---- online range-migration state (one migration in flight) ----
    mig_active: bool,                           // a migration is in flight
    mig_source: u32,                            // the migrating range's current owner
    mig_dest: u32,                              // where the range is moving to
    mig_table: std::string,                     // table the range belongs to ("" = table-agnostic)
    mig_lo: std::string,                        // range [lo, hi)
    mig_hi: std::string,
    mig_locked: bool,                           // source's 2PC vote: range frozen (prepared)
    mig_dst_prepared: bool,                     // destination's 2PC vote: caught up (prepared)
    mig_generation: u64,                        // per-attempt id; a stale (old-gen) vote is ignored
    mig_staged: btree_port::BTreeMap<std::string, std::string>,  // writes-during-copy delta (puts)
    mig_deleted: btree_port::BTreeMap<std::string, bool>,        // writes-during-copy delta (deletes)
}
impl ShardMaster {
    fn new(cm: *mut ConfigManager, cfg: *mut ClusterConfig) -> ShardMaster {
        ShardMaster {
            cm: cm,
            cfg: cfg,
            shards: btree_port::BTreeMap::<u32, *mut ShardData>::new_(),
            mig_active: false,
            mig_source: 0,
            mig_dest: 0,
            mig_table: std::string(""),
            mig_lo: std::string(""),
            mig_hi: std::string(""),
            mig_locked: false,
            mig_dst_prepared: false,
            mig_generation: 0,
            mig_staged: btree_port::BTreeMap::<std::string, std::string>::new_(),
            mig_deleted: btree_port::BTreeMap::<std::string, bool>::new_(),
        }
    }
    // Pull the authoritative config into the routing cache (after any reconfig).
    fn reload(&mut self) {
        unsafe { (*(*self).cfg).load_from_config_manager((*self).cm) };
    }
    // Bind shard `id`'s data plane (a borrowed ShardData: a local
    // OrderedIndexShardData, an in-memory fake, or a RemoteShardData proxy).
    fn attach_shard(&mut self, id: u32, data: *mut ShardData) {
        (*self).shards.insert(id, data);
    }
    fn has_shard(&self, id: u32) -> bool {
        (*self).shards.contains_key(id)
    }
    // Register a new shard: the ConfigManager (master) allocates the id, we bind
    // its data plane under that id and reload routing. Returns the id.
    fn register_shard(&mut self, replicas: &std::vector<std::string>, data: *mut ShardData) -> u32 {
        let id: u32 = unsafe { (*(*self).cm).register_shard(replicas) };
        (*self).shards.insert(id, data);
        self.reload();
        id
    }
    // Kill `dead`, handing its routing over to `taker` (config verb + reload so
    // routing chases dead->taker). Data hand-off is a range migration.
    fn kill_shard(&mut self, dead: u32, taker: u32) -> bool {
        let ok: bool = unsafe { (*(*self).cm).kill_shard(dead, taker) };
        if ok {
            (*self).shards.remove(dead);
            self.reload();
        }
        ok
    }
    // Remove a shard entirely (config verb + drop its data plane + reload).
    fn remove_shard(&mut self, id: u32) -> bool {
        let ok: bool = unsafe { (*(*self).cm).remove_shard(id) };
        if ok {
            (*self).shards.remove(id);
            self.reload();
        }
        ok
    }
    // Route a key through the REAL routing plane (ClusterConfig, which honors
    // committed migration overrides after reload). Table-agnostic hash default.
    fn route(&self, key: &std::string) -> u32 {
        unsafe { (*(*self).cfg).get_shard_for_key_default(key) }
    }
    // True while a key's range is frozen by an in-flight LOCK (2PC prepare) —
    // the source has stopped serving it, clients retry.
    fn frozen(&self, key: &std::string) -> bool {
        (*self).mig_active
            && (*self).mig_locked
            && (*key) >= (*self).mig_lo && (*key) < (*self).mig_hi
    }
    // True while a key's range is being background-copied (before LOCK).
    fn copying(&self, key: &std::string) -> bool {
        (*self).mig_active
            && !(*self).mig_locked
            && (*key) >= (*self).mig_lo && (*key) < (*self).mig_hi
    }
    // Client write routed through the master: rejected while the range is frozen;
    // staged into the delta (and applied to the source) while it is being copied;
    // otherwise applied to the routed owner's data plane.
    fn client_put(&mut self, key: &std::string, value: &std::string) {
        if self.frozen(key) { return; }
        if self.copying(key) {
            (*self).mig_staged.insert(key, value);   // stage the put
            (*self).mig_deleted.remove(key);         // ...supersedes a staged delete
            let src_id: u32 = (*self).mig_source;
            if (*self).shards.contains_key(src_id) {
                let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
                unsafe { (*src).put(key, value) };
            }
            return;
        }
        let sid: u32 = self.route(key);
        if (*self).shards.contains_key(sid) {
            let sd: *mut ShardData = (*self).shards.get(sid).unwrap().get();
            unsafe { (*sd).put(key, value) };
        }
    }
    // Client delete routed through the master (same freeze/copy/route rules).
    fn client_remove(&mut self, key: &std::string) {
        if self.frozen(key) { return; }
        if self.copying(key) {
            (*self).mig_deleted.insert(key, true);   // stage the delete
            (*self).mig_staged.remove(key);          // ...supersedes a staged put
            let src_id: u32 = (*self).mig_source;
            if (*self).shards.contains_key(src_id) {
                let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
                unsafe { (*src).remove(key) };
            }
            return;
        }
        let sid: u32 = self.route(key);
        if (*self).shards.contains_key(sid) {
            let sd: *mut ShardData = (*self).shards.get(sid).unwrap().get();
            unsafe { (*sd).remove(key) };
        }
    }
    // ---- online migration protocol (docs/mako-book.md §3) ----------------
    // PREPARE: record the intent for range [lo, hi) source->dest in `table`. The
    // range keeps routing to the source until COMMIT. Each attempt gets a fresh
    // generation so a vote from an earlier attempt is recognized as stale.
    fn begin_migration(&mut self, source: u32, dest: u32, table: &std::string, lo: &std::string, hi: &std::string) -> bool {
        if (*self).mig_active { return false; }
        if !(*self).shards.contains_key(source) { return false; }
        if !(*self).shards.contains_key(dest) { return false; }
        (*self).mig_active = true;
        (*self).mig_source = source;
        (*self).mig_dest = dest;
        (*self).mig_table = (*table);
        (*self).mig_lo = (*lo);
        (*self).mig_hi = (*hi);
        (*self).mig_locked = false;
        (*self).mig_dst_prepared = false;
        (*self).mig_generation = (*self).mig_generation + 1;
        (*self).mig_staged.clear();
        (*self).mig_deleted.clear();
        true
    }
    // Phase 1 — BACKGROUND COPY: snapshot the source's [lo, hi) into the
    // destination, leaving the source intact (it keeps serving). Re-runnable.
    fn background_copy(&mut self) {
        if !(*self).mig_active { return; }
        let src_id: u32 = (*self).mig_source;
        let dst_id: u32 = (*self).mig_dest;
        if !(*self).shards.contains_key(src_id) { return; }
        if !(*self).shards.contains_key(dst_id) { return; }
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
        let dst: *mut ShardData = (*self).shards.get(dst_id).unwrap().get();
        unsafe { (*dst).copy_range_from(src, &lo, &hi) };
    }
    // Phase 2 — LOCK: 2PC prepare on the source. Sets the master's lock flag AND
    // freezes [lo,hi) on the SOURCE participant (its write fence): on a local
    // participant that lands in the process-global MigrationGuard the shard's
    // non-txn write handler enforces (SERVER_BUSY); on a remote participant it is
    // one FreezeRange RPC to the source's process. From here the source takes no
    // writes in the range, so the final copy + checksum see a stable range.
    fn lock_range(&mut self) {
        if !(*self).mig_active { return; }
        (*self).mig_locked = true;
        let src_id: u32 = (*self).mig_source;
        if !(*self).shards.contains_key(src_id) { return; }
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
        unsafe { (*src).freeze_range(&lo, &hi) };
    }
    // True iff source and destination hold identical data over [lo, hi): the
    // source computes its checksum (its "prepared" proof) and the destination
    // verifies its own range against it (one RPC for a remote dst — the dst's
    // rows never leave it, only the bool vote comes back).
    fn range_checksums_match(&self) -> bool {
        if !(*self).mig_active { return false; }
        let src_id: u32 = (*self).mig_source;
        let dst_id: u32 = (*self).mig_dest;
        if !(*self).shards.contains_key(src_id) { return false; }
        if !(*self).shards.contains_key(dst_id) { return false; }
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
        let dst: *mut ShardData = (*self).shards.get(dst_id).unwrap().get();
        let src_ck: u64 = unsafe { (*src).checksum(&lo, &hi) };
        unsafe { (*dst).verify_range(&lo, &hi, src_ck) }
    }
    // Phase 3 — FINAL SYNC: replay the captured delta (puts + deletes) to the
    // destination, then vote prepared ONLY if its range checksum-matches the
    // source; otherwise the copy is corrupt/incomplete and the master aborts.
    fn final_sync(&mut self) {
        if !(*self).mig_active { return; }
        let dst_id: u32 = (*self).mig_dest;
        if !(*self).shards.contains_key(dst_id) { return; }
        let dst: *mut ShardData = (*self).shards.get(dst_id).unwrap().get();
        for kv in (*self).mig_staged {
            unsafe { (*dst).put(kv.first, kv.second) };
        }
        for dk in (*self).mig_deleted {
            unsafe { (*dst).remove(dk.first) };
        }
        if self.range_checksums_match() {
            (*self).mig_dst_prepared = true;
        }
    }
    // A generation-tagged prepare-ack from the destination (2PC vote), accepted
    // only for the CURRENT migration whose range checksum-matches the source.
    fn prepare_dest(&mut self, generation: u64) -> bool {
        if !(*self).mig_active { return false; }
        if generation != (*self).mig_generation { return false; }
        if !self.range_checksums_match() { return false; }
        (*self).mig_dst_prepared = true;
        true
    }
    fn migration_generation(&self) -> u64 {
        (*self).mig_generation
    }
    // Both participants have voted to commit (source frozen + dest caught up).
    fn both_prepared(&self) -> bool {
        (*self).mig_active && (*self).mig_locked && (*self).mig_dst_prepared
    }
    // Phase 4 — COMMIT: only when BOTH have prepared. The source sheds the range,
    // and the cutover is PUBLISHED on the real config plane (ConfigManager bumps
    // the version); ClusterConfig reroutes [lo, hi) to the destination on reload.
    fn commit_migration(&mut self) -> bool {
        if !(*self).mig_active { return false; }
        if !(*self).mig_locked { return false; }
        if !(*self).mig_dst_prepared { return false; }
        let src_id: u32 = (*self).mig_source;
        let dst_id: u32 = (*self).mig_dest;
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        let table: std::string = (*self).mig_table;
        if (*self).shards.contains_key(src_id) {
            let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
            unsafe { (*src).drop_range(&lo, &hi) };
        }
        // Publish the cutover by mutating the authoritative partition table:
        // split at lo/hi and reassign [lo, hi) to the destination (version bumped
        // last). Requires the table's partition to be seeded (map mode).
        unsafe { janus::cm_split_and_reassign((*self).cm, &table, &lo, &hi, dst_id) };
        self.reload();
        // The SOURCE's freeze (set at lock_range) is deliberately NOT lifted: the
        // source no longer owns [lo, hi), and the standing fence keeps rejecting
        // stale-routed writers (SERVER_BUSY -> client retries) until their config
        // reloads and lands them on the destination -- closing the cutover race.
        // The destination was never frozen, so it serves immediately.
        (*self).mig_active = false;
        (*self).mig_staged.clear();
        (*self).mig_deleted.clear();
        true
    }
    // ABORT (before COMMIT): the destination discards its partial copy; the
    // source keeps the range and RESUMES serving it -- its write fence (set at
    // lock_range) is lifted. No config change, so routing never moved.
    fn abort_migration(&mut self) {
        if !(*self).mig_active { return; }
        let dst_id: u32 = (*self).mig_dest;
        let lo: std::string = (*self).mig_lo;
        let hi: std::string = (*self).mig_hi;
        if (*self).shards.contains_key(dst_id) {
            let dst: *mut ShardData = (*self).shards.get(dst_id).unwrap().get();
            unsafe { (*dst).drop_range(&lo, &hi) };
        }
        let src_id: u32 = (*self).mig_source;
        if (*self).mig_locked && (*self).shards.contains_key(src_id) {
            let src: *mut ShardData = (*self).shards.get(src_id).unwrap().get();
            unsafe { (*src).unfreeze_range(&lo, &hi) };
        }
        (*self).mig_active = false;
        (*self).mig_staged.clear();
        (*self).mig_deleted.clear();
    }
    fn is_migrating(&self) -> bool { (*self).mig_active }
    fn migration_locked(&self) -> bool {
        (*self).mig_active && (*self).mig_locked
    }
    // ---- per-shard migration-role queries (computed from the master's state) --
    fn shard_is_migrating(&self, id: u32) -> bool {
        (*self).mig_active && ((*self).mig_source == id || (*self).mig_dest == id)
    }
    fn shard_migration_is_source(&self, id: u32) -> bool {
        (*self).mig_active && (*self).mig_source == id
    }
    fn shard_migration_locked(&self, id: u32) -> bool {
        (*self).mig_active && (*self).mig_locked && (*self).mig_source == id
    }
    // Does shard `id`'s freeze cover `key`? True only for the source's locked
    // migrating range.
    fn shard_frozen_for(&self, id: u32, key: &std::string) -> bool {
        (*self).mig_active
            && (*self).mig_locked
            && (*self).mig_source == id
            && (*key) >= (*self).mig_lo && (*key) < (*self).mig_hi
    }
    // ---- range observability (delegate to a participant's data plane) --------
    fn shard_range_count(&self, id: u32, lo: &std::string, hi: &std::string) -> usize {
        if !(*self).shards.contains_key(id) { return 0; }
        let sd: *mut ShardData = (*self).shards.get(id).unwrap().get();
        unsafe { (*sd).range_count(lo, hi) }
    }
    fn shard_range_checksum(&self, id: u32, lo: &std::string, hi: &std::string) -> u64 {
        if !(*self).shards.contains_key(id) { return 0; }
        let sd: *mut ShardData = (*self).shards.get(id).unwrap().get();
        unsafe { (*sd).checksum(lo, hi) }
    }
    // ---- config observers ----------------------------------------------------
    fn shard_count(&self) -> u32 {
        unsafe { (*(*self).cfg).get_shard_count() }
    }
    fn epoch(&mut self) -> u64 {
        unsafe { (*(*self).cm).get_epoch() }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard_master.1 version=1 rust_sha256=7b5ed1ccb4d140d2f431f44131c984a858d34fefe60da6d657dd4fa409cad634*/
struct ShardMaster;

struct ShardMaster {
    ConfigManager* cm;
    ClusterConfig* cfg;
    btree_port::BTreeMap<uint32_t, ShardData*> shards;
    bool mig_active;
    uint32_t mig_source;
    uint32_t mig_dest;
    std::string mig_table;
    std::string mig_lo;
    std::string mig_hi;
    bool mig_locked;
    bool mig_dst_prepared;
    uint64_t mig_generation;
    btree_port::BTreeMap<std::string, std::string> mig_staged;
    btree_port::BTreeMap<std::string, bool> mig_deleted;

    static ShardMaster new_(ConfigManager* cm, ClusterConfig* cfg);
    void reload();
    void attach_shard(uint32_t id, ShardData* data);
    bool has_shard(uint32_t id) const;
    uint32_t register_shard(const std::vector<std::string>& replicas, ShardData* data);
    bool kill_shard(uint32_t dead, uint32_t taker);
    bool remove_shard(uint32_t id);
    uint32_t route(const std::string& key) const;
    bool frozen(const std::string& key) const;
    bool copying(const std::string& key) const;
    void client_put(const std::string& key, const std::string& value);
    void client_remove(const std::string& key);
    bool begin_migration(uint32_t source, uint32_t dest, const std::string& table, const std::string& lo, const std::string& hi);
    void background_copy();
    void lock_range();
    bool range_checksums_match() const;
    void final_sync();
    bool prepare_dest(uint64_t generation);
    uint64_t migration_generation() const;
    bool both_prepared() const;
    bool commit_migration();
    void abort_migration();
    bool is_migrating() const;
    bool migration_locked() const;
    bool shard_is_migrating(uint32_t id) const;
    bool shard_migration_is_source(uint32_t id) const;
    bool shard_migration_locked(uint32_t id) const;
    bool shard_frozen_for(uint32_t id, const std::string& key) const;
    size_t shard_range_count(uint32_t id, const std::string& lo, const std::string& hi) const;
    uint64_t shard_range_checksum(uint32_t id, const std::string& lo, const std::string& hi) const;
    uint32_t shard_count() const;
    uint64_t epoch();
};


inline ShardMaster ShardMaster::new_(ConfigManager* cm, ClusterConfig* cfg) {
    return ShardMaster{.cm = cm, .cfg = cfg, .shards = btree_port::BTreeMap<uint32_t, ShardData*>::new_(), .mig_active = false, .mig_source = static_cast<uint32_t>(0), .mig_dest = static_cast<uint32_t>(0), .mig_table = std::string(""), .mig_lo = std::string(""), .mig_hi = std::string(""), .mig_locked = false, .mig_dst_prepared = false, .mig_generation = static_cast<uint64_t>(0), .mig_staged = btree_port::BTreeMap<std::string, std::string>::new_(), .mig_deleted = btree_port::BTreeMap<std::string, bool>::new_()};
}

inline void ShardMaster::reload() {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).load_from_config_manager(((*this)).cm);
    }
}

inline void ShardMaster::attach_shard(uint32_t id, ShardData* data) {
    ((*this)).shards.insert(std::move(id), std::move(data));
}

inline bool ShardMaster::has_shard(uint32_t id) const {
    return ((*this)).shards.contains_key(std::move(id));
}

inline uint32_t ShardMaster::register_shard(const std::vector<std::string>& replicas, ShardData* data) {
    uint32_t id = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).register_shard(replicas);
    ((*this)).shards.insert(std::move(id), std::move(data));
    this->reload();
    return std::move(id);
}

inline bool ShardMaster::kill_shard(uint32_t dead, uint32_t taker) {
    bool ok = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).kill_shard(std::move(dead), std::move(taker));
    if (ok) {
        ((*this)).shards.remove(std::move(dead));
        this->reload();
    }
    return std::move(ok);
}

inline bool ShardMaster::remove_shard(uint32_t id) {
    bool ok = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).remove_shard(std::move(id));
    if (ok) {
        ((*this)).shards.remove(std::move(id));
        this->reload();
    }
    return std::move(ok);
}

inline uint32_t ShardMaster::route(const std::string& key) const {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).get_shard_for_key_default(key);
    }
}

inline bool ShardMaster::frozen(const std::string& key) const {
    return ((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}

inline bool ShardMaster::copying(const std::string& key) const {
    return ((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && !((*this)).mig_locked) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}

inline void ShardMaster::client_put(const std::string& key, const std::string& value) {
    if (this->frozen(key)) {
        return;
    }
    if (this->copying(key)) {
        ((*this)).mig_staged.insert(key, std::move(value));
        ((*this)).mig_deleted.remove(key);
        const uint32_t src_id = ((*this)).mig_source;
        if (((*this)).shards.contains_key(std::move(src_id))) {
            ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
            // @unsafe
            {
                ((*src)).put(key, value);
            }
        }
        return;
    }
    const uint32_t sid = this->route(key);
    if (((*this)).shards.contains_key(std::move(sid))) {
        ShardData* const sd = ((*this)).shards.get(std::move(sid)).unwrap().get();
        // @unsafe
        {
            ((*sd)).put(key, value);
        }
    }
}

inline void ShardMaster::client_remove(const std::string& key) {
    if (this->frozen(key)) {
        return;
    }
    if (this->copying(key)) {
        ((*this)).mig_deleted.insert(key, true);
        ((*this)).mig_staged.remove(key);
        const uint32_t src_id = ((*this)).mig_source;
        if (((*this)).shards.contains_key(std::move(src_id))) {
            ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
            // @unsafe
            {
                ((*src)).remove(key);
            }
        }
        return;
    }
    const uint32_t sid = this->route(key);
    if (((*this)).shards.contains_key(std::move(sid))) {
        ShardData* const sd = ((*this)).shards.get(std::move(sid)).unwrap().get();
        // @unsafe
        {
            ((*sd)).remove(key);
        }
    }
}

inline bool ShardMaster::begin_migration(uint32_t source, uint32_t dest, const std::string& table, const std::string& lo, const std::string& hi) {
    if (((*this)).mig_active) {
        return false;
    }
    if (!((*this)).shards.contains_key(std::move(source))) {
        return false;
    }
    if (!((*this)).shards.contains_key(std::move(dest))) {
        return false;
    }
    ((*this)).mig_active = true;
    ((*this)).mig_source = std::move(source);
    ((*this)).mig_dest = std::move(dest);
    ((*this)).mig_table = (table);
    ((*this)).mig_lo = (lo);
    ((*this)).mig_hi = (hi);
    ((*this)).mig_locked = false;
    ((*this)).mig_dst_prepared = false;
    ((*this)).mig_generation = rusty::detail::deref_if_pointer_like(((*this)).mig_generation) + 1;
    ((*this)).mig_staged.clear();
    ((*this)).mig_deleted.clear();
    return true;
}

inline void ShardMaster::background_copy() {
    if (!((*this)).mig_active) {
        return;
    }
    const uint32_t src_id = ((*this)).mig_source;
    const uint32_t dst_id = ((*this)).mig_dest;
    if (!((*this)).shards.contains_key(std::move(src_id))) {
        return;
    }
    if (!((*this)).shards.contains_key(std::move(dst_id))) {
        return;
    }
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
    ShardData* const dst = ((*this)).shards.get(std::move(dst_id)).unwrap().get();
    // @unsafe
    {
        ((*dst)).copy_range_from(src, lo, hi);
    }
}

inline void ShardMaster::lock_range() {
    if (!((*this)).mig_active) {
        return;
    }
    ((*this)).mig_locked = true;
    const uint32_t src_id = ((*this)).mig_source;
    if (!((*this)).shards.contains_key(std::move(src_id))) {
        return;
    }
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
    // @unsafe
    {
        ((*src)).freeze_range(lo, hi);
    }
}

inline bool ShardMaster::range_checksums_match() const {
    if (!((*this)).mig_active) {
        return false;
    }
    const uint32_t src_id = ((*this)).mig_source;
    const uint32_t dst_id = ((*this)).mig_dest;
    if (!((*this)).shards.contains_key(std::move(src_id))) {
        return false;
    }
    if (!((*this)).shards.contains_key(std::move(dst_id))) {
        return false;
    }
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
    ShardData* const dst = ((*this)).shards.get(std::move(dst_id)).unwrap().get();
    const uint64_t src_ck = ((*src)).checksum(lo, hi);
    // @unsafe
    {
        return ((*dst)).verify_range(lo, hi, std::move(src_ck));
    }
}

inline void ShardMaster::final_sync() {
    if (!((*this)).mig_active) {
        return;
    }
    const uint32_t dst_id = ((*this)).mig_dest;
    if (!((*this)).shards.contains_key(std::move(dst_id))) {
        return;
    }
    ShardData* const dst = ((*this)).shards.get(std::move(dst_id)).unwrap().get();
    for (auto&& kv : rusty::for_in(((*this)).mig_staged)) {
        // @unsafe
        {
            ((*dst)).put(std::move(kv.first), std::move(kv.second));
        }
    }
    for (auto&& dk : rusty::for_in(((*this)).mig_deleted)) {
        // @unsafe
        {
            ((*dst)).remove(std::move(dk.first));
        }
    }
    if (this->range_checksums_match()) {
        ((*this)).mig_dst_prepared = true;
    }
}

inline bool ShardMaster::prepare_dest(uint64_t generation) {
    if (!((*this)).mig_active) {
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(generation) != rusty::detail::deref_if_pointer_like(((*this)).mig_generation)) {
        return false;
    }
    if (!this->range_checksums_match()) {
        return false;
    }
    ((*this)).mig_dst_prepared = true;
    return true;
}

inline uint64_t ShardMaster::migration_generation() const {
    return ((*this)).mig_generation;
}

inline bool ShardMaster::both_prepared() const {
    return (rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && rusty::detail::deref_if_pointer_like(((*this)).mig_dst_prepared);
}

inline bool ShardMaster::commit_migration() {
    if (!((*this)).mig_active) {
        return false;
    }
    if (!((*this)).mig_locked) {
        return false;
    }
    if (!((*this)).mig_dst_prepared) {
        return false;
    }
    const uint32_t src_id = ((*this)).mig_source;
    const uint32_t dst_id = ((*this)).mig_dest;
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    const std::string table = ((*this)).mig_table;
    if (((*this)).shards.contains_key(std::move(src_id))) {
        ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
        // @unsafe
        {
            ((*src)).drop_range(lo, hi);
        }
    }
    // @unsafe
    {
        janus::cm_split_and_reassign(((*this)).cm, table, lo, hi, std::move(dst_id));
    }
    this->reload();
    ((*this)).mig_active = false;
    ((*this)).mig_staged.clear();
    ((*this)).mig_deleted.clear();
    return true;
}

inline void ShardMaster::abort_migration() {
    if (!((*this)).mig_active) {
        return;
    }
    const uint32_t dst_id = ((*this)).mig_dest;
    const std::string lo = ((*this)).mig_lo;
    const std::string hi = ((*this)).mig_hi;
    if (((*this)).shards.contains_key(std::move(dst_id))) {
        ShardData* const dst = ((*this)).shards.get(std::move(dst_id)).unwrap().get();
        // @unsafe
        {
            ((*dst)).drop_range(lo, hi);
        }
    }
    const uint32_t src_id = ((*this)).mig_source;
    if (rusty::detail::deref_if_pointer_like(((*this)).mig_locked) && ((*this)).shards.contains_key(std::move(src_id))) {
        ShardData* const src = ((*this)).shards.get(std::move(src_id)).unwrap().get();
        // @unsafe
        {
            ((*src)).unfreeze_range(lo, hi);
        }
    }
    ((*this)).mig_active = false;
    ((*this)).mig_staged.clear();
    ((*this)).mig_deleted.clear();
}

inline bool ShardMaster::is_migrating() const {
    return ((*this)).mig_active;
}

inline bool ShardMaster::migration_locked() const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked);
}

inline bool ShardMaster::shard_is_migrating(uint32_t id) const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && (((rusty::detail::deref_if_pointer_like(((*this)).mig_source) == rusty::detail::deref_if_pointer_like(id)) || (rusty::detail::deref_if_pointer_like(((*this)).mig_dest) == rusty::detail::deref_if_pointer_like(id))));
}

inline bool ShardMaster::shard_migration_is_source(uint32_t id) const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && (rusty::detail::deref_if_pointer_like(((*this)).mig_source) == rusty::detail::deref_if_pointer_like(id));
}

inline bool ShardMaster::shard_migration_locked(uint32_t id) const {
    return (rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && (rusty::detail::deref_if_pointer_like(((*this)).mig_source) == rusty::detail::deref_if_pointer_like(id));
}

inline bool ShardMaster::shard_frozen_for(uint32_t id, const std::string& key) const {
    return (((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && (rusty::detail::deref_if_pointer_like(((*this)).mig_source) == rusty::detail::deref_if_pointer_like(id))) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}

inline size_t ShardMaster::shard_range_count(uint32_t id, const std::string& lo, const std::string& hi) const {
    if (!((*this)).shards.contains_key(std::move(id))) {
        return static_cast<size_t>(0);
    }
    ShardData* const sd = ((*this)).shards.get(std::move(id)).unwrap().get();
    // @unsafe
    {
        return ((*sd)).range_count(lo, hi);
    }
}

inline uint64_t ShardMaster::shard_range_checksum(uint32_t id, const std::string& lo, const std::string& hi) const {
    if (!((*this)).shards.contains_key(std::move(id))) {
        return static_cast<uint64_t>(0);
    }
    ShardData* const sd = ((*this)).shards.get(std::move(id)).unwrap().get();
    // @unsafe
    {
        return ((*sd)).checksum(lo, hi);
    }
}

inline uint32_t ShardMaster::shard_count() const {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cfg))).get_shard_count();
    }
}

inline uint64_t ShardMaster::epoch() {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(((*this)).cm))).get_epoch();
    }
}
/*RUSTYCPP:GEN-END id=shard_master.1*/

}  // namespace janus
